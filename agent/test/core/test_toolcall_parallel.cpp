/// test_toolcall_parallel —— 工具调用并行化 (计划 TOOL-1 / TOOL-2 / TOOL-3)
///
/// 背景: 同一条 assistant 消息可以声明多个 tool_call。工具执行被拆成三个阶段:
/// prepare (参数修正/权限/重复确认, 串行) → run (执行体, 受限并发) → finalize
/// (结果定稿, 串行)。只有显式声明并行安全的执行体才并发, 其余保持独占并形成
/// 顺序屏障; 结果始终按声明的 tool_call 顺序写回会话。
///
/// 覆盖:
/// - T1 并行安全工具并发执行 (执行区间重叠) 且完成顺序与声明顺序不同
/// - T2 结果按声明顺序写回 (完成顺序不影响提交顺序)
/// - T3 未声明并行安全的工具形成屏障: 与前后调用互不重叠, 顺序严格
/// - T4 并发上限 (AgentConfig::toolParallelMaxConcurrency) 生效
/// - T5 取消: 已完成结果保留, 未完成补 [User canceled], 每条 tool_call 都有回复
#include "agentxx-test/core/test_toolcall_parallel.h"

#include "agentxx-test/core/test_agent.h"
#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/tools/tool.h"
#include "asio/co_spawn.hpp"
#include "asio/deferred.hpp"
#include "asio/experimental/parallel_group.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tp_passed = 0;
int g_tp_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tp_passed
#define XX_TEST_FAILED g_tp_failed

namespace agentxx {
namespace test {

namespace {

int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

/// 一次执行的开始/结束时刻 (秒表: 用同一 steady_clock 记相对时间)
struct TraceEntry {
    std::string name;
    bool        isStart = true;
    int64_t     ms      = 0;
};

using Trace = std::shared_ptr<std::vector<TraceEntry>>;

/// 记录执行区间的测试工具
/// - `sleepMs`: 执行体睡眠时长 (模拟耗时工具, 用于观察并发与串行差异)
/// - `parallelSafe`: 是否声明并行安全 (见 XXToolBase::supportsParallel)
class TraceTool : public agentxx::tools::XXToolBase {
public:

    TraceTool(
        std::string_view name,
        Trace            trace,
        int64_t          sleepMs,
        bool             parallelSafe
    ) :
        XXToolBase(
            name,
            std::weak_ptr<agentxx::agent::AgentContext>{},
            /*in_autoSummaryOutput=*/false,
            /*in_canDelayLoad=*/false,
            /*in_maxRetry=*/0,
            /*in_repeatCallCheck=*/false,
            parallelSafe
        ),
        name_(name),
        trace_(std::move(trace)),
        sleepMs_(sleepMs) {}

    neograph::ChatTool get_definition() const override {
        return neograph::ChatTool{
            .name        = name_,
            .description = "trace tool for toolcall parallel test",
            .parameters  = neograph::json{{"type", "object"}, {"properties", neograph::json::object()}},
        };
    }

    asio::awaitable<std::string> execute_async(const utilxx_base::Json&) override {
        trace_->push_back(TraceEntry{name_, true, steadyNowMs()});
        if (sleepMs_ > 0) {
            asio::steady_timer timer{co_await asio::this_coro::executor};
            timer.expires_after(std::chrono::milliseconds{sleepMs_});
            co_await timer.async_wait(asio::use_awaitable);
        }
        trace_->push_back(TraceEntry{name_, false, steadyNowMs()});
        co_return fmt::format("{} done", name_);
    }

private:

    std::string name_;
    Trace       trace_;
    int64_t     sleepMs_ = 0;
};

/// 测试用 agent: 在默认工具之外注入本模块的轨迹工具
class ParallelTestAgent : public agentxx::agent::CodeAgent {
public:

    ParallelTestAgent(
        std::shared_ptr<agentxx::agent::AgentConfig>   cfg,
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

/// 构造一个 tool_call 声明 (OpenAI 流式 delta 形态; 与模拟器约定一致)
utilxx_base::Json makeToolCall(int index, std::string_view id, std::string_view name) {
    return utilxx_base::Json{
        {"index", index},
        {"id", std::string{id}},
        {"type", "function"},
        {"function", utilxx_base::Json{{"name", std::string{name}}, {"arguments", "{}"}}},
    };
}

/// 取某工具的执行区间 (开始/结束时刻); 轨迹缺失返回 false
bool intervalOf(const Trace& trace, std::string_view name, int64_t& startMs, int64_t& endMs) {
    bool foundStart = false;
    bool foundEnd   = false;
    for (const auto& e : *trace) {
        if (e.name != name) {
            continue;
        }
        if (e.isStart && !foundStart) {
            startMs    = e.ms;
            foundStart = true;
        } else if (!e.isStart) {
            endMs    = e.ms;
            foundEnd = true;
        }
    }
    return foundStart && foundEnd;
}

/// 两个执行区间是否重叠 (端点相接不算重叠)
bool intervalsOverlap(int64_t aStart, int64_t aEnd, int64_t bStart, int64_t bEnd) {
    return aStart < bEnd && bStart < aEnd;
}

/// 轨迹中同时执行的工具数峰值 (按开始/结束事件扫描)
size_t maxConcurrentOf(const Trace& trace) {
    std::vector<std::pair<int64_t, int>> events;
    events.reserve(trace->size());
    for (const auto& e : *trace) {
        events.emplace_back(e.ms, e.isStart ? 1 : -1);
    }
    std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) {
            return a.first < b.first;
        }
        // 同一时刻先处理结束: 端点相接不计入并发
        return a.second < b.second;
    });
    size_t current = 0;
    size_t peak    = 0;
    for (const auto& [_, delta] : events) {
        if (delta > 0) {
            ++current;
            peak = std::max(peak, current);
        } else {
            --current;
        }
    }
    return peak;
}

/// 取会话上下文中"最后一条带 tool_calls 的 assistant"之后连续的 tool 结果
/// `return` <tool_call_id, content> 列表 (按上下文顺序)
std::vector<std::pair<std::string, std::string>>
    toolResultsAfterLastAssistant(const std::shared_ptr<agentxx::agent::Session>& session) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!session) {
        return out;
    }
    const auto& messages = session->messages();
    size_t      assistantIndex = messages.size();
    for (size_t i = messages.size(); i > 0; --i) {
        const auto& msg = messages[i - 1];
        if (msg.role == "assistant" && !msg.tool_calls.empty()) {
            assistantIndex = i - 1;
            break;
        }
    }
    if (assistantIndex >= messages.size()) {
        return out;
    }
    for (size_t i = assistantIndex + 1; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        if (msg.role == "tool") {
            out.emplace_back(msg.tool_call_id, msg.content);
        } else if (msg.role == "assistant") {
            break;
        }
    }
    return out;
}

/// 声明一次 LLM 响应 (若干 tool_call; 模拟器响应一次后自动清空, 下一次返回文本)
void declareToolCalls(const std::vector<std::pair<std::string, std::string>>& idAndName) {
    utilxx_base::Json calls = utilxx_base::Json::array();
    int               index = 0;
    for (const auto& [id, name] : idAndName) {
        calls.push_back(makeToolCall(index++, id, name));
    }
    g_da_sim_tool_calls = std::move(calls);
}

/// 等待会话上下文里出现 [count] 条 tool 结果 (取消路径由轮末收敛, 需轮询)
asio::awaitable<bool> waitToolResults(
    const std::shared_ptr<agentxx::agent::Session>& session,
    size_t                                          count,
    int                                             timeoutMs = 5000
) {
    asio::steady_timer timer{co_await asio::this_coro::executor};
    const auto         deadline = std::chrono::steady_clock::now()
                                  + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline) {
        if (toolResultsAfterLastAssistant(session).size() >= count) {
            co_return true;
        }
        timer.expires_after(std::chrono::milliseconds{20});
        co_await timer.async_wait(asio::use_awaitable);
    }
    co_return toolResultsAfterLastAssistant(session).size() >= count;
}

} // namespace

