/// 并发与竞态清单 (计划 TST-4)
///
/// 背景: 这些时序竞态在单线程协程模型下同样存在 —— 两个协程的交错点由
/// `co_await` 决定, 取消信号、工具结算、持久化刷盘和中断应答都可能在"别人
/// 正在处理一半"的时候到达。这里不去断言"谁先谁后" (那会变成看调度运气),
/// 而是断言**不变量**: 每条 tool_call 恰好一条结果、已结算结果不被取消占位
/// 顶掉、轮次一定收敛 (不挂死)、节流与轮末刷盘合起来不丢消息。
///
/// 覆盖:
/// - R1 取消 vs 工具结算: 取消时已完成的结果保留, 未启动的补取消占位,
///   整批结果数量与 tool_call 一一对应
/// - R2 并行结果乱序: 5 个并行安全调用按声明顺序提交 (完成顺序完全相反)
/// - R3 中断应答在途 vs 取消: 无论落到"取消结束"还是"应答后 resume",
///   轮次收敛且结果唯一 (应答迟到不重复执行)
/// - R4 工具执行中被注销 (插件工具的动态注册表路径): 执行体保活,
///   调用正常返回真实结果, 注册表已不再包含该工具
/// - R5 持久化节流 vs 轮末: 窗口内的多次写 + 轮末立即刷盘 = 库内容完整且不重复
#include "agentxx-test/core/test_race_guards.h"

#include "agentxx-test/core/test_agent.h" // 本地 LLM 模拟器

#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/tools/tool.h"
#include "asio/co_spawn.hpp"
#include "asio/deferred.hpp"
#include "asio/experimental/parallel_group.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_rg_passed = 0;
int g_rg_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_rg_passed
#define XX_TEST_FAILED g_rg_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

using agentxx::agent::ViewMessage;

std::string makeTempRoot() {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_rg_test_{}",
                   std::chrono::steady_clock::now().time_since_epoch().count()
               );
    fs::create_directories(dir);
    return dir.string();
}

void removeTempRoot(const std::string& root) {
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::error_code ec;
        fs::remove_all(utilxx_base::utf8ToPath(root), ec);
        if (!ec) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
}

/// 一次工具执行的开始/结束轨迹
struct TraceEntry {
    std::string name;
    bool        isStart = true;
};

using Trace = std::shared_ptr<std::vector<TraceEntry>>;

/// 记录执行轨迹、可睡眠的测试工具
class TraceTool : public agentxx::tools::XXToolBase {
public:

    TraceTool(
        std::string_view name,
        Trace            trace,
        int64_t          sleepMs,
        bool             parallelSafe,
        std::atomic<int>* execCount = nullptr
    ) :
        XXToolBase(
            name,
            std::weak_ptr<agentxx::agent::AgentContext>{},
            false,
            false,
            0,
            false,
            parallelSafe
        ),
        name_(name),
        trace_(std::move(trace)),
        sleepMs_(sleepMs),
        execCount_(execCount) {}

    neograph::ChatTool get_definition() const override {
        return neograph::ChatTool{
            .name        = name_,
            .description = "race guard trace tool",
            .parameters  = neograph::json{{"type", "object"}, {"properties", neograph::json::object()}},
        };
    }

    asio::awaitable<std::string> execute_async(const utilxx_base::Json&) override {
        if (execCount_) {
            execCount_->fetch_add(1, std::memory_order_relaxed);
        }
        trace_->push_back(TraceEntry{name_, true});
        if (sleepMs_ > 0) {
            asio::steady_timer timer{co_await asio::this_coro::executor};
            timer.expires_after(std::chrono::milliseconds{sleepMs_});
            co_await timer.async_wait(asio::use_awaitable);
        }
        trace_->push_back(TraceEntry{name_, false});
        co_return fmt::format("{} done", name_);
    }

private:

    std::string       name_;
    Trace             trace_;
    int64_t           sleepMs_ = 0;
    std::atomic<int>* execCount_ = nullptr;
};

/// 触发中断的测试工具 (首次调用抛 NodeInterrupt, resume 后从中断结果取值)
class InterruptTool : public agentxx::tools::XXToolBase {
public:

    InterruptTool(std::weak_ptr<agentxx::agent::AgentContext> ctx, std::atomic<int>* execCount) :
        XXToolBase("race_interrupt", ctx, false, false),
        execCount_(execCount) {}

    neograph::ChatTool get_definition() const override {
        return neograph::ChatTool{
            .name        = "race_interrupt",
            .description = "interrupt tool for race guard test",
            .parameters  = neograph::json{{"type", "object"}, {"properties", neograph::json::object()}},
        };
    }

    asio::awaitable<std::string> execute_async(const utilxx_base::Json& arguments) override {
        execCount_->fetch_add(1, std::memory_order_relaxed);
        auto ctx = agentContext.lock();
        if (!ctx || !ctx->middlewareHandleContext) {
            co_return "[no context]";
        }
        const auto sessionId = arguments.value("sessionId", std::string{});
        const auto resultId  = arguments.value("tool_call_id", std::string{});
        auto       result    = co_await ctx->middlewareHandleContext->requestInterrupt(
            sessionId,
            [&]() {
                return agentxx::middleware::InterruptHandleArg{
                    .name = agentxx::middleware::MiddlewareContext::interruptHandleName_default,
                    .arg  = utilxx_base::Json{{"question", "approve?"}},
                    .resultId = resultId,
                };
            },
            nullptr
        );
        if (result.is_object() && !resultId.empty() && result.contains(resultId)) {
            auto val = result[resultId];
            if (val.is_string()) {
                co_return val.get<std::string>();
            }
            if (val.is_object() && val.size() == 1) {
                for (auto it = val.begin(); it != val.end(); ++it) {
                    if (it->is_string()) {
                        co_return it->get<std::string>();
                    }
                }
            }
            co_return val.dump();
        }
        co_return result.is_string() ? result.get<std::string>() : result.dump();
    }

private:

    std::atomic<int>* execCount_;
};

/// 测试用 agent: 注入本模块的轨迹工具 / 中断工具
class RaceTestAgent : public agentxx::agent::CodeAgent {
public:

    RaceTestAgent(
        std::shared_ptr<agentxx::agent::AgentConfig>             cfg,
        std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> extra
    ) :
        CodeAgent(std::move(cfg)),
        extraToolList_(std::move(extra)) {}

protected:

    asio::awaitable<std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>> initTools() override {
        auto tools = co_await CodeAgent::initTools();
        for (auto& tool : extraToolList_) {
            tools.push_back(std::move(tool));
        }
        co_return tools;
    }

private:

    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> extraToolList_;
};

/// 中断用例专用 agent: 工具需要拿到真实的 AgentContext (在 initTools 里创建)
class InterruptRaceAgent : public agentxx::agent::CodeAgent {
public:

    std::shared_ptr<std::atomic<int>> interruptExecCount = std::make_shared<std::atomic<int>>(0);

    explicit InterruptRaceAgent(std::shared_ptr<agentxx::agent::AgentConfig> cfg) :
        CodeAgent(std::move(cfg)) {}

protected:

    asio::awaitable<std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>> initTools() override {
        auto tools = co_await CodeAgent::initTools();
        tools.push_back(std::make_unique<InterruptTool>(agentContext, interruptExecCount.get()));
        co_return tools;
    }
};

/// 中断应答故意迟到的 IO: 先等一下再给结果 (模拟"用户还在看卡片")
class DelayedInterruptIO : public agentxx::agent::AgentIOBase {
public:

    /// 应答前等待时长 (毫秒)
    int64_t                                     answerDelayMs = 250;
    std::weak_ptr<agentxx::agent::AgentContext> agentContext;
    std::atomic<int>                            interruptCalls{0};
    std::atomic<bool>                           answerDelivered{false};

    explicit DelayedInterruptIO(std::shared_ptr<agentxx::agent::AgentContext> ctx) :
        agentContext(ctx) {}

    void onDelta(const agentxx::agent::WireDelta&) override {}

    void onSync(const agentxx::agent::WireSyncPayload&) override {}

    asio::awaitable<std::optional<std::string>> getInput() override {
        co_return std::nullopt;
    }

