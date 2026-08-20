// 注意: test_agent.h 会重定义 XX_TEST_PASSED/XX_TEST_FAILED 宏, 必须在
// test_message_supplement.h 之前包含, 保证本文件的断言计入 g_ms_* 计数器
#include "test_agent.h"

#include "test_message_supplement.h"

#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/tools/tool.h"
#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/deferred.hpp"
#include "asio/experimental/parallel_group.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "neograph/graph/cancel.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace agentxx {
namespace test {

int g_ms_passed = 0;
int g_ms_failed = 0;

// ===========================================================================
// 测试目标: 中断 / 取消(超时停止) 后 BaseAgent 自动补充的消息
// (实现位于 agent/lib/src/nodes/toolcall.cpp)
//
// 1. 中断 (tool 触发 NodeInterrupt):
//    - 触发中断的 tool 结果自动补充为 [Interrupt] 占位消息
//      (role=tool, flags=Interrupt), 保证 assistant tool_call 有对应 tool 回复
//    - 中断处理并 resume 后, 该 tool 重新执行返回真实结果, 最终上下文
//      角色顺序: system -> user -> assistant(tool_calls) -> tool -> assistant
// 2. 取消 / 超时停止 (CancelToken 取消):
//    - 串行 toolcall 下, 取消时未完成的 tool 全部自动补充 [User canceled]
//      tool 消息 (role=tool, flags=AutoInserted, 按 tool_calls 声明顺序)
//    - 最终上下文角色顺序完整, 每条 assistant tool_call 都有 tool 回复
// ===========================================================================

/// 中断 E2E 专用 IO:
/// - handleInterrupt 在中断处理时刻校验自动补充的 [Interrupt] 消息
///   (角色/内容/flag/关联 tool_call_id/tool_name)
/// - 同时校验中断时刻保存的上下文 (tempMessages) 角色顺序
/// - 返回固定结果驱动 resume
class InterruptMockIO : public agentxx::agent::AgentIOBase {
public:

    // 注意: 必须用 weak_ptr 持有 AgentContext!
    // 否则 io 被 BaseAgent 存入 session->io (shared_ptr) 后, 会形成
    // AgentContext -> Session::io -> InterruptMockIO -> AgentContext 循环引用,
    // 导致 AgentContext 整棵树无法释放 (ASan 报告内存泄漏)
    std::weak_ptr<agentxx::agent::AgentContext> agentContext;
    std::atomic<bool>                           interruptMsgOk{false};
    std::atomic<bool>                           interruptTempOrderOk{false};
    std::atomic<int>                            interruptCalls{0};

    explicit InterruptMockIO(std::shared_ptr<agentxx::agent::AgentContext> ctx) :
        agentContext(ctx) {}

    void onDelta(const agentxx::agent::Delta&) override {}

    void onSync(const agentxx::agent::SyncPayload&) override {}

    asio::awaitable<std::optional<std::string>> getInput() override {
        co_return std::nullopt;
    }