// ===========================================================================
// T1/T2: 并行安全工具并发执行 + 结果按声明顺序写回
// ===========================================================================
asio::awaitable<void> test_parallel_tools_run_concurrently() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = baseUrl;
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";
    cfg->toolParallelMaxConcurrency = 4;

    g_da_sim_response_content = "All tools done.";
    g_da_sim_delay_ms         = 0;
    // 声明顺序 a → b → c, 睡眠时长 80/10/40ms: 完成顺序必然与声明顺序不同
    declareToolCalls({{"call_a", "test_par_a"}, {"call_b", "test_par_b"}, {"call_c", "test_par_c"}});

    auto trace = std::make_shared<std::vector<TraceEntry>>();
    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> tools;
    tools.push_back(std::make_unique<TraceTool>("test_par_a", trace, 80, true));
    tools.push_back(std::make_unique<TraceTool>("test_par_b", trace, 10, true));
    tools.push_back(std::make_unique<TraceTool>("test_par_c", trace, 40, true));

    ParallelTestAgent agent(cfg, std::move(tools));
    co_await agent.init();

    const auto result = co_await agent.runTurnAsync("toolcall_par_1", "Run tools", nullptr);
    XX_TEST_EXPECT_FALSE(result.hasError);

    // 1) 并发: b 在 a 结束前已经开始 (区间重叠), 说明同批并发推进
    int64_t aStart = 0, aEnd = 0, bStart = 0, bEnd = 0, cStart = 0, cEnd = 0;
    XX_TEST_EXPECT_TRUE(intervalOf(trace, "test_par_a", aStart, aEnd));
    XX_TEST_EXPECT_TRUE(intervalOf(trace, "test_par_b", bStart, bEnd));
    XX_TEST_EXPECT_TRUE(intervalOf(trace, "test_par_c", cStart, cEnd));
    XX_TEST_EXPECT_TRUE(intervalsOverlap(aStart, aEnd, bStart, bEnd));
    XX_TEST_EXPECT_TRUE(intervalsOverlap(aStart, aEnd, cStart, cEnd));
    XX_TEST_EXPECT_EQ(maxConcurrentOf(trace), size_t{3});

    // 2) 完成顺序与声明顺序不同 (b/c 先于 a 完成)
    XX_TEST_EXPECT_TRUE(bEnd <= cEnd);
    XX_TEST_EXPECT_TRUE(cEnd < aEnd);

    // 3) 结果仍按声明的 tool_call 顺序写回
    const auto results = toolResultsAfterLastAssistant(
        agent.agentContext->sessions->get("toolcall_par_1")
    );
    XX_TEST_EXPECT_EQ(results.size(), size_t{3});
    if (results.size() == 3) {
        XX_TEST_EXPECT_EQ(results[0].first, std::string{"call_a"});
        XX_TEST_EXPECT_EQ(results[1].first, std::string{"call_b"});
        XX_TEST_EXPECT_EQ(results[2].first, std::string{"call_c"});
        XX_TEST_EXPECT_EQ(results[0].second, std::string{"test_par_a done"});
        XX_TEST_EXPECT_EQ(results[1].second, std::string{"test_par_b done"});
        XX_TEST_EXPECT_EQ(results[2].second, std::string{"test_par_c done"});
    }

    g_da_sim_tool_calls = utilxx_base::Json::array();
    sim.stop();
    co_return;
}

