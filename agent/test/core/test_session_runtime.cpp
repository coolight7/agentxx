// 统一启动壳与会话输入解析测试
//
// 覆盖:
// - `parseUserInputJson` 的 8 类边界 (它把 FFI 里的输入解析提出来, CLI/FFI/插件
//   共用同一套规则, 改动会同时影响三方, 因此单独覆盖)
// - `AgentSessionRuntime` 的装配: 缺参数拒绝、start 幂等、init 后驱动循环起得来、
//   stop 后 running 归零、init 失败时上报并退出
// - 已初始化 (skipInit) 的 agent: 投递一条输入 → 收到增量与轮次结束 → 会话末尾是
//   模型最终回答 (插件通道就按这条路走)
#include "agentxx-test/core/test_session_runtime.h"
#include "agentxx-test/core/fake_provider.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/embedded_client_agent_io.h"
#include "agentxx/agent/io/user_input_json.h"
#include "agentxx/agent/session_runtime.h"
#include "asio/executor_work_guard.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_srt_passed = 0;
int g_srt_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_srt_passed
#define XX_TEST_FAILED g_srt_failed

namespace agentxx {
namespace test {

namespace {

using agentxx::agent::AgentConfig;
using agentxx::agent::AgentSessionRuntime;
using agentxx::agent::EmbeddedClientAgentIO;

constexpr std::string_view kRtModelName = "test-rt-fake";

/// 测试用客户端端点: 记录收到的正文增量与轮次结果
///
/// 同时充当 [EmbeddedClientAgentIO] 的第一处使用示例 (FFI 端点之外): 只实现
/// "事件怎么交出去" 两个回调即可, 中断应答与增量合批都由基类提供。
class RecordingClientIO : public EmbeddedClientAgentIO,
                          public std::enable_shared_from_this<RecordingClientIO> {
public:

    explicit RecordingClientIO(asio::any_io_executor ex) :
        EmbeddedClientAgentIO(std::move(ex)) {}

    /// 轮次结果计数 (原子: 用例协程与 io 线程各读一次)
    std::atomic<int> turnResults{0};

    std::string textSnapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return text_;
    }

protected:

    void emitDeltaText(std::string_view kind, std::string_view chunk) override {
        if (kind != "text") {
            return;
        }
        std::lock_guard<std::mutex> lock(mu_);
        text_.append(chunk);
    }

    void emitDeltaEvent(const agentxx::agent::WireDelta& delta) override {
        if (delta.type != agentxx::agent::WireDelta::Type::TextToken) {
            return;
        }
        std::lock_guard<std::mutex> lock(mu_);
        text_.append(delta.text);
    }

    void onTurnResult(const agentxx::agent::WireTurnResult& /*result*/) override {
        turnResults.fetch_add(1);
    }

private:

    mutable std::mutex mu_;
    std::string        text_;
};

/// 组装一份能过启动校验的配置 (假 provider 不打网络, 地址只为通过校验)
std::shared_ptr<AgentConfig> makeRtConfig() {
    auto cfg                 = std::make_shared<AgentConfig>();
    cfg->model.baseUrl       = "http://127.0.0.1:1";
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = std::string{kRtModelName};
    cfg->prompt.systemPrompt = "You are a helpful assistant.";
    cfg->llmMaxRetry         = 0;
    // 不落盘: 用例不碰用户数据目录
    cfg->enableSessionStore = false;
    return cfg;
}

/// 等待条件成立 (轮转调用方 io 线程; 不用阻塞 sleep)
asio::awaitable<bool> waitUntil(std::function<bool()> cond, int timeoutMs = 20000, int pollMs = 15) {
    auto ex       = co_await asio::this_coro::executor;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) {
            co_return true;
        }
        asio::steady_timer t(ex);
        t.expires_after(std::chrono::milliseconds{pollMs});
        utilxx_base::AsioErrorCode ec;
        co_await t.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
    co_return cond();
}

/// 会话最后一条 assistant 消息正文 (无则空)
std::string lastAssistantContent(const std::shared_ptr<agentxx::agent::Session>& session) {
    if (!session) {
        return {};
    }
    const auto& msgs = session->messages();
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
        if (it->role == "assistant") {
            return it->content;
        }
    }
    return {};
}

/// 工具初始化必失败的 agent (模拟 init 报错; 构造期模型配置仍然合法)
class BrokenToolsAgent : public agentxx::agent::CodeAgent {
public:

    using CodeAgent::CodeAgent;

protected:

    asio::awaitable<std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>> initTools() override {
        throw std::runtime_error{"broken tools for init failure test"};
    }
};

