// 注意: test_agent.h 会重定义 XX_TEST_PASSED/XX_TEST_FAILED 宏, 必须在
// test_cancel.h 之前包含, 保证本文件的断言计入 g_cancel_* 计数器
#include "test_agent.h"

#include "agentxx/agent/code_agent.h"
#include "agentxx/tools/tool.h"
#include "agentxx/util/async_offload.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/deferred.hpp"
#include "asio/experimental/parallel_group.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "neograph/graph/cancel.h"
#include "neograph/tool.h"

#include "test_cancel.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace agentxx {
namespace test {

int g_cancel_passed = 0;
int g_cancel_failed = 0;

// ===========================================================================
// CancelToken 基础行为: 轮询埋点 + fork 级联
// ===========================================================================
asio::awaitable<void> test_cancel_token_checkpoint() {
    auto token = std::make_shared<neograph::graph::CancelToken>();
    XX_TEST_EXPECT_FALSE(token->is_cancelled());

    // 未取消时埋点不抛出
    bool threw = false;
    try {
        token->throw_if_cancelled("checkpoint");
    } catch (...) {
        threw = true;
    }
    XX_TEST_EXPECT_FALSE(threw);

    // fork 子令牌: 父取消级联到子
    auto child = token->fork();
    token->cancel();
    XX_TEST_EXPECT_TRUE(token->is_cancelled());
    XX_TEST_EXPECT_TRUE(child->is_cancelled());

    // 已取消时埋点抛出 CancelledException
    threw = false;
    try {
        token->throw_if_cancelled("checkpoint");
    } catch (const neograph::graph::CancelledException&) {
        threw = true;
    }
    XX_TEST_EXPECT_TRUE(threw);

    // 幂等: 重复取消不崩溃
    token->cancel();
    XX_TEST_EXPECT_TRUE(token->is_cancelled());

    co_return;
}

// ===========================================================================
// 边界转换: operation_aborted + 已取消令牌 => CancelledException
// ===========================================================================
asio::awaitable<void> test_isCancelAbort() {
    auto cancelled = std::make_shared<neograph::graph::CancelToken>();
    cancelled->cancel();
    auto active = std::make_shared<neograph::graph::CancelToken>();

    XX_TEST_EXPECT_TRUE(agentxx::util::isCancelAbort(
        neograph_asio_system_error(asio::error::operation_aborted),
        cancelled
    ));
    // 非 operation_aborted 错误码不算取消
    XX_TEST_EXPECT_FALSE(
        agentxx::util::isCancelAbort(neograph_asio_system_error(asio::error::timed_out), cancelled)
    );
    // 无令牌 / 令牌未取消时按超时处理, 不算取消
    XX_TEST_EXPECT_FALSE(agentxx::util::isCancelAbort(
        neograph_asio_system_error(asio::error::operation_aborted),
        nullptr
    ));
    XX_TEST_EXPECT_FALSE(agentxx::util::isCancelAbort(
        neograph_asio_system_error(asio::error::operation_aborted),
        active
    ));
    co_return;
}

asio::awaitable<void> test_catchError_cancel_conversion() {
    // 已取消 + operation_aborted => 抛出 CancelledException (不被吞为错误消息)
    auto token = std::make_shared<neograph::graph::CancelToken>();
    token->cancel();
    bool caughtCancel = false;
    try {
        agentxx::util::catchError<int>(
            []() -> int {
                throw neograph_asio_system_error(asio::error::operation_aborted);
            },
            [](std::string) -> int {
                return -1;
            },
            nullptr,
            token
        );
    } catch (const neograph::graph::CancelledException&) {
        caughtCancel = true;
    }
    XX_TEST_EXPECT_TRUE(caughtCancel);

    // 未取消 => 按超时错误处理
    auto active = std::make_shared<neograph::graph::CancelToken>();
    auto errMsg = std::string{};
    auto result = agentxx::util::catchError<int>(
        []() -> int {
            throw neograph_asio_system_error(asio::error::operation_aborted);
        },
        [&](std::string msg) -> int {
            errMsg = std::move(msg);
            return -1;
        },
        nullptr,
        active
    );
    XX_TEST_EXPECT_EQ(result, -1);
    XX_TEST_EXPECT_TRUE(errMsg.starts_with("timeout:"));
    co_return;
}

asio::awaitable<void> test_catchErrorAsync_cancel_conversion() {
    // 1. 已取消 + operation_aborted + 无 onRethrow => CancelledException 向上传播
    {
        auto token = std::make_shared<neograph::graph::CancelToken>();
        token->cancel();
        bool caughtCancel = false;
        try {
            co_await agentxx::util::catchErrorAsync<bool>(
                []() -> asio::awaitable<bool> {
                    throw neograph_asio_system_error(asio::error::operation_aborted);
                    co_return true;
                },
                [](std::string) -> asio::awaitable<bool> {
                    co_return false;
                },
                nullptr,
                token
            );
        } catch (const neograph::graph::CancelledException&) {
            caughtCancel = true;
        }
        XX_TEST_EXPECT_TRUE(caughtCancel);
    }

    // 2. 已取消 + operation_aborted + 有 onRethrow => 走取消处理分支
    {
        auto token = std::make_shared<neograph::graph::CancelToken>();
        token->cancel();
        bool rethrowCalled = false;
        auto r             = co_await agentxx::util::catchErrorAsync<int>(
            []() -> asio::awaitable<int> {
                throw neograph_asio_system_error(asio::error::operation_aborted);
                co_return 0;
            },
            [](std::string) -> asio::awaitable<int> {
                co_return -1;
            },
            [&](std::string&) -> std::optional<int> {
                rethrowCalled = true;
                return 42;
            },
            token
        );
        XX_TEST_EXPECT_TRUE(rethrowCalled);
        XX_TEST_EXPECT_EQ(r, 42);
    }

    // 3. 无令牌 => operation_aborted 按超时错误消息处理 (保留原行为)
    {
        auto errMsg = std::string{};
        auto r      = co_await agentxx::util::catchErrorAsync<int>(
            []() -> asio::awaitable<int> {
                throw neograph_asio_system_error(asio::error::operation_aborted);
                co_return 0;
            },
            [&](std::string msg) -> asio::awaitable<int> {
                errMsg = std::move(msg);
                co_return -1;
            }
        );
        XX_TEST_EXPECT_EQ(r, -1);
        XX_TEST_EXPECT_TRUE(errMsg.starts_with("timeout:"));
    }

    // 4. 令牌未取消 => operation_aborted 按超时错误消息处理
    {
        auto token  = std::make_shared<neograph::graph::CancelToken>();
        auto errMsg = std::string{};
        auto r      = co_await agentxx::util::catchErrorAsync<int>(
            []() -> asio::awaitable<int> {
                throw neograph_asio_system_error(asio::error::operation_aborted);
                co_return 0;
            },
            [&](std::string msg) -> asio::awaitable<int> {
                errMsg = std::move(msg);
                co_return -1;
            },
            nullptr,
            token
        );
        XX_TEST_EXPECT_EQ(r, -1);
        XX_TEST_EXPECT_TRUE(errMsg.starts_with("timeout:"));
    }

    co_return;
}

// ===========================================================================
// E2E: LLM 请求在途时取消 => "Cancelled by user", 且不等待慢速响应完成
// ===========================================================================
asio::awaitable<void> test_agent_cancel_llm_request() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = baseUrl;
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";