// ===========================================================================
// T3: 未声明并行安全的工具形成顺序屏障
// ===========================================================================
asio::awaitable<void> test_exclusive_tool_is_barrier() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = baseUrl;
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";
    cfg->toolParallelMaxConcurrency = 4;

    g_da_sim_response_content = "Barrier tools done.";
    g_da_sim_delay_ms         = 0;
    // 声明顺序: 并行安全 p1 → 独占 w → 并行安全 p2
    declareToolCalls({{"call_p1", "test_bar_p1"}, {"call_w", "test_bar_w"}, {"call_p2", "test_bar_p2"}});

    auto trace = std::make_shared<std::vector<TraceEntry>>();
    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> tools;
    tools.push_back(std::make_unique<TraceTool>("test_bar_p1", trace, 50, true));
    tools.push_back(std::make_unique<TraceTool>("test_bar_w", trace, 20, /*parallelSafe=*/false));
    tools.push_back(std::make_unique<TraceTool>("test_bar_p2", trace, 20, true));

    ParallelTestAgent agent(cfg, std::move(tools));
    co_await agent.init();

    const auto result = co_await agent.runTurnAsync("toolcall_par_2", "Run tools", nullptr);
    XX_TEST_EXPECT_FALSE(result.hasError);

    int64_t p1Start = 0, p1End = 0, wStart = 0, wEnd = 0, p2Start = 0, p2End = 0;
    XX_TEST_EXPECT_TRUE(intervalOf(trace, "test_bar_p1", p1Start, p1End));
    XX_TEST_EXPECT_TRUE(intervalOf(trace, "test_bar_w", wStart, wEnd));
    XX_TEST_EXPECT_TRUE(intervalOf(trace, "test_bar_p2", p2Start, p2End));
    // 屏障: 独占工具与前后调用都不重叠, 且严格按声明顺序先后执行
    XX_TEST_EXPECT_FALSE(intervalsOverlap(p1Start, p1End, wStart, wEnd));
    XX_TEST_EXPECT_FALSE(intervalsOverlap(wStart, wEnd, p2Start, p2End));
    XX_TEST_EXPECT_FALSE(intervalsOverlap(p1Start, p1End, p2Start, p2End));
    XX_TEST_EXPECT_TRUE(p1End <= wStart);
    XX_TEST_EXPECT_TRUE(wEnd <= p2Start);

    const auto results = toolResultsAfterLastAssistant(
        agent.agentContext->sessions->get("toolcall_par_2")
    );
    XX_TEST_EXPECT_EQ(results.size(), size_t{3});
    if (results.size() == 3) {
        XX_TEST_EXPECT_EQ(results[0].first, std::string{"call_p1"});
        XX_TEST_EXPECT_EQ(results[1].first, std::string{"call_w"});
        XX_TEST_EXPECT_EQ(results[2].first, std::string{"call_p2"});
    }

    g_da_sim_tool_calls = utilxx_base::Json::array();
    sim.stop();
    co_return;
}

// ===========================================================================
// T4: 并发上限生效 (上限 2, 声明 3 个并行安全调用)
// ===========================================================================
asio::awaitable<void> test_parallel_concurrency_limit() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = baseUrl;
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";
    cfg->toolParallelMaxConcurrency = 2;

    g_da_sim_response_content = "Limited tools done.";
    g_da_sim_delay_ms         = 0;
    declareToolCalls({{"call_l1", "test_lim_a"}, {"call_l2", "test_lim_b"}, {"call_l3", "test_lim_c"}});

    auto trace = std::make_shared<std::vector<TraceEntry>>();
    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> tools;
    tools.push_back(std::make_unique<TraceTool>("test_lim_a", trace, 40, true));
    tools.push_back(std::make_unique<TraceTool>("test_lim_b", trace, 40, true));
    tools.push_back(std::make_unique<TraceTool>("test_lim_c", trace, 40, true));

    ParallelTestAgent agent(cfg, std::move(tools));
    co_await agent.init();

    const auto result = co_await agent.runTurnAsync("toolcall_par_3", "Run tools", nullptr);
    XX_TEST_EXPECT_FALSE(result.hasError);

    // 同时执行数不超过配置上限 (3 个调用分两批: 2 + 1)
    XX_TEST_EXPECT_EQ(maxConcurrentOf(trace), size_t{2});

    const auto results = toolResultsAfterLastAssistant(
        agent.agentContext->sessions->get("toolcall_par_3")
    );
    XX_TEST_EXPECT_EQ(results.size(), size_t{3});
    if (results.size() == 3) {
        XX_TEST_EXPECT_EQ(results[0].first, std::string{"call_l1"});
        XX_TEST_EXPECT_EQ(results[1].first, std::string{"call_l2"});
        XX_TEST_EXPECT_EQ(results[2].first, std::string{"call_l3"});
    }

    g_da_sim_tool_calls = utilxx_base::Json::array();
    sim.stop();
    co_return;
}