/// 会话期测试台: 给 agent 自己的 io_context 一条线程 (启动壳的 agent 侧协程在上面跑)
///
/// 本地直连模式里这条线程由 `runLocalUnifiedMain` 提供, FFI 里是 Server-IO 线程;
/// 用例必须自己给它一条线程, 否则 `run()` 永远不会开始跑。
class RuntimeHarness {
public:

    RuntimeHarness(std::shared_ptr<agentxx::agent::BaseAgent> agent) :
        agent_(std::move(agent)) {
        work_.emplace(asio::make_work_guard(*agent_->ioCtx));
        thread_ = std::thread([io = agent_->ioCtx]() {
            io->run();
        });
    }

    ~RuntimeHarness() {
        shutdown();
    }

    RuntimeHarness(const RuntimeHarness&)            = delete;
    RuntimeHarness& operator=(const RuntimeHarness&) = delete;

    asio::any_io_executor executor() const {
        return agent_->ioCtx->get_executor();
    }

    /// 两端都用 agent 的 io 线程 (单线程交错, 与 FFI 的两线程结构等价)
    void shutdown() {
        if (thread_.joinable()) {
            work_.reset();
            agent_->ioCtx->stop();
            thread_.join();
        }
    }

private:

    std::shared_ptr<agentxx::agent::BaseAgent>                                      agent_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>>       work_;
    std::thread                                                                     thread_;
};

// ---------------------------------------------------------------------------
// 输入 JSON 解析: 8 类边界
// ---------------------------------------------------------------------------
void testParseUserInputJson() {
    using agentxx::agent::parseUserInputJson;

    // 1) 空串: 没有内容
    XX_TEST_EXPECT_FALSE(parseUserInputJson("").has_value());

    // 2) 非法 JSON 文本: 标记为语法错误 (调用方据此回"格式错误"类错误码)
    {
        auto r = parseUserInputJson("{not json");
        XX_TEST_EXPECT_FALSE(r.has_value());
        XX_TEST_EXPECT_TRUE(r.error().badJson);
    }

    // 3) 合法 JSON 但不是对象
    {
        auto r = parseUserInputJson("[1,2,3]");
        XX_TEST_EXPECT_FALSE(r.has_value());
        XX_TEST_EXPECT_FALSE(r.error().badJson);
        auto r2 = parseUserInputJson("\"text\"");
        XX_TEST_EXPECT_FALSE(r2.has_value());
        XX_TEST_EXPECT_FALSE(r2.error().badJson);
    }

    // 4) 空内容: text 与 attachments 都为空
    XX_TEST_EXPECT_FALSE(parseUserInputJson(R"({"text":""})").has_value());
    XX_TEST_EXPECT_FALSE(parseUserInputJson(R"({})").has_value());
    XX_TEST_EXPECT_FALSE(parseUserInputJson(R"({"attachments":[]})").has_value());

    // 5) attachments 形态: 非数组 (且非 null) 拒绝; null 视为没有附件
    XX_TEST_EXPECT_FALSE(parseUserInputJson(R"({"text":"hi","attachments":"a.png"})").has_value());
    {
        auto r = parseUserInputJson(R"({"text":"hi","attachments":null})");
        XX_TEST_EXPECT_TRUE(r.has_value());
        if (r) {
            XX_TEST_EXPECT_EQ(r->text, std::string{"hi"});
            XX_TEST_EXPECT_TRUE(r->attachments.empty());
        }
    }

    // 6) 附件条数上限 (单条消息 kMaxAttachmentsPerMessage)
    {
        std::string arr;
        for (size_t i = 0; i <= agentxx::agent::kMaxAttachmentsPerMessage; ++i) {
            if (!arr.empty()) {
                arr += ",";
            }
            arr += fmt::format(R"({{"type":"image","pathOrUrl":"D:/t{}.png"}})", i);
        }
        auto r = parseUserInputJson(fmt::format(R"({{"attachments":[{}]}})", arr));
        XX_TEST_EXPECT_FALSE(r.has_value());
        XX_TEST_EXPECT_FALSE(r.error().badJson);
        XX_TEST_EXPECT_TRUE(r.error().message.find("附件条数") != std::string::npos);
    }

    // 7) 单个附件超限 (显式 sizeBytes 超过该类型上限)
    {
        auto r = parseUserInputJson(
            R"({"attachments":[{"type":"image","displayName":"big.png",)"
            R"("sizeBytes":20971520}]})"
        );
        XX_TEST_EXPECT_FALSE(r.has_value());
        XX_TEST_EXPECT_FALSE(r.error().badJson);
        XX_TEST_EXPECT_TRUE(r.error().message.find("big.png") != std::string::npos);
    }

    // 8) 正常取值: 正文 + 模型 + 投递模式 + 单个附件全部落到 wire 输入
    {
        auto r = parseUserInputJson(
            R"({"text":"你好","model":"gpt-4o","delivery":"next-step",)"
            R"("attachments":[{"type":"image","displayName":"a.png","pathOrUrl":"D:/a.png"}]})"
        );
        XX_TEST_EXPECT_TRUE(r.has_value());
        if (r) {
            XX_TEST_EXPECT_EQ(r->text, std::string{"你好"});
            XX_TEST_EXPECT_EQ(r->model, std::string{"gpt-4o"});
            XX_TEST_EXPECT_EQ(r->delivery, std::string{"next-step"});
            XX_TEST_EXPECT_EQ(r->attachments.size(), size_t{1});
            // 会话与请求序号由调用方补 (解析函数不决定"这条输入属于谁")
            XX_TEST_EXPECT_TRUE(r->sessionId.empty());
            XX_TEST_EXPECT_EQ(r->requestId, uint64_t{0});
        }
    }

    // 再补一条"只给附件不给正文": 合法
    {
        auto r = parseUserInputJson(
            R"({"attachments":[{"type":"image","pathOrUrl":"D:/only.png"}]})"
        );
        XX_TEST_EXPECT_TRUE(r.has_value());
        if (r) {
            XX_TEST_EXPECT_TRUE(r->text.empty());
            XX_TEST_EXPECT_EQ(r->attachments.size(), size_t{1});
        }
    }
}