    g_da_sim_response_content = "Should never arrive";
    g_da_sim_tool_calls       = neograph::json::array();
    g_da_sim_delay_ms         = 5000;

    agentxx::agent::CodeAgent agent(cfg);
    co_await agent.init();

    auto ex = co_await asio::this_coro::executor;

    auto cancelWatcher = [&]() -> asio::awaitable<void> {
        asio::steady_timer timer(ex);
        // 等待 session 的取消令牌创建 (turn 已开始)
        for (int i = 0; i < 1000; ++i) {
            auto session = agent.agentContext->sessions->get("cancel_llm_test");
            if (session && session->getCancelToken()) {
                break;
            }
            timer.expires_after(std::chrono::milliseconds(5));
            co_await timer.async_wait(asio::use_awaitable);
        }
        // 稍等进入 LLM HTTP 在途状态再取消
        timer.expires_after(std::chrono::milliseconds(300));
        co_await timer.async_wait(asio::use_awaitable);
        auto session = agent.agentContext->sessions->get("cancel_llm_test");
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
                  agent.runConversationTurnAsync("cancel_llm_test", "Hello", true, nullptr),
                  asio::deferred
              ),
              asio::co_spawn(ex, cancelWatcher(), asio::deferred)
        )
              .async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - startAt
    )
                               .count();

    XX_TEST_EXPECT_TRUE(turnExc == nullptr);
    XX_TEST_EXPECT_TRUE(watcherExc == nullptr);
    XX_TEST_EXPECT_TRUE(turnResult.hasError);
    XX_TEST_EXPECT_EQ(turnResult.errorMessage, std::string{"Cancelled by user"});
    // 在途 LLM 请求应被中断, 不应等待满 5s 延迟
    XX_TEST_EXPECT_TRUE(elapsedMs < 4000);

    g_da_sim_delay_ms = 0;
    co_return;
}