    asio::awaitable<neograph::json> handleInterrupt(
        std::string_view sessionId,
        std::string_view /*interruptNode*/,
        std::string_view /*interruptValue*/,
        std::string_view /*interruptArgJson*/
    ) override {
        ++interruptCalls;
        auto ctx = agentContext.lock();
        if (!ctx || !ctx->middlewareHandleContext) {
            co_return neograph::json{};
        }
        auto& graphData = ctx->middlewareHandleContext;

        // 1) 中断时刻自动补充的 [Interrupt] tool 消息:
        //    - 触发中断的 tool 结果以 [Interrupt] 占位 (role=tool), 保证
        //      assistant tool_call 有对应 tool 回复, 角色顺序完整
        auto cache = graphData->getGraphDataItemValue<neograph::json>(
            sessionId,
            agentxx::middleware::MiddlewareContext::graphDataKey_interruptToolcallCache
        );
        if (cache.is_array() && cache.size() == 1) {
            const auto& m = cache[0];
            const auto  flags
                = static_cast<neograph::MessageFlag>(m.value<unsigned long long>("flags", 0));
            interruptMsgOk = m.value("role", std::string{}) == "tool"
                             && m.value("content", std::string{}) == "[Interrupt]"
                             && m.value("toolCallId", std::string{}) == "call_it_1"
                             && m.value("toolName", std::string{}) == "test_interrupt"
                             && neograph::hasFlag(flags, neograph::MessageFlag::Interrupt);
        }

        // 2) 中断时刻上下文 (wrap_handle 保存的 tempMessages):
        //    角色顺序应为 system -> user -> assistant(tool_calls)
        //    (modelcall 节点会在 state 头部补充 system 消息)
        auto temp = graphData->getGraphDataItemValue<neograph::json>(
            sessionId,
            agentxx::middleware::MiddlewareContext::graphDataKey_tempMessages
        );
        if (temp.is_array() && temp.size() == 3) {
            interruptTempOrderOk
                = temp[0].value("role", std::string{}) == "system"
                  && temp[1].value("role", std::string{}) == "user"
                  && temp[2].value("role", std::string{}) == "assistant"
                  && temp[2]["tool_calls"].is_array() && temp[2]["tool_calls"].size() == 1
                  && temp[2]["tool_calls"][0].value("id", std::string{}) == "call_it_1";
        }

        co_return neograph::json(std::string{"handled"});
    }
};

/// 触发中断的 tool: 首次调用经 requestInterrupt 存储参数并抛 NodeInterrupt,
/// resume 后从 interruptResult 按 resultId 提取结果返回 (与 subagent 工具同模式)
class InterruptTriggerTool : public agentxx::tools::XXToolBase {
public:

    explicit InterruptTriggerTool(std::weak_ptr<agentxx::agent::AgentContext> ctx) :
        XXToolBase("test_interrupt", ctx, false, false) {}

    neograph::ChatTool get_definition() const override {
        return neograph::ChatTool{
            .name        = "test_interrupt",
            .description = "trigger interrupt for supplement test",
            .parameters  = neograph::json{{"type", "object"}},
        };
    }

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override {
        auto agentCtxPtr = agentContext.lock();
        if (!agentCtxPtr || !agentCtxPtr->middlewareHandleContext) {
            co_return R"({"error":"AgentContext not available"})";
        }
        auto sessionId = arguments.value("session_id", std::string{});
        auto resultId  = arguments.value("toolCallId", std::string{});

        // 通过 requestInterrupt 触发/恢复中断:
        // - 首次: 存储中断参数到 graphData, 抛 NodeInterrupt
        // - 恢复: 从 graphData 读取中断结果, 按 resultId 提取
        auto result = co_await agentCtxPtr->middlewareHandleContext->requestInterrupt(
            sessionId,
            [&]() {
                return agentxx::middleware::InterruptHandleArg{
                    .name     = agentxx::middleware::MiddlewareContext::interruptHandleName_default,
                    .arg      = neograph::json{{"question", "approve?"}},
                    .resultId = resultId,
                };
            },
            nullptr
        );

        // interruptResult 存储的是 {resultId: value} map; 按自身 resultId 提取
        if (result.is_object() && !resultId.empty() && result.contains(resultId)) {
            auto val = result[resultId];
            if (val.is_string()) {
                co_return val.get<std::string>();
            }
            co_return val.dump();
        }
        if (result.is_string()) {
            co_return result.get<std::string>();
        }
        co_return result.dump();
    }
};

/// 快速 tool: 排在 slow 之后声明 (串行 toolcall 下取消后不再执行,
/// 同样被补充 [User canceled])
class FastDoneTool : public agentxx::tools::XXToolBase {
public:

    explicit FastDoneTool(std::weak_ptr<agentxx::agent::AgentContext> ctx) :
        XXToolBase("test_fast", ctx, false, false) {}

    neograph::ChatTool get_definition() const override {
        return neograph::ChatTool{
            .name        = "test_fast",
            .description = "fast tool for cancel supplement test",
            .parameters  = neograph::json{{"type", "object"}},
        };
    }

    asio::awaitable<std::string> execute_async(const neograph::json&) override {
        co_return "fast done";
    }
};

/// 慢速 tool: 模拟耗时 IO, 取消时未完成 → 应自动补充 [User canceled]
class SlowHangTool : public agentxx::tools::XXToolBase {
public:

    SlowHangTool(std::weak_ptr<agentxx::agent::AgentContext> ctx, std::atomic<bool>* started) :
        XXToolBase("test_slow", ctx, false, false),
        started_(started) {}

    neograph::ChatTool get_definition() const override {
        return neograph::ChatTool{
            .name        = "test_slow",
            .description = "slow tool for cancel supplement test",
            .parameters  = neograph::json{{"type", "object"}},
        };
    }

    asio::awaitable<std::string> execute_async(const neograph::json&) override {
        started_->store(true, std::memory_order_release);
        // 模拟耗时异步 IO: 最长等待 2s (取消时被取消语义中断, 未完成
        // → 自动补充 [User canceled])
        asio::steady_timer timer(co_await asio::this_coro::executor, std::chrono::seconds(2));
        co_await timer.async_wait(asio::use_awaitable);
        co_return "slow done";
    }

private:

    std::atomic<bool>* started_;
};

/// 测试用 Agent: 注入 中断/快速/慢速 tool
class SupplementTestAgent : public agentxx::agent::CodeAgent {
public:

    std::atomic<bool> slowStarted{false};

    explicit SupplementTestAgent(std::shared_ptr<agentxx::agent::AgentConfig> cfg) :
        CodeAgent(std::move(cfg)) {}

protected:

    asio::awaitable<std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>> initTools() override {
        auto tools = co_await CodeAgent::initTools();
        tools.push_back(std::make_unique<InterruptTriggerTool>(agentContext));
        tools.push_back(std::make_unique<FastDoneTool>(agentContext));
        tools.push_back(std::make_unique<SlowHangTool>(agentContext, &slowStarted));
        co_return tools;
    }
};

// ===========================================================================
// 中断 E2E: tool 触发 NodeInterrupt
// - 中断时刻: 自动补充 [Interrupt] tool 消息 (角色/内容/flag/关联 id 正确)
// - 中断处理后 resume: 最终上下文角色顺序正确, tool 回复为真实中断结果
// ===========================================================================
asio::awaitable<void> test_interrupt_auto_supplement() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = baseUrl;
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";

    g_da_sim_response_content = "Final answer after interrupt.";
    g_da_sim_delay_ms         = 0;
    // 首次 LLM 调用返回 tool_call; 响应后 sim 自动清空, 第二次调用返回 content
    g_da_sim_tool_calls = neograph::json::array({
        neograph::json{
                       {"index", 0},
                       {"id", "call_it_1"},
                       {"type", "function"},
                       {"function",
             neograph::json{
                 {"name", "test_interrupt"},
                 {"arguments", "{}"},
             }},
                       },
    });

    SupplementTestAgent agent(cfg);
    co_await agent.init();

    auto io     = std::make_shared<InterruptMockIO>(agent.agentContext);
    auto result = co_await agent.runTurnAsync("interrupt_msg_test", "Please interrupt", true, io);

    // 中断被处理并 resume, 轮次正常完成 (interrupted 标记 true, 无错误)
    XX_TEST_EXPECT_FALSE(result.hasError);
    XX_TEST_EXPECT_TRUE(result.interrupted);

    // 中断处理时刻: IO 的 handleInterrupt 内已完成校验
    XX_TEST_EXPECT_EQ(io->interruptCalls.load(), 1);
    XX_TEST_EXPECT_TRUE(io->interruptMsgOk.load());
    XX_TEST_EXPECT_TRUE(io->interruptTempOrderOk.load());

