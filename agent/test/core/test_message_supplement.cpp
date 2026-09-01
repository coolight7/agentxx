#include "test_agent.h"

#include "test_message_supplement.h"

#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/nodes/modelcall.h"
#include "agentxx/tools/tool.h"
#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/deferred.hpp"
#include "asio/experimental/parallel_group.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "neograph/graph/cancel.h"
#include "neograph/graph/loader.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "fmt/format.h"

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ms_passed = 0;
int g_ms_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_ms_passed
#define XX_TEST_FAILED g_ms_failed

namespace agentxx {
namespace test {

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

    void onDelta(const agentxx::agent::WireDelta&) override {}

    void onSync(const agentxx::agent::WireSyncPayload&) override {}

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
                             && m.value("tool_call_id", std::string{}) == "call_it_1"
                             && m.value("tool_name", std::string{}) == "test_interrupt"
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
        auto sessionId = arguments.value("sessionId", std::string{});
        auto resultId  = arguments.value("tool_call_id", std::string{});

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
    auto result = co_await agent.runTurnAsync("interrupt_msg_test", "Please interrupt", io);

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
            XX_TEST_EXPECT_EQ(
                msgs[3].value("tool_call_id", std::string{}),
                std::string{"call_it_1"}
            );
            XX_TEST_EXPECT_EQ(
                msgs[3].value("tool_name", std::string{}),
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
    if (msgs[3].value("tool_call_id", std::string{}) != "call_slow_1") {
        return false;
    }
    if (msgs[3].value("tool_name", std::string{}) != "test_slow") {
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
    if (msgs[4].value("tool_call_id", std::string{}) != "call_fast_1") {
        return false;
    }
    if (msgs[4].value("tool_name", std::string{}) != "test_fast") {
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
                  agent.runTurnAsync("cancel_msg_test", "Run tools", nullptr),
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
            // 轮末错误路径已把 tempMessages 快照收敛进 llmMessages 并清理
            // graphData, 断言权威面 (llmMessages) 即可
            auto sess = agent.agentContext->sessions->get("cancel_msg_test");
            im        = sess ? sess->llmMessages : neograph::json{};
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

// ===========================================================================
// repairMessages 悬挂 toolcall 清理 (实现位于 agent/lib/src/nodes/modelcall.cpp)
//
// 背景: 取消/异常后重新从图开始节点执行时, 上下文可能以
//   "assistant(tool_calls) 但无对应 tool 结果" 结尾 (悬挂 tool_calls)。
//   repairMessages 会清空该 assistant 的 tool_calls 声明; 但清空后,
//   assistant 之后引用这些 id 的 tool 结果消息成为孤儿 (无对应声明),
//   会让 LLM API 校验失败 (OpenAI invalid tool_call_id / Anthropic orphan
//   tool result 400), 也应一并删除, 删除范围直到下一条 agent 消息
//   (user/system 或 无 tool_calls 的 assistant) 为止。
// ===========================================================================

/// 构造悬挂上下文, 直接调用 ModelCallWrapNode::repairMessages 验证清理结果
/// - assistant 声明 [danglingCount] 个 tool_call (id: call_a, call_b, ...),
///   其中仅 [repliedCount] 个有对应 tool 结果
/// - [followUpRole] 悬挂结果之后的下一条 agent 消息 role
///   ("" 表示没有后续消息)
/// - [expectDanglingCleared] 期望悬挂 tool_calls 被清空
/// - [expectOrphanRemoved] 期望已回复的 tool 结果被删除
///   (悬挂清空后它们成为孤儿, 一并清理)
/// - [expectFollowUpKept] 期望下一条 agent 消息被保留
static void runRepairDanglingScenario(
    size_t      danglingCount,
    size_t      repliedCount,
    std::string followUpRole,
    bool        expectDanglingCleared,
    bool        expectOrphanRemoved,
    bool        expectFollowUpKept
) {
    // ---- 构造 AgentContext ----
    auto ctx          = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig  = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->agentConfig->repairMessages = true;
    ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();

    // ---- 构造 ModelCallWrapNode ----
    neograph::graph::NodeContext nodeCtx;
    nodeCtx.model = "test-model";
    agentxx::nodes::ModelCallWrapNode node("test_llm", nodeCtx, ctx);

    // ---- 构造 state: 悬挂上下文 ----
    // [system, user, assistant(tool_calls=[call_a..]), tool(已回复)xN, <followUp>]
    neograph::json msgs = neograph::json::array();
    msgs.push_back(neograph::json{{"role", "system"}, {"content", "sys"}});
    msgs.push_back(neograph::json{{"role", "user"}, {"content", "hello"}});

    auto toolCalls = neograph::json::array();
    for (size_t i = 0; i < danglingCount; ++i) {
        toolCalls.push_back(neograph::json{
            {"id", fmt::format("call_{}", char('a' + i))},
            {"type", "function"},
            {"function", neograph::json{{"name", "tool_a"}, {"arguments", "{}"}}},
        });
    }
    msgs.push_back(neograph::json{
        {"role", "assistant"},
        {"content", "calling tools"},
        {"tool_calls", std::move(toolCalls)},
    });
    for (size_t i = 0; i < repliedCount; ++i) {
        msgs.push_back(neograph::json{
            {"role", "tool"},
            {"content", "result"},
            {"tool_call_id", fmt::format("call_{}", char('a' + i))},
            {"tool_name", "tool_a"},
        });
    }
    if (!followUpRole.empty()) {
        msgs.push_back(neograph::json{{"role", followUpRole}, {"content", "next"}});
    }

    neograph::graph::GraphState state;
    state.init_channel(
        "messages",
        neograph::graph::ReducerType::APPEND,
        neograph::graph::ReducerRegistry::instance().get("append"),
        msgs
    );

    neograph::graph::RunContext runCtx;
    runCtx.thread_id = "repair_dangling_test";
    neograph::graph::NodeInput in{state, runCtx, nullptr};

    // ---- 调用 repairMessages ----
    node.repairMessages(in);

    // ---- 断言清理结果 ----
    const auto resultMsgs = in.state.get_messages();
    size_t     idx        = 0;
    // system + user 保留
    XX_TEST_EXPECT_TRUE(resultMsgs.size() >= 2);
    XX_TEST_EXPECT_EQ(resultMsgs[0].role, std::string{"system"});
    XX_TEST_EXPECT_EQ(resultMsgs[1].role, std::string{"user"});
    idx = 2;

    // 悬挂 assistant 消息
    XX_TEST_EXPECT_TRUE(idx < resultMsgs.size());
    if (idx < resultMsgs.size()) {
        XX_TEST_EXPECT_EQ(resultMsgs[idx].role, std::string{"assistant"});
        if (expectDanglingCleared) {
            XX_TEST_EXPECT_TRUE(resultMsgs[idx].tool_calls.empty());
        } else {
            XX_TEST_EXPECT_TRUE(!resultMsgs[idx].tool_calls.empty());
        }
        ++idx;
    }

    // 孤儿 tool 结果: 应被删除 (除非未清理)
    if (expectOrphanRemoved) {
        XX_TEST_EXPECT_TRUE(idx >= resultMsgs.size() || resultMsgs[idx].role != std::string{"tool"});
    } else {
        // 未清理时孤儿结果仍在 (正常路径: 全部 tool_call 都有回复, 不悬挂)
        XX_TEST_EXPECT_TRUE(idx < resultMsgs.size());
        if (idx < resultMsgs.size()) {
            XX_TEST_EXPECT_EQ(resultMsgs[idx].role, std::string{"tool"});
        }
    }

    // 下一条 agent 消息: 应保留 (如果存在)
    if (expectFollowUpKept) {
        bool foundFollowUp = false;
        for (size_t i = idx; i < resultMsgs.size(); ++i) {
            if (resultMsgs[i].role == followUpRole) {
                foundFollowUp = true;
                break;
            }
        }
        XX_TEST_EXPECT_TRUE(foundFollowUp);
    }
}

/// 测试 1: 悬挂 tool_calls (call_b 无结果) + 已回复的 tool(call_a) 成为孤儿
/// [system, user, assistant(tool_calls=[call_a, call_b]), tool(call_a=result)]
/// - call_b 无 tool 结果 → 悬挂 → 清空 tool_calls
/// - tool(call_a=result) 随声明清空成为孤儿 → 删除
/// - 末尾孤儿删除后末尾为 assistant(无 tool_calls)
asio::awaitable<void> test_repair_dangling_orphan_tool_result() {
    runRepairDanglingScenario(2, 1, "", true, true, false);
    co_return;
}

/// 测试 2: 悬挂 + 孤儿 tool 结果 + 下一条 agent 消息 (user)
/// [system, user, assistant(tool_calls=[call_a, call_b]), tool(call_a=result),
///  user("next")]
/// - 悬挂 → 清空 tool_calls, 删除孤儿 tool 结果
/// - 下一条 user 消息保留
asio::awaitable<void> test_repair_dangling_multiple_orphans_keep_next_agent_msg() {
    runRepairDanglingScenario(2, 1, "user", true, true, true);
    co_return;
}

/// 测试 3: 悬挂 + 孤儿 tool 结果 + 下一条 agent 消息 (assistant 无 tool_calls)
/// [system, user, assistant(tool_calls=[call_a, call_b]), tool(call_a=result),
///  assistant("prev answer")]
/// - 悬挂 → 清空 tool_calls, 删除孤儿 tool 结果
/// - 下一条 assistant 消息 (无 tool_calls, 是 agent 消息边界) 保留
asio::awaitable<void> test_repair_dangling_keep_next_assistant_msg() {
    runRepairDanglingScenario(2, 1, "assistant", true, true, true);
    co_return;
}

/// 测试 4 (对照): 全部 tool_call 都有回复 → 不悬挂, 不清理
/// [system, user, assistant(tool_calls=[call_a, call_b]),
///  tool(call_a=result), tool(call_b=result), user("next")]
/// - call_a/call_b 都有 tool 结果 → 不悬挂 → tool_calls 保留, tool 结果保留
/// - 后续 user 消息保留
asio::awaitable<void> test_repair_all_replied_no_cleanup() {
    runRepairDanglingScenario(2, 2, "user", false, false, true);
    co_return;
}

asio::awaitable<TestResult> run_message_supplement_tests() {
    g_ms_passed = 0;
    g_ms_failed = 0;

    try {
        co_await test_interrupt_auto_supplement();
        co_await test_cancel_auto_supplement();
        co_await test_repair_dangling_orphan_tool_result();
        co_await test_repair_dangling_multiple_orphans_keep_next_agent_msg();
        co_await test_repair_dangling_keep_next_assistant_msg();
        co_await test_repair_all_replied_no_cleanup();
    } catch (const std::exception& e) {
        TEST_FAIL << "message_supplement suite exception: " << e.what() << std::endl;
        g_ms_failed++;
    }

    co_return TestResult{g_ms_passed, g_ms_failed};
}

} // namespace test
} // namespace agentxx