// ===========================================================================
// E2E: toolcall 执行中取消 => 后续 tool 不再执行, 取消语义正确传播
// ===========================================================================

/// 慢速 tool: 模拟耗时异步 IO, 取消时定时器收到 operation_aborted 被立即中断
/// (工具协程绑定 engine 取消信号, 生命周期跟随 engine)
class CancelSlowTool : public agentxx::tools::XXToolBase {
public:

    CancelSlowTool(
        std::weak_ptr<agentxx::agent::AgentContext> ctx,
        std::atomic<bool>*                          executed,
        std::atomic<bool>*                          finished
    ) :
        XXToolBase("test_slow", ctx, false, false),
        executed_(executed),
        finished_(finished) {}

    neograph::ChatTool get_definition() const override {
        return neograph::ChatTool{
            .name        = "test_slow",
            .description = "slow tool for cancel test",
            .parameters  = neograph::json{{"type", "object"}},
        };
    }

    asio::awaitable<std::string> execute_async(const neograph::json&) override {
        executed_->store(true, std::memory_order_release);
        // 模拟耗时 IO: 2s 等待, 取消时被 operation_aborted 立即中断
        asio::steady_timer timer(co_await asio::this_coro::executor, std::chrono::seconds(2));
        co_await timer.async_wait(asio::use_awaitable);
        finished_->store(true, std::memory_order_release);
        co_return "slow done";
    }

private:

    std::atomic<bool>* executed_;
    std::atomic<bool>* finished_;
};

/// 标记 tool: 排在慢速 tool 之后声明, 验证取消后后续 tool 不再执行
class CancelMarkerTool : public agentxx::tools::XXToolBase {
public:

    CancelMarkerTool(std::weak_ptr<agentxx::agent::AgentContext> ctx, std::atomic<bool>* executed) :
        XXToolBase("test_marker", ctx, false, false),
        executed_(executed) {}

    neograph::ChatTool get_definition() const override {
        return neograph::ChatTool{
            .name        = "test_marker",
            .description = "marker tool for cancel test",
            .parameters  = neograph::json{{"type", "object"}},
        };
    }

    asio::awaitable<std::string> execute_async(const neograph::json&) override {
        // 标记 tool: 串行 toolcall 下排在 slow 之后, slow 被取消中断后不再执行
        executed_->store(true, std::memory_order_release);
        co_return "marker";
    }

private:

    std::atomic<bool>* executed_;
};

/// 测试用 Agent: 注入慢速/标记 tool
class CancelTestAgent : public agentxx::agent::CodeAgent {
public:

    std::atomic<bool> slowExecuted{false};
    std::atomic<bool> slowFinished{false};
    std::atomic<bool> markerExecuted{false};

    explicit CancelTestAgent(std::shared_ptr<agentxx::agent::AgentConfig> cfg) :
        CodeAgent(std::move(cfg)) {}

protected:

    asio::awaitable<std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>> initTools() override {
        auto tools = co_await CodeAgent::initTools();
        tools.push_back(std::make_unique<CancelSlowTool>(agentContext, &slowExecuted, &slowFinished)
        );
        tools.push_back(std::make_unique<CancelMarkerTool>(agentContext, &markerExecuted));
        co_return tools;
    }
};