    // resume 后最终上下文: 角色顺序 system -> user -> assistant(tool_calls)
    // -> tool(真实中断结果) -> assistant(最终回复)
    auto session = agent.agentContext->sessions->get("interrupt_msg_test");
    XX_TEST_EXPECT_TRUE(session != nullptr);
    if (session) {
        const auto& msgs = session->llmMessages;
        XX_TEST_EXPECT_TRUE(msgs.is_array() && msgs.size() == 5);
        if (msgs.is_array() && msgs.size() == 5) {
            XX_TEST_EXPECT_EQ(msgs[0].value("role", std::string{}), std::string{"system"});
            XX_TEST_EXPECT_EQ(msgs[1].value("role", std::string{}), std::string{"user"});
            XX_TEST_EXPECT_EQ(msgs[2].value("role", std::string{}), std::string{"assistant"});
            XX_TEST_EXPECT_EQ(msgs[3].value("role", std::string{}), std::string{"tool"});
            XX_TEST_EXPECT_EQ(msgs[4].value("role", std::string{}), std::string{"assistant"});

            // assistant tool_call 与 tool 回复按 tool_call_id 关联
            XX_TEST_EXPECT_TRUE(
                msgs[2]["tool_calls"].is_array() && msgs[2]["tool_calls"].size() == 1
            );
            if (msgs[2]["tool_calls"].is_array() && msgs[2]["tool_calls"].size() == 1) {
                XX_TEST_EXPECT_EQ(
                    msgs[2]["tool_calls"][0].value("id", std::string{}),
                    std::string{"call_it_1"}
                );
            }
            XX_TEST_EXPECT_EQ(msgs[3].value("toolCallId", std::string{}), std::string{"call_it_1"});
            XX_TEST_EXPECT_EQ(
                msgs[3].value("toolName", std::string{}),
                std::string{"test_interrupt"}
            );
            // resume 后 tool 重新执行返回真实中断结果, 不再是 [Interrupt] 占位
            XX_TEST_EXPECT_EQ(msgs[3].value("content", std::string{}), std::string{"handled"});
            // 最终 tool 消息不应残留 [Interrupt]/[AutoInserted] 标记
            const auto flags
                = static_cast<neograph::MessageFlag>(msgs[3].value<unsigned long long>("flags", 0));
            XX_TEST_EXPECT_FALSE(neograph::hasFlag(flags, neograph::MessageFlag::Interrupt));
            XX_TEST_EXPECT_FALSE(neograph::hasFlag(flags, neograph::MessageFlag::AutoInserted));

            XX_TEST_EXPECT_EQ(
                msgs[4].value("content", std::string{}),
                std::string{"Final answer after interrupt."}
            );
        }
    }

    co_return;
}

// ===========================================================================
// 取消/超时停止 E2E: tool 执行中取消
// - 串行 toolcall: slow 先执行被取消中断, 后续 fast 不再执行
// - 未完成的 tool 全部自动补充 [User canceled] (按声明顺序)
// - 最终上下文角色顺序完整: 每条 assistant tool_call 都有对应 tool 回复
// ===========================================================================

/// 校验取消后的消息序列 (串行 toolcall 语义):
/// [system, user, assistant(tool_calls=[call_slow_1, call_fast_1]),
///  tool(call_slow_1=[User canceled] AutoInserted), tool(call_fast_1=[User canceled] AutoInserted)]
/// - 串行执行: slow 先执行, 取消中断 slow 后 fast 不再执行
/// - 未完成的 tool 全部自动补充 [User canceled] (按声明顺序), 保证角色顺序完整
static bool checkCanceledMessageSequence(const neograph::json& msgs) {
    if (!msgs.is_array() || msgs.size() != 5) {
        return false;
    }
    // 角色顺序
    if (msgs[0].value("role", std::string{}) != "system") {
        return false;
    }
    if (msgs[1].value("role", std::string{}) != "user") {
        return false;
    }
    if (msgs[2].value("role", std::string{}) != "assistant") {
        return false;
    }
    if (msgs[3].value("role", std::string{}) != "tool") {
        return false;
    }
    if (msgs[4].value("role", std::string{}) != "tool") {
        return false;
    }
    // assistant 声明的 tool_calls 顺序
    if (!msgs[2]["tool_calls"].is_array() || msgs[2]["tool_calls"].size() != 2) {
        return false;
    }
    if (msgs[2]["tool_calls"][0].value("id", std::string{}) != "call_slow_1") {
        return false;
    }
    if (msgs[2]["tool_calls"][1].value("id", std::string{}) != "call_fast_1") {
        return false;
    }
    // 未完成的 tool 自动补充 [User canceled] (按 tool_calls 声明顺序):
    // - slow 先执行, 取消时在途未完成 → 补充 [User canceled]
    if (msgs[3].value("toolCallId", std::string{}) != "call_slow_1") {
        return false;
    }
    if (msgs[3].value("toolName", std::string{}) != "test_slow") {
        return false;
    }
    if (msgs[3].value("content", std::string{}) != "[User canceled]") {
        return false;
    }
    {
        const auto flags
            = static_cast<neograph::MessageFlag>(msgs[3].value<unsigned long long>("flags", 0));
        if (!neograph::hasFlag(flags, neograph::MessageFlag::AutoInserted)) {
            return false;
        }
    }
    // - fast 排在 slow 之后, 取消后未执行 → 同样补充 [User canceled]
    if (msgs[4].value("toolCallId", std::string{}) != "call_fast_1") {
        return false;
    }
    if (msgs[4].value("toolName", std::string{}) != "test_fast") {
        return false;
    }
    if (msgs[4].value("content", std::string{}) != "[User canceled]") {
        return false;
    }
    {
        const auto flags
            = static_cast<neograph::MessageFlag>(msgs[4].value<unsigned long long>("flags", 0));
        if (!neograph::hasFlag(flags, neograph::MessageFlag::AutoInserted)) {
            return false;
        }
    }
    return true;
}