// ---------------------------------------------------------------------------
// 装配: 缺参数拒绝 + start 幂等 + init 后能跑起来 + stop 后退出 + 失败上报
// ---------------------------------------------------------------------------
asio::awaitable<void> testRuntimeLifecycle() {
    auto ex = co_await asio::this_coro::executor;

    // 1) 缺参数: 没有 agent / 没有客户端端点 / 没有执行器 / 没有会话 id 都装配失败
    {
        auto                               agent = std::make_shared<agentxx::agent::CodeAgent>(makeRtConfig());
        auto                               io    = std::make_shared<RecordingClientIO>(ex);
        agent::AgentSessionRuntime::Config cfg;
        cfg.sessionId      = "rt-lack-args";
        cfg.clientExecutor = ex;
        XX_TEST_EXPECT_TRUE(AgentSessionRuntime::create(nullptr, io, cfg) == nullptr);
        XX_TEST_EXPECT_TRUE(AgentSessionRuntime::create(agent, nullptr, cfg) == nullptr);
        auto noEx           = cfg;
        noEx.clientExecutor = asio::any_io_executor{};
        XX_TEST_EXPECT_TRUE(AgentSessionRuntime::create(agent, io, noEx) == nullptr);
        auto noSid = cfg;
        noSid.sessionId.clear();
        XX_TEST_EXPECT_TRUE(AgentSessionRuntime::create(agent, io, noSid) == nullptr);
    }

    // 2) 正常装配: start 幂等, init 完成后驱动循环起来, stop 后 running 归零
    {
        auto           agent   = std::make_shared<agentxx::agent::CodeAgent>(makeRtConfig());
        RuntimeHarness harness{agent};
        auto           io = std::make_shared<RecordingClientIO>(harness.executor());
        io->initSelf(io);

        agent::AgentSessionRuntime::Config cfg;
        cfg.sessionId      = "rt-lifecycle";
        cfg.clientExecutor = harness.executor();
        auto runtime       = AgentSessionRuntime::create(agent, io, cfg);
        XX_TEST_EXPECT_TRUE(runtime != nullptr);
        if (!runtime) {
            co_return;
        }
        runtime->start();
        runtime->start(); // 幂等: 第二次不应再起一套协程

        const bool started = co_await waitUntil([&]() {
            return runtime->running();
        });
        XX_TEST_EXPECT_TRUE(started);
        XX_TEST_EXPECT_FALSE(runtime->failed());
        XX_TEST_EXPECT_TRUE(runtime->serverIO() != nullptr);
        XX_TEST_EXPECT_TRUE(runtime->host() != nullptr);
        XX_TEST_EXPECT_EQ(std::string{runtime->sessionId()}, std::string{"rt-lifecycle"});

        runtime->stop();
        const bool stopped = co_await waitUntil([&]() {
            return !runtime->running();
        });
        XX_TEST_EXPECT_TRUE(stopped);
        XX_TEST_EXPECT_TRUE(runtime->stopping());
        // 关掉插件挂载 (只停驱动循环不会拆挂载; 不关会留一条析构警告)
        co_await agent->shutdownAsync(std::chrono::seconds{10});
        harness.shutdown();
    }

    // 3) init 失败: 失败标记置位, 错误回调收到一次, 驱动循环随之退出
    //    (构造期只要求模型配置合法, 这里让工具初始化抛错, 才是真的 init 失败)
    {
        auto           agent = std::make_shared<BrokenToolsAgent>(makeRtConfig());
        RuntimeHarness harness{agent};
        auto           io = std::make_shared<RecordingClientIO>(harness.executor());
        io->initSelf(io);

        std::atomic<int>                   initErrors{0};
        agent::AgentSessionRuntime::Config cfg;
        cfg.sessionId      = "rt-init-fail";
        cfg.clientExecutor = harness.executor();
        cfg.onInitError    = [&initErrors](std::string_view) {
            initErrors.fetch_add(1);
        };
        auto runtime = AgentSessionRuntime::create(agent, io, std::move(cfg));
        XX_TEST_EXPECT_TRUE(runtime != nullptr);
        if (!runtime) {
            co_return;
        }
        runtime->start();
        const bool failed = co_await waitUntil([&]() {
            return runtime->failed();
        });
        XX_TEST_EXPECT_TRUE(failed);
        XX_TEST_EXPECT_EQ(initErrors.load(), 1);
        XX_TEST_EXPECT_FALSE(runtime->running());
        runtime->stop();
        co_await agent->shutdownAsync(std::chrono::seconds{10});
        harness.shutdown();
    }
}