    asio::awaitable<utilxx_base::Json> handleInterrupt(
        std::string_view,
        std::string_view,
        std::string_view,
        std::string_view
    ) override {
        ++interruptCalls;
        asio::steady_timer timer{co_await asio::this_coro::executor};
        timer.expires_after(std::chrono::milliseconds{answerDelayMs});
        utilxx_base::AsioErrorCode ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        answerDelivered.store(!ec, std::memory_order_release);
        co_return agentxx::middleware::makeInterruptResult(
            utilxx_base::Json{{"handled", "late-answer"}}
        );
    }
};

/// 构造一个 tool_call 声明 (OpenAI 流式 delta 形态)
utilxx_base::Json makeToolCall(int index, std::string_view id, std::string_view name) {
    return utilxx_base::Json{
        {"index", index},
        {"id", std::string{id}},
        {"type", "function"},
        {"function", utilxx_base::Json{{"name", std::string{name}}, {"arguments", "{}"}}},
    };
}

void declareToolCalls(const std::vector<std::pair<std::string, std::string>>& idAndName) {
    utilxx_base::Json calls = utilxx_base::Json::array();
    int               index = 0;
    for (const auto& [id, name] : idAndName) {
        calls.push_back(makeToolCall(index++, id, name));
    }
    g_da_sim_tool_calls = std::move(calls);
}

/// 最后一条带 tool_calls 的 assistant 之后连续的 tool 结果
std::vector<std::pair<std::string, std::string>> toolResults(const std::shared_ptr<agentxx::agent::Session>& session) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!session) {
        return out;
    }
    const auto& messages       = session->messages();
    size_t      assistantIndex = messages.size();
    for (size_t i = messages.size(); i > 0; --i) {
        if (messages[i - 1].role == "assistant" && !messages[i - 1].tool_calls.empty()) {
            assistantIndex = i - 1;
            break;
        }
    }
    if (assistantIndex >= messages.size()) {
        return out;
    }
    for (size_t i = assistantIndex + 1; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        if (msg.role != "tool") {
            break;
        }
        out.emplace_back(msg.tool_call_id, msg.content);
    }
    return out;
}

/// 声明的 tool_call id 列表 (最后一条带 tool_calls 的 assistant)
std::vector<std::string> declaredCallIds(const std::shared_ptr<agentxx::agent::Session>& session) {
    std::vector<std::string> out;
    if (!session) {
        return out;
    }
    for (auto it = session->messages().rbegin(); it != session->messages().rend(); ++it) {
        if (it->role == "assistant" && !it->tool_calls.empty()) {
            for (const auto& tc : it->tool_calls) {
                out.push_back(tc.id);
            }
            break;
        }
    }
    return out;
}

/// 统计"声明了 tool_calls 但没有对应 tool 结果"的悬挂调用数 (0 = 上下文完整)
/// - 取消/中断后允许该轮工具调用尚未定稿, 但不允许留下半截状态 (模型看到
///   "assistant 声明了工具但没有结果"会被 API 拒绝)
size_t danglingToolCallCount(const std::shared_ptr<agentxx::agent::Session>& session) {
    size_t dangling = 0;
    if (!session) {
        return dangling;
    }
    const auto& msgs = session->messages();
    for (size_t i = 0; i < msgs.size(); ++i) {
        if (msgs[i].role != "assistant" || msgs[i].tool_calls.empty()) {
            continue;
        }
        for (const auto& tc : msgs[i].tool_calls) {
            bool replied = false;
            for (size_t j = i + 1; j < msgs.size() && msgs[j].role == "tool"; ++j) {
                if (msgs[j].tool_call_id == tc.id) {
                    replied = true;
                    break;
                }
            }
            if (!replied) {
                ++dangling;
            }
        }
    }
    return dangling;
}

/// 轮询等待会话上下文里出现 [count] 条 tool 结果
asio::awaitable<bool> waitToolResults(
    const std::shared_ptr<agentxx::agent::Session>& session,
    size_t                                          count,
    int                                             timeoutMs = 6000
) {
    asio::steady_timer timer{co_await asio::this_coro::executor};
    const auto         deadline = std::chrono::steady_clock::now()
                                  + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline) {
        if (toolResults(session).size() >= count) {
            co_return true;
        }
        timer.expires_after(std::chrono::milliseconds{20});
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return toolResults(session).size() >= count;
}