asio::awaitable<void> test_agent_cancel_toolcall() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = baseUrl;
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";

    g_da_sim_response_content = "";
    g_da_sim_delay_ms         = 0;
    // LLM 返回两个 toolcall: 先慢速 tool, 后标记 tool
    g_da_sim_tool_calls = neograph::json::array({
        neograph::json{
                       {"index", 0},
                       {"id", "call_slow_1"},
                       {"type", "function"},
                       {"function",
             neograph::json{
                 {"name", "test_slow"},
                 {"arguments", "{}"},
             }},
                       },
        neograph::json{
                       {"index", 1},
                       {"id", "call_marker_1"},
                       {"type", "function"},
                       {"function",
             neograph::json{
                 {"name", "test_marker"},
                 {"arguments", "{}"},
             }},
                       },
    });

    CancelTestAgent agent(cfg);
    co_await agent.init();

    auto ex = co_await asio::this_coro::executor;

    auto cancelWatcher = [&]() -> asio::awaitable<void> {
        asio::steady_timer timer(ex);
        // 串行 toolcall: 先执行 slow (2s 在途), 取消发生在 slow 执行期间:
        // - slow 被取消信号立即中断 (未自然完成) → 经取消埋点补 [User canceled]
        // - marker 排在 slow 之后, 取消后不再执行
        for (int i = 0; i < 2000; ++i) {
            if (agent.slowExecuted.load(std::memory_order_acquire)) {
                break;
            }
            timer.expires_after(std::chrono::milliseconds(5));
            co_await timer.async_wait(asio::use_awaitable);
        }
        auto session = agent.agentContext->sessions->get("cancel_tool_test");
        if (session) {
            auto token = session->getCancelToken();
            if (token) {
                std::fprintf(stderr, "[cancel-test-dbg] cancelling token=%p\n", (void*)token.get());
                std::fflush(stderr);

                token->cancel();
            } else {
                std::fprintf(stderr, "[cancel-test-dbg] NO TOKEN\n");
            }
        } else {
            std::fprintf(stderr, "[cancel-test-dbg] NO SESSION\n");
        }
        co_return;
    };

    const auto startAt = std::chrono::steady_clock::now();
    auto [order, turnExc, turnResult, watcherExc]
        = co_await asio::experimental::make_parallel_group(
              asio::co_spawn(
                  ex,
                  agent.runConversationTurnAsync("cancel_tool_test", "Run tools", true, nullptr),
                  asio::deferred
              ),
              asio::co_spawn(ex, cancelWatcher(), asio::deferred)
        )
              .async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - startAt
    )
                               .count();

    XX_TEST_EXPECT_TRUE(turnExc == nullptr);
    XX_TEST_EXPECT_TRUE(watcherExc == nullptr);
    XX_TEST_EXPECT_TRUE(turnResult.hasError);
    XX_TEST_EXPECT_EQ(turnResult.errorMessage, std::string{"Cancelled by user"});
    // 慢速 tool 已开始执行, 但被取消立即中断 (未自然完成):
    // 工具协程绑定 engine 取消信号, 生命周期跟随 engine
    XX_TEST_EXPECT_TRUE(agent.slowExecuted.load());
    XX_TEST_EXPECT_TRUE(agent.slowFinished.load() == false);
    // 串行执行语义: 标记 tool 排在 slow 之后, slow 被取消中断后不再执行
    XX_TEST_EXPECT_TRUE(agent.markerExecuted.load() == false);
    // 取消应立即中断会话轮次 (工具随 engine 取消立即中断, 不等待自然完成)
    XX_TEST_EXPECT_TRUE(elapsedMs < 3000);

    // 取消语义: 未完成的 tool 结果丢弃并补 [User canceled], 后续 tool 不再执行
    // - 取消时工具协程随 engine 信号立即中断退出, baseRun 等待循环收满完成信号后
    //   走 isCancel 分支保存完整上下文 (wrap_handle 在 rethrow 前写入 tempMessages),
    //   因此这里轮询等待保存完成
    {
        auto               ex2 = co_await asio::this_coro::executor;
        asio::steady_timer poll(ex2);
        neograph::json     im;
        bool               slowCanceled   = false;
        bool               markerCanceled = false;
        const auto         deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            im = agent.agentContext->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                "cancel_tool_test",
                agentxx::middleware::MiddlewareContext::graphDataKey_tempMessages
            );
            slowCanceled   = false;
            markerCanceled = false;
            if (im.is_array()) {
                for (const auto& m : im) {
                    if (m.value("role", std::string{}) == "tool"
                        && m.value("tool_name", std::string{}) == "test_slow"
                        && m.value("content", std::string{}) == "[User canceled]") {
                        slowCanceled = true;
                    }
                    // 串行下取消发生在 slow 执行期间, marker 未执行, 同样补
                    // [User canceled] 保证每条 assistant tool_call 都有结果消息;
                    // 不应出现真实执行结果 "marker"
                    if (m.value("role", std::string{}) == "tool"
                        && m.value("tool_name", std::string{}) == "test_marker"
                        && m.value("content", std::string{}) == "[User canceled]") {
                        markerCanceled = true;
                    }
                }
            }
            if (slowCanceled && markerCanceled) {
                break;
            }
            poll.expires_after(std::chrono::milliseconds(20));
            co_await poll.async_wait(asio::use_awaitable);
        }
        XX_TEST_EXPECT_TRUE(slowCanceled);
        XX_TEST_EXPECT_TRUE(markerCanceled);
    }

    co_return;
}