// ===========================================================================
// T5: 取消: 已完成结果保留, 未完成补 [User canceled], 顺序完整
// ===========================================================================
asio::awaitable<void> test_parallel_cancel_keeps_completed() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = baseUrl;
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";
    cfg->toolParallelMaxConcurrency = 4;

    g_da_sim_response_content = "";
    g_da_sim_delay_ms         = 0;
    // 声明顺序: 快 (并行安全, 完成) → 慢 (并行安全, 取消时仍在跑) → 独占 (未启动)
    declareToolCalls({{"call_fast", "test_can_fast"}, {"call_slow", "test_can_slow"}, {"call_never", "test_can_never"}});

    auto trace = std::make_shared<std::vector<TraceEntry>>();
    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> tools;
    tools.push_back(std::make_unique<TraceTool>("test_can_fast", trace, 30, true));
    tools.push_back(std::make_unique<TraceTool>("test_can_slow", trace, 5000, true));
    tools.push_back(std::make_unique<TraceTool>("test_can_never", trace, 10, /*parallelSafe=*/false));

    ParallelTestAgent agent(cfg, std::move(tools));
    co_await agent.init();

    auto ex = co_await asio::this_coro::executor;

    // 等快工具执行完成 (观察到结束记录)、慢工具已开始后再取消:
    // 此时快的已完成应保留、慢的执行中被取消、独占的尚未启动
    auto cancelWatcher = [&]() -> asio::awaitable<void> {
        asio::steady_timer timer(ex);
        const auto         deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            bool fastDone     = false;
            bool slowStarted  = false;
            for (const auto& e : *trace) {
                if (e.name == "test_can_fast" && !e.isStart) {
                    fastDone = true;
                }
                if (e.name == "test_can_slow" && e.isStart) {
                    slowStarted = true;
                }
            }
            if (fastDone && slowStarted) {
                break;
            }
            timer.expires_after(std::chrono::milliseconds{5});
            co_await timer.async_wait(asio::use_awaitable);
        }
        auto session = agent.agentContext->sessions->get("toolcall_par_4");
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
              asio::co_spawn(
                  ex,
                  agent.runTurnAsync("toolcall_par_4", "Run tools", nullptr),
                  asio::deferred
              ),
              asio::co_spawn(ex, cancelWatcher(), asio::deferred)
        )
              .async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
    (void)order;

    XX_TEST_EXPECT_TRUE(turnExc == nullptr);
    XX_TEST_EXPECT_TRUE(watcherExc == nullptr);
    XX_TEST_EXPECT_TRUE(turnResult.hasError);
    XX_TEST_EXPECT_EQ(turnResult.errorMessage, std::string{"Cancelled by user"});

    // 取消时独占工具未启动: 其执行体不应被调用
    const bool neverRan = std::none_of(
        trace->begin(),
        trace->end(),
        [](const TraceEntry& e) { return e.name == "test_can_never"; }
    );
    XX_TEST_EXPECT_TRUE(neverRan);

    // 已完成的快工具结果保留; 慢工具与未启动的独占工具补 [User canceled]
    auto session = agent.agentContext->sessions->get("toolcall_par_4");
    XX_TEST_EXPECT_TRUE(co_await waitToolResults(session, 3));
    const auto results = toolResultsAfterLastAssistant(session);
    XX_TEST_EXPECT_EQ(results.size(), size_t{3});
    if (results.size() == 3) {
        XX_TEST_EXPECT_EQ(results[0].first, std::string{"call_fast"});
        XX_TEST_EXPECT_EQ(results[0].second, std::string{"test_can_fast done"});
        XX_TEST_EXPECT_EQ(results[1].first, std::string{"call_slow"});
        XX_TEST_EXPECT_EQ(results[1].second, std::string{"[User canceled]"});
        XX_TEST_EXPECT_EQ(results[2].first, std::string{"call_never"});
        XX_TEST_EXPECT_EQ(results[2].second, std::string{"[User canceled]"});
    }

    g_da_sim_tool_calls = utilxx_base::Json::array();
    sim.stop();
    co_return;
}

asio::awaitable<TestResult> run_toolcall_parallel_tests() {
    g_tp_passed = 0;
    g_tp_failed = 0;

    try {
        co_await test_parallel_tools_run_concurrently();
        co_await test_exclusive_tool_is_barrier();
        co_await test_parallel_concurrency_limit();
        co_await test_parallel_cancel_keeps_completed();
    } catch (const std::exception& e) {
        TEST_FAIL << "toolcall_parallel suite exception: " << e.what() << std::endl;
        g_tp_failed++;
    }

    co_return TestResult{g_tp_passed, g_tp_failed};
}

} // namespace test
} // namespace agentxx