/// 不变量: 每条声明的 tool_call 恰好一条结果, 结果顺序与声明顺序一致
void expectExactlyOneResultPerCall(
    const std::shared_ptr<agentxx::agent::Session>& session,
    size_t                                          expectedCount
) {
    const auto ids     = declaredCallIds(session);
    const auto results = toolResults(session);
    XX_TEST_EXPECT_EQ(ids.size(), expectedCount);
    XX_TEST_EXPECT_EQ(results.size(), expectedCount);
    std::map<std::string, int> counted;
    for (const auto& [id, _] : results) {
        ++counted[id];
    }
    for (const auto& id : ids) {
        XX_TEST_EXPECT_EQ(counted[id], 1);
    }
    for (size_t i = 0; i < ids.size() && i < results.size(); ++i) {
        XX_TEST_EXPECT_EQ(results[i].first, ids[i]);
    }
}

} // namespace

// ===========================================================================
// R1: 取消 vs 工具结算 (快的已完成, 中的执行中被取消, 独占的未启动)
// ===========================================================================
namespace {

asio::awaitable<void> test_cancel_vs_tool_settlement() {
    const std::string sessionId = "race_cancel_settlement";
    auto              sim       = startDaSimServer();

    auto cfg                        = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl              = "http://127.0.0.1:" + std::to_string(sim.port);
    cfg->model.apiKey               = "EMPTY";
    cfg->model.modelName            = "test-sim";
    cfg->prompt.systemPrompt        = "You are a helpful assistant.";
    cfg->toolParallelMaxConcurrency = 4;

    g_da_sim_response_content = "";
    g_da_sim_delay_ms         = 0;
    declareToolCalls(
        {{"call_fast", "race_fast"}, {"call_mid", "race_mid"}, {"call_later", "race_later"}}
    );

    auto trace = std::make_shared<std::vector<TraceEntry>>();
    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> tools;
    tools.push_back(std::make_unique<TraceTool>("race_fast", trace, 20, /*parallelSafe=*/true));
    tools.push_back(std::make_unique<TraceTool>("race_mid", trace, 600, /*parallelSafe=*/true));
    tools.push_back(std::make_unique<TraceTool>("race_later", trace, 10, /*parallelSafe=*/false));

    RaceTestAgent agent(cfg, std::move(tools));
    co_await agent.init();

    auto ex = co_await asio::this_coro::executor;

    // 快的已经结算 (轨迹出现结束记录)、中的正在跑时取消
    auto cancelWatcher = [&]() -> asio::awaitable<void> {
        asio::steady_timer timer(ex);
        for (int i = 0; i < 1000; ++i) {
            bool fastDone   = false;
            bool midStarted = false;
            for (const auto& e : *trace) {
                if (e.name == "race_fast" && !e.isStart) {
                    fastDone = true;
                }
                if (e.name == "race_mid" && e.isStart) {
                    midStarted = true;
                }
            }
            if (fastDone && midStarted) {
                break;
            }
            timer.expires_after(std::chrono::milliseconds{2});
            co_await timer.async_wait(asio::use_awaitable);
        }
        auto session = agent.agentContext->sessions->get(sessionId);
        if (session) {
            auto token = session->getCancelToken();
            if (token) {
                token->cancel();
            }
        }
        co_return;
    };

    auto [order, turnExc, turnResult, watcherExc]
        = co_await asio::experimental::make_parallel_group(
              asio::co_spawn(ex, agent.runTurnAsync(sessionId, "Run tools", nullptr), asio::deferred),
              asio::co_spawn(ex, cancelWatcher(), asio::deferred)
        )
              .async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
    (void)order;

    XX_TEST_EXPECT_TRUE(turnExc == nullptr);
    XX_TEST_EXPECT_TRUE(watcherExc == nullptr);
    XX_TEST_EXPECT_TRUE(turnResult.hasError);
    XX_TEST_EXPECT_EQ(turnResult.errorMessage, std::string{"Cancelled by user"});

    // 独占工具排在并行批之后: 取消时还没轮到它, 其执行体不得被调用
    const bool laterRan = std::any_of(
        trace->begin(),
        trace->end(),
        [](const TraceEntry& e) { return e.name == "race_later"; }
    );
    XX_TEST_EXPECT_FALSE(laterRan);

    auto session = agent.agentContext->sessions->get(sessionId);
    XX_TEST_EXPECT_TRUE(co_await waitToolResults(session, 3));
    // 不变量: 3 条声明 = 3 条结果, 每条恰好一次, 且顺序按声明顺序
    expectExactlyOneResultPerCall(session, 3);
    const auto results = toolResults(session);
    if (results.size() == 3) {
        // 已结算的保留真实结果 (不能被取消占位顶掉)
        XX_TEST_EXPECT_EQ(results[0].second, std::string{"race_fast done"});
        // 执行中被取消的与未启动的都补取消占位
        XX_TEST_EXPECT_EQ(results[1].second, std::string{"[User canceled]"});
        XX_TEST_EXPECT_EQ(results[2].second, std::string{"[User canceled]"});
    }

    // 等被取消的执行体真正结束再退出 (它的内部计时器不随取消信号中止)
    {
        asio::steady_timer timer(ex);
        for (int i = 0; i < 300; ++i) {
            const bool midDone = std::any_of(
                trace->begin(),
                trace->end(),
                [](const TraceEntry& e) { return e.name == "race_mid" && !e.isStart; }
            );
            if (midDone) {
                break;
            }
            timer.expires_after(std::chrono::milliseconds{10});
            co_await timer.async_wait(asio::use_awaitable);
        }
    }

    g_da_sim_tool_calls = utilxx_base::Json::array();
    sim.stop();
    co_return;
}

// ===========================================================================
// R2: 并行结果乱序 (5 个调用完成顺序与声明顺序完全相反)
// ===========================================================================
asio::awaitable<void> test_parallel_reverse_completion_order() {
    const std::string sessionId = "race_reverse_order";
    auto              sim       = startDaSimServer();

    auto cfg                        = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl              = "http://127.0.0.1:" + std::to_string(sim.port);
    cfg->model.apiKey               = "EMPTY";
    cfg->model.modelName            = "test-sim";
    cfg->prompt.systemPrompt        = "You are a helpful assistant.";
    cfg->toolParallelMaxConcurrency = 8;

    g_da_sim_response_content = "All reverse tools done.";
    g_da_sim_delay_ms         = 0;
    constexpr int kCount      = 5;
    std::vector<std::pair<std::string, std::string>> declared;
    for (int i = 0; i < kCount; ++i) {
        declared.emplace_back(fmt::format("call_r{}", i), fmt::format("race_rev{}", i));
    }
    declareToolCalls(declared);

    auto trace = std::make_shared<std::vector<TraceEntry>>();
    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> tools;
    for (int i = 0; i < kCount; ++i) {
        // 睡眠时长递减: 后声明的先完成 (完成顺序与声明顺序完全相反)
        const int64_t sleepMs = 100 - i * 20;
        tools.push_back(std::make_unique<TraceTool>(
            fmt::format("race_rev{}", i),
            trace,
            sleepMs,
            /*parallelSafe=*/true
        ));
    }

    RaceTestAgent agent(cfg, std::move(tools));
    co_await agent.init();

    const auto result = co_await agent.runTurnAsync(sessionId, "Run tools", nullptr);
    XX_TEST_EXPECT_FALSE(result.hasError);

    auto session = agent.agentContext->sessions->get(sessionId);
    XX_TEST_EXPECT_TRUE(co_await waitToolResults(session, kCount));
    // 结果按声明顺序写回 (与完成顺序无关)
    expectExactlyOneResultPerCall(session, kCount);
    const auto results = toolResults(session);
    for (size_t i = 0; i < results.size() && i < size_t{kCount}; ++i) {
        XX_TEST_EXPECT_EQ(results[i].second, fmt::format("race_rev{} done", i));
    }
    // 完成顺序确实是反的 (最先出现结束记录的是最后声明的那个)
    {
        std::string firstFinished;
        for (const auto& e : *trace) {
            if (!e.isStart) {
                firstFinished = e.name;
                break;
            }
        }
        XX_TEST_EXPECT_EQ(firstFinished, std::string{"race_rev4"});
    }

    g_da_sim_tool_calls = utilxx_base::Json::array();
    sim.stop();
    co_return;
}

// ===========================================================================
// R3: 中断应答在途 vs 取消
// ===========================================================================
asio::awaitable<void> test_interrupt_answer_inflight_vs_cancel() {
    const std::string sessionId = "race_interrupt_cancel";
    auto              sim       = startDaSimServer();

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = "http://127.0.0.1:" + std::to_string(sim.port);
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";

    g_da_sim_response_content = "Final answer.";
    g_da_sim_delay_ms         = 0;
    g_da_sim_tool_calls_remaining = 1;
    declareToolCalls({{"call_int_1", "race_interrupt"}});

    InterruptRaceAgent agent(cfg);
    co_await agent.init();

    auto ex = co_await asio::this_coro::executor;
    auto io = std::make_shared<DelayedInterruptIO>(agent.agentContext);
    io->answerDelayMs = 300;

    // 中断已经问出去之后取消 (应答还在路上)
    auto cancelWatcher = [&]() -> asio::awaitable<void> {
        asio::steady_timer timer(ex);
        for (int i = 0; i < 1000; ++i) {
            if (io->interruptCalls.load(std::memory_order_acquire) > 0) {
                break;
            }
            timer.expires_after(std::chrono::milliseconds{5});
            co_await timer.async_wait(asio::use_awaitable);
        }
        auto session = agent.agentContext->sessions->get(sessionId);
        if (session) {
            auto token = session->getCancelToken();
            if (token) {
                token->cancel();
            }
        }
        co_return;
    };

    const auto startAt = std::chrono::steady_clock::now();
    auto [order, turnExc, turnResult, watcherExc]
        = co_await asio::experimental::make_parallel_group(
              asio::co_spawn(
                  ex,
                  agent.runTurnAsync(sessionId, "Please interrupt", io),
                  asio::deferred
              ),
              asio::co_spawn(ex, cancelWatcher(), asio::deferred)
        )
              .async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
    (void)order;
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - startAt
    )
                               .count();

