/// test_fake_provider —— 假 provider 接缝 (计划 LLM-5 / TST-1)
///
/// 背景: 现有 LLM 交互用例多经本地 HTTP 模拟服务器验证, 依赖端口、服务线程与
/// HTTP 协议栈。本模块改为经 `ModelProviderRegistry::setProvider` 注入假 provider
/// (与真实 provider 完全相同的调用路径, 见 `nodes/modelcall.cpp` 的
/// `resolveCurrentProvider`), 于是重试、压缩、取消与工具循环都能在不联网、
/// 不占端口的前提下断言。
///
/// 覆盖:
/// - 固定流: 分片正文与思考片段的拼接结果、请求体结构 (system 首条 / 工具 schema)
/// - 工具循环: 模型给出 tool_call → 工具真实执行 → 第二次请求带回 tool 结果
/// - 错误注入: 不可重试 (401) 立即结束; 可重试 (500 + retry_after) 退避后成功
/// - 溢出压缩: 400 + context_length_exceeded 触发一次压缩后重试成功
/// - 取消: 等待期间的取消立即中断在途请求 (不必等延迟走完)
/// - 用量记账: 假 provider 上报的用量进入会话账本 (STO-8 的调用点)
#include "agentxx-test/core/test_fake_provider.h"

#include "agentxx-test/core/fake_provider.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/tools/tool.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include "utilxx_base/string_util.h"
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_fp_passed = 0;
int g_fp_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_fp_passed
#define XX_TEST_FAILED g_fp_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kFakeModelName = "test-fake";

/// 测试用回显工具: 工具循环用例据此断言"执行体真的跑了并回填了结果"
class EchoTool : public agentxx::tools::XXToolBase {
public:

    EchoTool() :
        XXToolBase(
            "fake_echo",
            std::weak_ptr<agentxx::agent::AgentContext>{},
            /*in_autoSummaryOutput=*/false,
            /*in_canDelayLoad=*/false,
            /*in_maxRetry=*/0,
            /*in_repeatCallCheck=*/false,
            /*parallelSafe=*/true
        ) {}

    neograph::ChatTool get_definition() const override {
        neograph::ChatTool def;
        def.name        = "fake_echo";
        def.description = "echo the given value (fake provider test tool)";
        def.parameters  = neograph::json{
            {"type", "object"},
            {"properties", neograph::json{{"value", neograph::json{{"type", "string"}}}}},
        };
        return def;
    }

    asio::awaitable<std::string> execute_async(const utilxx_base::Json& args) override {
        co_return fmt::format("echo:{}", args.value("value", std::string{}));
    }
};

/// 携带回显工具的测试 agent (工具需进入静态工具列表才能被 toolcall 节点解析)
class FakeProviderAgent : public agentxx::agent::CodeAgent {
public:

    using CodeAgent::CodeAgent;

protected:

    asio::awaitable<std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>> initTools() override {
        auto tools = co_await CodeAgent::initTools();
        tools.push_back(std::make_unique<EchoTool>());
        co_return tools;
    }
};

std::shared_ptr<agentxx::agent::AgentConfig> makeCfg(std::string dataDir = {}) {
    auto cfg = std::make_shared<agentxx::agent::AgentConfig>();
    // 假 provider 不发网络请求, base_url 只为满足配置校验
    cfg->model.baseUrl       = "http://127.0.0.1:1";
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = std::string{kFakeModelName};
    cfg->prompt.systemPrompt = "You are a helpful assistant.";
    cfg->llmMaxRetry         = 5;
    cfg->dataDir             = std::move(dataDir);
    // 记账用例需要会话库落盘 (默认关闭)
    cfg->enableSessionStore  = !cfg->dataDir.empty();
    return cfg;
}

/// 取会话最后一条 assistant 消息的正文 (无则返回空)
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

/// 请求体里是否存在指定工具定义
bool hasToolDefinition(const neograph::CompletionParams& params, std::string_view name) {
    for (const auto& tool : params.tools) {
        if (tool.name == name) {
            return true;
        }
    }
    return false;
}