// ---------------------------------------------------------------------------
// 已初始化的 agent: 投递一条输入, 走完一轮
// ---------------------------------------------------------------------------
asio::awaitable<void> testRuntimeTurn() {
    auto ex = co_await asio::this_coro::executor;

    auto cfg   = makeRtConfig();
    auto agent = std::make_shared<agentxx::agent::CodeAgent>(cfg);
    // 先给 agent 一条线程, 并在那条线程上 init: 会话可变状态绑定 io 线程,
    // 跨线程 init 会让后续驱动循环撞断言
    RuntimeHarness harness{agent};
    co_await asio::co_spawn(
        harness.executor(),
        [agent]() -> asio::awaitable<void> {
            co_await agent->init();
        },
        asio::use_awaitable
    );
    // 插件挂载根在建根时就已经 init 过, 通道这里 skipInit
    auto provider = std::make_shared<FakeProvider>();
    provider->pushText("这条回复来自假 provider");
    FakeProvider::inject(*agent, kRtModelName, provider);

    auto io = std::make_shared<RecordingClientIO>(harness.executor());
    io->initSelf(io);

    agent::AgentSessionRuntime::Config rtCfg;
    rtCfg.sessionId             = "rt-turn";
    rtCfg.clientExecutor        = harness.executor();
    rtCfg.skipInit              = true; // 已初始化: 启动壳只驱动会话
    rtCfg.autoRequestComponents = false;
    auto runtime                = AgentSessionRuntime::create(agent, io, std::move(rtCfg));
    XX_TEST_EXPECT_TRUE(runtime != nullptr);
    if (!runtime) {
        co_return;
    }
    runtime->start();
    const bool started = co_await waitUntil([&]() {
        return runtime->running();
    });
    XX_TEST_EXPECT_TRUE(started);

    // 解析 → 投递 (会话与请求序号由启动壳补)
    auto parsed = agentxx::agent::parseUserInputJson(R"({"text":"打个招呼"})");
    XX_TEST_EXPECT_TRUE(parsed.has_value());
    if (parsed) {
        runtime->sendInput(std::move(*parsed));
    }
    const bool turnDone = co_await waitUntil([&]() {
        return io->turnResults.load() >= 1;
    });
    XX_TEST_EXPECT_TRUE(turnDone);
    XX_TEST_EXPECT_EQ(provider->requestCount(), size_t{1});
    XX_TEST_EXPECT_TRUE(runtime->lastInputRequestId() >= 1);

    // 会话末尾是模型最终回答 (说明这轮确实跑在 rt-turn 会话上)
    // - 会话可变状态只能在绑定 io 线程访问, 所以到 agent 线程上去读
    std::string answer;
    co_await asio::co_spawn(
        harness.executor(),
        [agent, &answer]() -> asio::awaitable<void> {
            answer = lastAssistantContent(agent->agentContext->sessions->get("rt-turn"));
            co_return;
        },
        asio::use_awaitable
    );
    XX_TEST_EXPECT_EQ(answer, std::string{"这条回复来自假 provider"});
    // 客户端也收到了同样的正文增量
    XX_TEST_EXPECT_TRUE(io->textSnapshot().find("假 provider") != std::string::npos);

    runtime->stop();
    const bool stopped = co_await waitUntil([&]() {
        return !runtime->running();
    });
    XX_TEST_EXPECT_TRUE(stopped);
    co_await agent->shutdownAsync(std::chrono::seconds{10});
    harness.shutdown();
}

} // namespace

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
asio::awaitable<TestResult> run_session_runtime_tests() {
    testParseUserInputJson();
    co_await testRuntimeLifecycle();
    co_await testRuntimeTurn();
    co_return TestResult{g_srt_passed, g_srt_failed};
}

} // namespace test
} // namespace agentxx