    XX_TEST_EXPECT_TRUE(turnExc == nullptr);
    XX_TEST_EXPECT_TRUE(watcherExc == nullptr);
    // 轮次一定收敛: 不允许"取消后还挂着等应答"
    XX_TEST_EXPECT_TRUE(elapsedMs < 6000);
    // 中断只问一次 (取消不会引起第二次询问)
    XX_TEST_EXPECT_EQ(io->interruptCalls.load(), 1);

    // 两条合法结局之一: ① 取消结束 (应答迟到不再 resume); ② 应答先落地, resume 完成
    auto session = agent.agentContext->sessions->get(sessionId);
    if (turnResult.hasError) {
        XX_TEST_EXPECT_EQ(turnResult.errorMessage, std::string{"Cancelled by user"});
        const auto danglingBefore = danglingToolCallCount(session);
        TEST_INFO << "[race_guards] 中断/取消落到取消分支: dangling=" << danglingBefore
                  << " messages=" << (session ? session->messagesCount() : 0) << std::endl;
        // 该轮工具调用尚未定稿, 取消后允许留下"声明了工具但还没有结果"的悬挂形态
        // (本例至多 1 条); 下一次请求前必须被修正 —— 否则模型会收到非法上下文
        XX_TEST_EXPECT_TRUE(danglingBefore <= 1);
        if (danglingBefore > 0) {
            g_da_sim_tool_calls           = utilxx_base::Json::array();
            g_da_sim_tool_calls_remaining = -1;
            g_da_sim_response_content     = "Answer after cancelled turn.";
            const auto second = co_await agent.runTurnAsync(sessionId, "continue", nullptr);
            XX_TEST_EXPECT_FALSE(second.hasError);
            auto after = agent.agentContext->sessions->get(sessionId);
            XX_TEST_EXPECT_EQ(danglingToolCallCount(after), size_t{0});
        }
    } else {
        // 应答落地后 resume: 工具返回真实应答文本, 不是占位
        XX_TEST_EXPECT_TRUE(co_await waitToolResults(session, 1));
        const auto results = toolResults(session);
        XX_TEST_EXPECT_EQ(results.size(), size_t{1});
        if (results.size() == 1) {
            XX_TEST_EXPECT_EQ(results[0].second, std::string{"late-answer"});
        }
        expectExactlyOneResultPerCall(session, 1);
    }
    // 中断执行不超过两次 (一次触发中断, 一次 resume 重跑)
    XX_TEST_EXPECT_TRUE(agent.interruptExecCount->load() <= 2);
    g_da_sim_tool_calls = utilxx_base::Json::array();
    sim.stop();
    co_return;
}