// ===========================================================================
// offloadCancellableAsync + CancelToken: 线程池同步工作经 watcher 监听令牌提前退出
// (工作线程不挂起, asio 信号无法抢占, 验证 cancelFlag 通知链)
// ===========================================================================
asio::awaitable<void> test_offload_cancel_token() {
    asio::thread_pool pool(2);
    auto              token = std::make_shared<neograph::graph::CancelToken>();

    auto ex = co_await asio::this_coro::executor;

    // 100ms 后取消
    asio::co_spawn(
        ex,
        [token]() -> asio::awaitable<void> {
            asio::steady_timer timer(co_await asio::this_coro::executor);
            timer.expires_after(std::chrono::milliseconds(100));
            co_await timer.async_wait(asio::use_awaitable);
            token->cancel();
        },
        asio::detached
    );

    const auto startAt      = std::chrono::steady_clock::now();
    bool       caughtCancel = false;
    try {
        co_await agentxx::util::offloadCancellableAsync<int>(
            pool,
            token,
            [](std::atomic<bool>& cancelFlag) -> asio::awaitable<int> {
                // 模拟长时间同步工作 (从不挂起): 每 10ms 轮询 flag, 取消时提前退出
                for (int i = 0; i < 1000; ++i) {
                    if (cancelFlag.load(std::memory_order_acquire)) {
                        throw neograph::graph::CancelledException("offload cancelled");
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                co_return 42;
            }
        );
    } catch (const neograph::graph::CancelledException&) {
        caughtCancel = true;
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - startAt
    )
                               .count();

    XX_TEST_EXPECT_TRUE(caughtCancel);
    // watcher 轮询间隔 20ms + 工作线程轮询间隔 10ms, 取消后应很快退出, 远小于工作满 10s
    XX_TEST_EXPECT_TRUE(elapsedMs < 5000);

    // 已取消令牌直接创建: 工作线程应立即检测到 flag 退出
    auto cancelledToken = std::make_shared<neograph::graph::CancelToken>();
    cancelledToken->cancel();
    bool caughtCancel2 = false;
    try {
        co_await agentxx::util::offloadCancellableAsync<int>(
            pool,
            cancelledToken,
            [](std::atomic<bool>& cancelFlag) -> asio::awaitable<int> {
                if (cancelFlag.load(std::memory_order_acquire)) {
                    throw neograph::graph::CancelledException("offload cancelled");
                }
                co_return 42;
            }
        );
    } catch (const neograph::graph::CancelledException&) {
        caughtCancel2 = true;
    }
    XX_TEST_EXPECT_TRUE(caughtCancel2);

    pool.stop();
    pool.join();
    co_return;
}

asio::awaitable<TestResult> run_cancel_tests() {
    g_cancel_passed = 0;
    g_cancel_failed = 0;

    try {
        co_await test_cancel_token_checkpoint();
        co_await test_isCancelAbort();
        co_await test_catchError_cancel_conversion();
        co_await test_catchErrorAsync_cancel_conversion();
        co_await test_offload_cancel_token();
        co_await test_agent_cancel_llm_request();
        co_await test_agent_cancel_toolcall();
    } catch (const std::exception& e) {
        TEST_FAIL << "cancel suite exception: " << e.what() << std::endl;
        g_cancel_failed++;
    }

    co_return TestResult{g_cancel_passed, g_cancel_failed};
}

} // namespace test
} // namespace agentxx