/// 请求体里第一条指定角色的消息内容 (无则返回空)
std::string firstMessageOfRole(const neograph::CompletionParams& params, std::string_view role) {
    for (const auto& msg : params.messages) {
        if (msg.role == role) {
            return msg.content;
        }
    }
    return {};
}

int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

std::string makeTempDir() {
    auto dir = fs::temp_directory_path()
               / fmt::format("agentxx_fp_test_{}", steadyNowMs());
    fs::create_directories(dir);
    return dir.string();
}

void removeTempDir(const std::string& dir) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::error_code ec;
        fs::remove_all(utilxx_base::utf8ToPath(dir), ec);
        if (!ec) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 1) 固定流: 分片正文 + 思考片段 + 请求体结构
// ---------------------------------------------------------------------------
asio::awaitable<void> test_fake_provider_stream() {
    auto provider = std::make_shared<FakeProvider>();
    const std::string answer = "你好, 假 provider! streaming chunks ✓ 结尾";
    // 一次响应内含思考片段 + 正文, 均按 5 个字符分片 (跨越 UTF-8 多字节字符边界)
    provider->pushThinking("先想一下", answer, 0, 5);

    auto cfg   = makeCfg();
    auto agent = std::make_shared<FakeProviderAgent>(cfg);
    co_await agent->init();
    FakeProvider::inject(*agent, kFakeModelName, provider);

    auto result = co_await agent->runTurnAsync("fake_stream", "打个招呼", nullptr);
    XX_TEST_EXPECT_FALSE(result.hasError);
    XX_TEST_EXPECT_EQ(provider->requestCount(), size_t{1});

    // 分片拼接后与脚本正文完全一致 (含 UTF-8 多字节字符)
    auto session = agent->agentContext->sessions->get("fake_stream");
    XX_TEST_EXPECT_EQ(lastAssistantContent(session), answer);
    // 思考片段单独成段, 不进入正文
    XX_TEST_EXPECT_TRUE(lastAssistantContent(session).find("先想一下") == std::string::npos);

    // 请求体结构: system 首条带系统提示词, 工具 schema 随请求下发
    auto req = provider->requestAt(0);
    XX_TEST_EXPECT_TRUE(!req.messages.empty());
    XX_TEST_EXPECT_EQ(req.messages[0].role, std::string{"system"});
    // system 首条含配置的系统提示词 (宿主可能在末尾追加语言指令等固定后缀, 故按包含判断)
    XX_TEST_EXPECT_TRUE(
        req.messages[0].content.find(cfg->prompt.systemPrompt) != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(hasToolDefinition(req, "fake_echo"));
    XX_TEST_EXPECT_EQ(req.model, std::string{kFakeModelName});
    co_return;
}

// ---------------------------------------------------------------------------
// 2) 工具循环: tool_call → 执行体 → 第二次请求带回结果 → 最终回答
// ---------------------------------------------------------------------------
asio::awaitable<void> test_fake_provider_tool_loop() {
    auto provider = std::make_shared<FakeProvider>();
    provider->pushToolCalls(
        {FakeProvider::toolCall("call_1", "fake_echo", R"({"value":"hello"})")}
    );
    provider->pushText("工具已执行, 这是最终回答");

    auto cfg   = makeCfg();
    auto agent = std::make_shared<FakeProviderAgent>(cfg);
    co_await agent->init();
    FakeProvider::inject(*agent, kFakeModelName, provider);

    auto result = co_await agent->runTurnAsync("fake_tool_loop", "调用回显工具", nullptr);
    XX_TEST_EXPECT_FALSE(result.hasError);
    XX_TEST_EXPECT_GE(provider->requestCount(), size_t{2});

    // 第二次请求: 末尾带 tool 角色消息, 内容为执行体真实产出
    auto second  = provider->requestAt(1);
    bool sawTool = false;
    for (const auto& msg : second.messages) {
        if (msg.role == "tool" && msg.content.find("echo:hello") != std::string::npos) {
            sawTool = true;
        }
    }
    XX_TEST_EXPECT_TRUE(sawTool);

    // 会话上下文末尾是第二轮 assistant 的最终回答
    auto session = agent->agentContext->sessions->get("fake_tool_loop");
    XX_TEST_EXPECT_EQ(lastAssistantContent(session), std::string{"工具已执行, 这是最终回答"});
    co_return;
}

// ---------------------------------------------------------------------------
// 3) 错误注入: 不可重试立即结束; 可重试按 retry-after 等待后成功
// ---------------------------------------------------------------------------
asio::awaitable<void> test_fake_provider_error_policy() {
    // ---- 3a) 401 鉴权失败: 不可重试, 只请求一次 ----
    {
        auto provider = std::make_shared<FakeProvider>();
        provider->pushError(401, R"({"error":{"message":"Invalid API key"}})");

        auto cfg   = makeCfg();
        auto agent = std::make_shared<FakeProviderAgent>(cfg);
        co_await agent->init();
        FakeProvider::inject(*agent, kFakeModelName, provider);

        auto result = co_await agent->runTurnAsync("fake_err_auth", "hello", nullptr);
        XX_TEST_EXPECT_TRUE(result.hasError);
        XX_TEST_EXPECT_EQ(provider->requestCount(), size_t{1});
    }

    // ---- 3b) 500 可重试: 退避 (retry-after 1 秒) 后第二次成功 ----
    {
        auto provider = std::make_shared<FakeProvider>();
        provider->pushError(500, R"({"error":"internal server error","retry_after":1})");
        provider->pushText("重试后成功");

        auto cfg   = makeCfg();
        auto agent = std::make_shared<FakeProviderAgent>(cfg);
        co_await agent->init();
        FakeProvider::inject(*agent, kFakeModelName, provider);

        const auto start  = steadyNowMs();
        auto       result = co_await agent->runTurnAsync("fake_err_retry", "hello", nullptr);
        const auto cost   = steadyNowMs() - start;

        XX_TEST_EXPECT_FALSE(result.hasError);
        XX_TEST_EXPECT_EQ(provider->requestCount(), size_t{2});
        // 确实等待了 retry-after (1 秒), 不是立即重试
        XX_TEST_EXPECT_GE(cost, int64_t{900});

        auto session = agent->agentContext->sessions->get("fake_err_retry");
        XX_TEST_EXPECT_EQ(lastAssistantContent(session), std::string{"重试后成功"});
    }
    co_return;
}

// ---------------------------------------------------------------------------
// 4) 溢出压缩: 400 + context_length_exceeded 触发一次压缩后重试成功
// ---------------------------------------------------------------------------
asio::awaitable<void> test_fake_provider_overflow_compaction() {
    auto provider = std::make_shared<FakeProvider>();
    provider->pushError(
        400,
        R"({"error":{"code":"context_length_exceeded","message":"maximum context length is 8192 tokens"}})"
    );
    provider->pushText("摘要内容");
    provider->pushText("压缩后重试成功");

    auto cfg   = makeCfg();
    auto agent = std::make_shared<FakeProviderAgent>(cfg);
    co_await agent->init();
    FakeProvider::inject(*agent, kFakeModelName, provider);

    auto result = co_await agent->runTurnAsync("fake_overflow", "hello", nullptr);
    XX_TEST_EXPECT_FALSE(result.hasError);
    // 请求数 = 原始请求 + (压缩/重试), 远小于 1 + llmMaxRetry(5): 走的是压缩路径
    XX_TEST_EXPECT_GE(provider->requestCount(), size_t{2});
    XX_TEST_EXPECT_TRUE(provider->requestCount() <= size_t{4});
    // 压缩确实发生: 摘要正文进入了会话语义上下文
    auto session = agent->agentContext->sessions->get("fake_overflow");
    XX_TEST_EXPECT_TRUE(session != nullptr);
    if (session) {
        bool hasSummary = false;
        for (const auto& msg : session->messages()) {
            if (msg.content.find("摘要内容") != std::string::npos) {
                hasSummary = true;
            }
        }
        XX_TEST_EXPECT_TRUE(hasSummary);
    }
    co_return;
}

// ---------------------------------------------------------------------------
// 5) 取消: 等待期间取消立即中断在途请求 (不必等延迟走完)
// ---------------------------------------------------------------------------
asio::awaitable<void> test_fake_provider_cancel() {
    auto provider = std::make_shared<FakeProvider>();
    provider->pushText("迟到的回答", 5000); // 5 秒后才产出

    auto cfg   = makeCfg();
    auto agent = std::make_shared<FakeProviderAgent>(cfg);
    co_await agent->init();
    FakeProvider::inject(*agent, kFakeModelName, provider);

    auto ex = co_await asio::this_coro::executor;

    // 后台观察者: 等会话取消令牌出现后请求取消
    auto cancelWatcher = [&]() -> asio::awaitable<void> {
        asio::steady_timer timer(ex);
        for (int i = 0; i < 1000; ++i) {
            auto session = agent->agentContext->sessions->get("fake_cancel");
            if (session && session->getCancelToken()) {
                break;
            }
            timer.expires_after(std::chrono::milliseconds(5));
            co_await timer.async_wait(asio::use_awaitable);
        }
        timer.expires_after(std::chrono::milliseconds(200));
        co_await timer.async_wait(asio::use_awaitable);
        if (auto session = agent->agentContext->sessions->get("fake_cancel")) {
            if (auto token = session->getCancelToken()) {
                token->cancel();
            }
        }
        co_return;
    };

    const auto start = steadyNowMs();
    asio::co_spawn(ex, cancelWatcher(), asio::detached);
    auto       result = co_await agent->runTurnAsync("fake_cancel", "长请求", nullptr);
    const auto cost   = steadyNowMs() - start;

    XX_TEST_EXPECT_TRUE(result.hasError);          // 取消按失败结束本轮
    XX_TEST_EXPECT_TRUE(cost < int64_t{2000});     // 没有等满 5 秒延迟
    XX_TEST_EXPECT_GE(provider->cancelObserved(), 1); // 假 provider 在等待期间观察到取消
    co_return;
}

// ---------------------------------------------------------------------------
// 6) 用量记账: 假 provider 上报的用量进入会话账本 (记账调用点在 modelcall)
// ---------------------------------------------------------------------------
asio::awaitable<void> test_fake_provider_usage_ledger() {
    auto dir = makeTempDir();
    {
        auto provider = std::make_shared<FakeProvider>();
        FakeStep step;
        step.kind             = FakeStep::Kind::Text;
        step.text             = "记账用例";
        step.promptTokens     = 123;
        step.completionTokens = 45;
        step.cachedTokens     = 7;
        step.reasoningTokens  = 3;
        provider->pushStep(step);

        auto cfg   = makeCfg(dir);
        auto agent = std::make_shared<FakeProviderAgent>(cfg);
        co_await agent->init();
        FakeProvider::inject(*agent, kFakeModelName, provider);

        auto result = co_await agent->runTurnAsync("fake_usage", "记账", nullptr);
        XX_TEST_EXPECT_FALSE(result.hasError);

        auto store = agent->agentContext->sessions->sessionStore;
        XX_TEST_EXPECT_TRUE(store != nullptr);
        if (store) {
            auto summary = store->usageSummary("fake_usage");
            XX_TEST_EXPECT_EQ(summary.calls, int64_t{1});
            XX_TEST_EXPECT_EQ(summary.failedCalls, int64_t{0});
            XX_TEST_EXPECT_EQ(summary.promptTokens, int64_t{123});
            XX_TEST_EXPECT_EQ(summary.completionTokens, int64_t{45});
            XX_TEST_EXPECT_EQ(summary.cachedPromptTokens, int64_t{7});
            XX_TEST_EXPECT_EQ(summary.reasoningTokens, int64_t{3});
            XX_TEST_EXPECT_EQ(summary.totalTokens, int64_t{168});
        }
    }
    removeTempDir(dir);
    co_return;
}

asio::awaitable<TestResult> run_fake_provider_tests() {
    co_await test_fake_provider_stream();
    co_await test_fake_provider_tool_loop();
    co_await test_fake_provider_error_policy();
    co_await test_fake_provider_overflow_compaction();
    co_await test_fake_provider_cancel();
    co_await test_fake_provider_usage_ledger();
    co_return TestResult{g_fp_passed, g_fp_failed};
}

} // namespace test
} // namespace agentxx