// ===========================================================================
// R4: 工具执行中被注销 (插件工具走的就是这条动态注册表路径)
// ===========================================================================
asio::awaitable<void> test_unregister_during_tool_execution() {
    const std::string sessionId = "race_unregister";
    auto              sim       = startDaSimServer();

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = "http://127.0.0.1:" + std::to_string(sim.port);
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";

    g_da_sim_response_content = "Done.";
    g_da_sim_delay_ms         = 0;
    declareToolCalls({{"call_dyn_1", "race_dyn"}});

    auto trace = std::make_shared<std::vector<TraceEntry>>();
    // 动态注册表持有 shared_ptr (与插件工具同一形态)
    auto dynTool = std::make_shared<TraceTool>(
        "race_dyn",
        trace,
        250,
        /*parallelSafe=*/false
    );

    RaceTestAgent agent(cfg, {});
    co_await agent.init();
    XX_TEST_EXPECT_TRUE(agent.agentContext->toolRegistry->registerTool("race_dyn", dynTool));
    XX_TEST_EXPECT_TRUE(agent.agentContext->toolRegistry->contains("race_dyn"));

    const auto weakRef = std::weak_ptr<agentxx::tools::XXToolBase>{dynTool};

    auto ex = co_await asio::this_coro::executor;
    // 工具开始执行后把它从注册表摘掉 (模拟插件卸载/工具注销)
    auto unregisterWatcher = [&]() -> asio::awaitable<void> {
        asio::steady_timer timer(ex);
        for (int i = 0; i < 1000; ++i) {
            if (!trace->empty()) {
                break;
            }
            timer.expires_after(std::chrono::milliseconds{2});
            co_await timer.async_wait(asio::use_awaitable);
        }
        agent.agentContext->toolRegistry->unregisterTool("race_dyn");
        co_return;
    };

    auto [order, turnExc, turnResult, watcherExc]
        = co_await asio::experimental::make_parallel_group(
              asio::co_spawn(ex, agent.runTurnAsync(sessionId, "Run tool", nullptr), asio::deferred),
              asio::co_spawn(ex, unregisterWatcher(), asio::deferred)
        )
              .async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
    (void)order;

    XX_TEST_EXPECT_TRUE(turnExc == nullptr);
    XX_TEST_EXPECT_TRUE(watcherExc == nullptr);
    XX_TEST_EXPECT_FALSE(turnResult.hasError);
    // 注册表已摘除, 但执行中的调用保活并正常返回真实结果
    XX_TEST_EXPECT_FALSE(agent.agentContext->toolRegistry->contains("race_dyn"));
    auto session = agent.agentContext->sessions->get(sessionId);
    XX_TEST_EXPECT_TRUE(co_await waitToolResults(session, 1));
    const auto results = toolResults(session);
    XX_TEST_EXPECT_EQ(results.size(), size_t{1});
    if (results.size() == 1) {
        XX_TEST_EXPECT_EQ(results[0].second, std::string{"race_dyn done"});
    }
    // 调用结束后保活引用随之释放 (只剩本用例持有的那一份)
    dynTool.reset();
    XX_TEST_EXPECT_TRUE(weakRef.expired());

    g_da_sim_tool_calls = utilxx_base::Json::array();
    sim.stop();
    co_return;
}