asio::awaitable<void> test_cancel_auto_supplement() {
    auto sim     = startDaSimServer();
    auto baseUrl = "http://127.0.0.1:" + std::to_string(sim.port);

    auto cfg                 = std::make_shared<agentxx::agent::AgentConfig>();
    cfg->model.baseUrl       = baseUrl;
    cfg->model.apiKey        = "EMPTY";
    cfg->model.modelName     = "test-sim";
    cfg->prompt.systemPrompt = "You are a helpful assistant.";

    g_da_sim_response_content = "";
    g_da_sim_delay_ms         = 0;
    // LLM 返回两个 toolcall: 先慢速 tool, 后快速 tool
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
                       {"id", "call_fast_1"},
                       {"type", "function"},
                       {"function",
             neograph::json{
                 {"name", "test_fast"},
                 {"arguments", "{}"},
             }},
                       },
    });

    SupplementTestAgent agent(cfg);
    co_await agent.init();

    auto ex = co_await asio::this_coro::executor;

    // 串行 toolcall: 等待 slow 开始执行后再取消, 确保取消发生在 slow 执行期间:
    // - slow (2s 在途) 被取消信号立即中断, 未完成 → 补充 [User canceled]
    // - fast 排在 slow 之后, 取消后不再执行 → 同样补充 [User canceled]
    auto cancelWatcher = [&]() -> asio::awaitable<void> {
        asio::steady_timer timer(ex);
        for (int i = 0; i < 2000; ++i) {
            if (agent.slowStarted.load(std::memory_order_acquire)) {
                break;
            }
            timer.expires_after(std::chrono::milliseconds(5));
            co_await timer.async_wait(asio::use_awaitable);
        }
        auto session = agent.agentContext->sessions->get("cancel_msg_test");
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
                  agent.runTurnAsync("cancel_msg_test", "Run tools", true, nullptr),
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
    // 取消应立即中断会话轮次 (不等待慢速 tool 自然完成)
    XX_TEST_EXPECT_TRUE(elapsedMs < 8000);

    // 轮询等待自动补充消息最终保存完成 (baseRun 走 isCancel 分支后,
    // wrap_handle 在 rethrow 前写入 tempMessages)
    {
        auto               ex2 = co_await asio::this_coro::executor;
        asio::steady_timer poll(ex2);
        bool               ok = false;
        neograph::json     im;
        const auto         deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            im = agent.agentContext->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                "cancel_msg_test",
                agentxx::middleware::MiddlewareContext::graphDataKey_tempMessages
            );
            if (checkCanceledMessageSequence(im)) {
                ok = true;
                break;
            }
            poll.expires_after(std::chrono::milliseconds(20));
            co_await poll.async_wait(asio::use_awaitable);
        }
        XX_TEST_EXPECT_TRUE(ok);
    }

    co_return;
}

asio::awaitable<TestResult> run_message_supplement_tests() {
    g_ms_passed = 0;
    g_ms_failed = 0;

    try {
        co_await test_interrupt_auto_supplement();
        co_await test_cancel_auto_supplement();
    } catch (const std::exception& e) {
        TEST_FAIL << "message_supplement suite exception: " << e.what() << std::endl;
        g_ms_failed++;
    }

    co_return TestResult{g_ms_passed, g_ms_failed};
}

} // namespace test
} // namespace agentxx