// ===========================================================================
// R5: 持久化节流 vs 轮末刷盘
// ===========================================================================
void test_persist_throttle_vs_turn_end() {
    const auto        root = makeTempRoot();
    const std::string sid  = "race_persist";

    auto store = std::make_shared<agentxx::agent::SessionStore>(root);
    {
        agentxx::agent::SessionsManager mgr;
        mgr.sessionStore = store;
        auto session     = mgr.getOrCreate(sid);

        auto makeMsg = [](int i) {
            ViewMessage msg;
            msg.id          = fmt::format("m{}", i);
            msg.role        = ViewMessage::Role::User;
            msg.text        = fmt::format("message-{}", i);
            msg.startTimeMs = 1700000000000LL + i;
            return msg;
        };

        // 节流窗口内连续追加: 只有第一条立即落盘, 其余进待落盘队列
        for (int i = 1; i <= 6; ++i) {
            session->appendViewMessage(makeMsg(i));
        }
        // 轮次进行中的节流刷盘 (窗口内不再写)
        session->persistThrottled("llm-output");
        // 轮末终态: 立即刷盘 (不受节流窗口限制)
        session->persistNow("turn-end");

        // 再叠加一轮: 节流窗口内追加 + 期间一次的"半途"节流写, 最后轮末刷盘
        for (int i = 7; i <= 9; ++i) {
            session->appendViewMessage(makeMsg(i));
        }
        session->persistThrottled("llm-output");
        session->appendViewMessage(makeMsg(10));
        session->persistNow("turn-end");
    }

    // 库内内容 = 内存里的全部消息, 顺序一致且不重复 (节流与轮末刷盘合起来无丢失)
    auto loaded = store->loadSession(sid);
    XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), size_t{10});
    for (size_t i = 0; i < loaded.viewMessages.size(); ++i) {
        XX_TEST_EXPECT_EQ(loaded.viewMessages[i].text, fmt::format("message-{}", i + 1));
    }
    // 序号连续且唯一 (重复刷盘不能产生重复序号)
    XX_TEST_EXPECT_EQ(loaded.lastViewSeq, uint64_t{10});

    removeTempRoot(root);
}

} // namespace

asio::awaitable<TestResult> run_race_guard_tests() {
    const int passedBefore = g_rg_passed;
    const int failedBefore = g_rg_failed;

    auto elapsedMs = [](const std::chrono::steady_clock::time_point& start) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start
        )
            .count();
    };

    try {
        {
            const auto t0 = std::chrono::steady_clock::now();
            co_await test_cancel_vs_tool_settlement();
            TEST_INFO << "[race_guards] R1 取消 vs 工具结算 用时 " << elapsedMs(t0) << " ms"
                      << std::endl;
        }
        {
            const auto t0 = std::chrono::steady_clock::now();
            co_await test_parallel_reverse_completion_order();
            TEST_INFO << "[race_guards] R2 并行结果乱序 用时 " << elapsedMs(t0) << " ms" << std::endl;
        }
        {
            const auto t0 = std::chrono::steady_clock::now();
            co_await test_interrupt_answer_inflight_vs_cancel();
            TEST_INFO << "[race_guards] R3 中断应答在途 vs 取消 用时 " << elapsedMs(t0) << " ms"
                      << std::endl;
        }
        {
            const auto t0 = std::chrono::steady_clock::now();
            co_await test_unregister_during_tool_execution();
            TEST_INFO << "[race_guards] R4 执行中注销工具 用时 " << elapsedMs(t0) << " ms"
                      << std::endl;
        }
        {
            const auto t0 = std::chrono::steady_clock::now();
            test_persist_throttle_vs_turn_end();
            TEST_INFO << "[race_guards] R5 节流 vs 轮末 用时 " << elapsedMs(t0) << " ms" << std::endl;
        }
    } catch (const std::exception& e) {
        TEST_FAIL << "race_guards suite exception: " << e.what() << std::endl;
        g_rg_failed++;
    }

    co_return TestResult{g_rg_passed - passedBefore, g_rg_failed - failedBefore};
}

} // namespace test
} // namespace agentxx
