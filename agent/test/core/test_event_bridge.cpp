#include "test_event_bridge.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
#include "agentxx/middlewares/summarization.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/use_awaitable.hpp"
#include <atomic>
#include <iostream>
#include <memory>
#include <string>

namespace agentxx {
namespace test {

int g_eb_passed = 0;
int g_eb_failed = 0;

/// 测试用 IO: 覆写 sendToPeer 记录收到的 delta (等价于 server 端点缓冲行为)
class TestEbIO : public agentxx::agent::AgentIOBase {
public:

    std::vector<agentxx::agent::Delta>            deltas;
    std::vector<agentxx::agent::WireContextStats> stats;

    void sendToPeer(agentxx::agent::WireMessage msg) override {
        if (auto* d = std::get_if<agentxx::agent::Delta>(&msg)) {
            deltas.push_back(*d);
        } else if (auto* s = std::get_if<agentxx::agent::WireContextStats>(&msg)) {
            stats.push_back(*s);
        } else {
            agentxx::agent::AgentIOBase::sendToPeer(std::move(msg));
        }
    }

    void onDelta(const agentxx::agent::Delta& delta) override {
        deltas.push_back(delta);
    }

    void onSync(const agentxx::agent::SyncPayload&) override {}

    void onTurnResult(const agentxx::agent::WireTurnResult&) override {}

    void onContextStats(const agentxx::agent::WireContextStats&) override {}

    asio::awaitable<std::optional<std::string>> getInput() override {
        co_return std::nullopt;
    }

    asio::awaitable<neograph::json>
        handleInterrupt(std::string_view, std::string_view, std::string_view, std::string_view)
            override {
        co_return neograph::json::array();
    }
};

/// 构造一个完整的 EventBridge (含 bus/session/io)
static std::shared_ptr<agentxx::event::EventBridge> makeTestBridge(
    std::shared_ptr<agentxx::agent::AgentContext> agentContext,
    std::shared_ptr<agentxx::agent::Session>      session,
    std::shared_ptr<TestEbIO>                     io,
    neograph::graph::GraphStreamCallback          origCb = nullptr
) {
    return std::make_shared<agentxx::event::EventBridge>(
        "testAgent",
        "thread_42",
        agentContext,
        session,
        io,
        std::move(origCb)
    );
}

/// 验证 EventBridge 把 GraphEvent::LLM_TOKEN 翻译成 EventModelToken 发布到 bus,
/// 同时保留原始 callback 的转发行为
asio::awaitable<void> test_eventbridge_token() {
    auto agentConfig          = std::make_shared<agentxx::agent::AgentConfig>();
    auto agentContext         = std::make_shared<agentxx::agent::AgentContext>();
    agentContext->agentConfig = agentConfig;
    agentContext->bus
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);
    auto session = std::make_shared<agentxx::agent::Session>();
    auto io      = std::make_shared<TestEbIO>();

    // 订阅 ModelToken 事件
    std::atomic<int> tokenEventCount{0};
    std::string      lastToken;
    std::string      lastAgentName;
    std::string      lastThreadId;
    agentContext->bus->get<agentxx::events::EventModelToken>(agentxx::events::Topic::ModelToken)
        .subscribe([&](const agentxx::events::EventModelToken& e) -> asio::awaitable<void> {
            tokenEventCount++;
            lastToken     = e.token;
            lastAgentName = e.agentName;
            lastThreadId  = e.sessionId;
            co_return;
        });

    // 原始 callback 计数 (验证 passthrough)
    std::atomic<int>                     origCbCount{0};
    neograph::graph::GraphStreamCallback origCb = [&](const neograph::graph::GraphEvent& event) {
        if (event.type == neograph::graph::GraphEvent::Type::LLM_TOKEN) {
            origCbCount++;
        }
    };

    auto bridge   = makeTestBridge(agentContext, session, io, origCb);
    auto bridgeCb = bridge->makeCallback();

    // 触发 LLM_TOKEN 事件
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::LLM_TOKEN,
        "llm",
        neograph::json(std::string{"Hello"})
    });

    // fire-and-forget co_spawn 异步发布; 让出一次让协程跑完
    auto timer
        = asio::steady_timer(co_await asio::this_coro::executor, std::chrono::milliseconds(10));
    co_await timer.async_wait(asio::use_awaitable);

    XX_TEST_EXPECT_EQ(origCbCount.load(), 1);
    XX_TEST_EXPECT_EQ(tokenEventCount.load(), 1);
    XX_TEST_EXPECT_EQ(lastToken, std::string{"Hello"});
    XX_TEST_EXPECT_EQ(lastAgentName, std::string{"testAgent"});
    XX_TEST_EXPECT_EQ(lastThreadId, std::string{"thread_42"});

    // 再触发一次, 验证多次
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::LLM_TOKEN,
        "llm",
        neograph::json(std::string{" World"})
    });
    co_await asio::steady_timer(co_await asio::this_coro::executor, std::chrono::milliseconds(10))
        .async_wait(asio::use_awaitable);
    XX_TEST_EXPECT_EQ(origCbCount.load(), 2);
    XX_TEST_EXPECT_EQ(tokenEventCount.load(), 2);
    XX_TEST_EXPECT_EQ(lastToken, std::string{" World"});

    // 同时应产出 TextToken delta, 且 seq 会话级单调递增
    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{2});
    if (io->deltas.size() == 2) {
        XX_TEST_EXPECT_TRUE(io->deltas[0].type == agentxx::agent::Delta::Type::TextToken);
        XX_TEST_EXPECT_EQ(io->deltas[0].seq, uint64_t{1});
        XX_TEST_EXPECT_EQ(io->deltas[0].text, std::string{"Hello"});
        XX_TEST_EXPECT_TRUE(io->deltas[1].type == agentxx::agent::Delta::Type::TextToken);
        XX_TEST_EXPECT_EQ(io->deltas[1].seq, uint64_t{2});
        XX_TEST_EXPECT_EQ(io->deltas[1].text, std::string{" World"});
    }

    co_return;
}

/// 验证 bus 为空时, EventBridge 仍转发原始 callback 并产出 Delta
asio::awaitable<void> test_eventbridge_nullbus_passthrough() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    // 不设置 bus (bus == nullptr)
    auto session = std::make_shared<agentxx::agent::Session>();
    auto io      = std::make_shared<TestEbIO>();

    std::atomic<int>                     origCbCount{0};
    neograph::graph::GraphStreamCallback origCb = [&](const neograph::graph::GraphEvent& event) {
        if (event.type == neograph::graph::GraphEvent::Type::LLM_TOKEN) {
            origCbCount++;
        }
    };

    auto bridge   = makeTestBridge(agentContext, session, io, origCb);
    auto bridgeCb = bridge->makeCallback();
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::LLM_TOKEN,
        "llm",
        neograph::json(std::string{"x"})
    });
    XX_TEST_EXPECT_EQ(origCbCount.load(), 1);
    // 无 bus 时 Delta 翻译仍正常
    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{1});

    co_return;
}

/// 验证 ERROR 事件发布
asio::awaitable<void> test_eventbridge_error() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    agentContext->bus
        = std::make_shared<agentxx::event::EventBus>(co_await asio::this_coro::executor);
    auto session = std::make_shared<agentxx::agent::Session>();
    auto io      = std::make_shared<TestEbIO>();

    std::atomic<int> errCount{0};
    std::string      lastMsg;
    std::string      lastWhere;
    agentContext->bus->get<agentxx::events::EventError>(agentxx::events::Topic::Error)
        .subscribe([&](const agentxx::events::EventError& e) -> asio::awaitable<void> {
            errCount++;
            lastMsg   = e.message;
            lastWhere = e.where;
            co_return;
        });

    auto bridge   = makeTestBridge(agentContext, session, io);
    auto bridgeCb = bridge->makeCallback();
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::ERROR,
        "tool_x",
        neograph::json(std::string{"boom"})
    });

    co_await asio::steady_timer(co_await asio::this_coro::executor, std::chrono::milliseconds(10))
        .async_wait(asio::use_awaitable);
    XX_TEST_EXPECT_EQ(errCount.load(), 1);
    XX_TEST_EXPECT_EQ(lastMsg, std::string{"boom"});
    XX_TEST_EXPECT_EQ(lastWhere, std::string{"tool_x"});
    // ERROR 不产出 Delta
    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{0});

    co_return;
}

/// 验证 MessageUITip: CHANNEL_WRITE "message_tip" -> Delta::MessageUITip (warning/error/info)
asio::awaitable<void> test_eventbridge_message_tip() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    auto session      = std::make_shared<agentxx::agent::Session>();
    auto io           = std::make_shared<TestEbIO>();

    auto bridge   = makeTestBridge(agentContext, session, io);
    auto bridgeCb = bridge->makeCallback();

    // warning 级别
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::CHANNEL_WRITE,
        "llm",
        neograph::json{
                       {"channel", "message_tip"},
                       {"value",
             neograph::json{
                 {"tipType", "warning"},
                 {"text", "LLM API 请求失败，6 秒后自动重试 (2/5)"},
             }},
                       }
    });
    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{1});
    if (!io->deltas.empty()) {
        XX_TEST_EXPECT_TRUE(io->deltas[0].type == agentxx::agent::Delta::Type::MessageUITip);
        XX_TEST_EXPECT_TRUE(io->deltas[0].tipType == agentxx::agent::Delta::TipType::Warning);
        XX_TEST_EXPECT_EQ(
            io->deltas[0].text,
            std::string{"LLM API 请求失败，6 秒后自动重试 (2/5)"}
        );
    }

    // error 级别
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::CHANNEL_WRITE,
        "llm",
        neograph::json{
                       {"channel", "message_tip"},
                       {"value",
             neograph::json{
                 {"tipType", "error"},
                 {"text", "boom"},
             }},
                       }
    });
    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{2});
    if (io->deltas.size() >= 2) {
        XX_TEST_EXPECT_TRUE(io->deltas[1].tipType == agentxx::agent::Delta::TipType::Error);
        XX_TEST_EXPECT_EQ(io->deltas[1].text, std::string{"boom"});
    }

    // 默认 info 级别
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::CHANNEL_WRITE,
        "llm",
        neograph::json{
                       {"channel", "message_tip"},
                       {"value", neograph::json{{"text", "hello"}}},
                       }
    });
    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{3});
    if (io->deltas.size() >= 3) {
        XX_TEST_EXPECT_TRUE(io->deltas[2].tipType == agentxx::agent::Delta::TipType::Info);
    }

    co_return;
}

/// 验证 CHANNEL_WRITE messages: assistant(tool_calls) -> ToolStart + 历史追加
asio::awaitable<void> test_eventbridge_channel_write_messages() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    auto session      = std::make_shared<agentxx::agent::Session>();
    auto io           = std::make_shared<TestEbIO>();

    auto bridge   = makeTestBridge(agentContext, session, io);
    auto bridgeCb = bridge->makeCallback();

    auto msgJson = neograph::json{
        {"role",       "assistant"},
        {"content",    ""         },
        {"tool_calls",
         neograph::json::array({
             neograph::json{
                 {"id", "call_1"},
                 {"name", "bash"},
                 {"arguments", "{\"cmd\":\"ls\"}"},
             },
         })                       },
    };
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::CHANNEL_WRITE,
        "llm",
        neograph::json{
                       {"channel", "messages"},
                       {"value", neograph::json::array({msgJson})},
                       }
    });

    // 产出 ToolStart delta
    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{1});
    if (!io->deltas.empty()) {
        XX_TEST_EXPECT_TRUE(io->deltas[0].type == agentxx::agent::Delta::Type::ToolStart);
        XX_TEST_EXPECT_EQ(io->deltas[0].toolName, std::string{"bash"});
        XX_TEST_EXPECT_EQ(io->deltas[0].toolCallId, std::string{"call_1"});
    }
    // 历史已追加
    XX_TEST_EXPECT_EQ(session->viewMessages.size(), size_t{1});

    co_return;
}

/// 回归: assistant 消息携带 reasoning_content 时必须展开为 Think 历史消息,
/// 否则 viewMessages 不保存 Think, 重启恢复会话后 Think 丢失
/// - 含 tool_calls 的消息 (thinking 模型 + 工具调用的常见形态) 同样要展开:
///   修复前该分支只展开 Tool 消息, Think 从未进入 viewMessages/持久化
/// - 纯文本 assistant (无 tool_calls) 也应展开: Think 在前, Assistant 在后
asio::awaitable<void> test_eventbridge_channel_write_thinking() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    auto session      = std::make_shared<agentxx::agent::Session>();
    auto io           = std::make_shared<TestEbIO>();

    auto bridge   = makeTestBridge(agentContext, session, io);
    auto bridgeCb = bridge->makeCallback();

    // ---- 1. assistant 含 reasoning_content + tool_calls ----
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::CHANNEL_WRITE,
        "llm",
        neograph::json{
                       {"channel", "messages"},
                       {"value",
             neograph::json::array({
                 neograph::json{
                     {"role", "assistant"},
                     {"content", ""},
                     {"reasoning_content", "need to list files first"},
                     {"tool_calls",
                      neograph::json::array({
                          neograph::json{
                              {"id", "call_1"},
                              {"name", "bash"},
                              {"arguments", "{\"cmd\":\"ls\"}"},
                          },
                      })},
                 },
             })},
                       }
    });
    // Think 在前, Tool 在后
    XX_TEST_EXPECT_EQ(session->viewMessages.size(), size_t{2});
    if (session->viewMessages.size() == 2) {
        const auto& thinking = session->viewMessages[0];
        XX_TEST_EXPECT_TRUE(thinking.role == agentxx::agent::ViewMessage::Role::Think);
        XX_TEST_EXPECT_EQ(thinking.text, std::string{"need to list files first"});
        XX_TEST_EXPECT_TRUE(thinking.collapsed);
        XX_TEST_EXPECT_FALSE(thinking.id.empty()); // 已分配 id (可持久化定位)

        const auto& tool = session->viewMessages[1];
        XX_TEST_EXPECT_TRUE(tool.role == agentxx::agent::ViewMessage::Role::Tool);
        if (tool.tool) {
            XX_TEST_EXPECT_EQ(tool.tool->toolCallId, std::string{"call_1"});
        }
    }

    // ---- 2. 纯文本 assistant 含 reasoning_content (无 tool_calls) ----
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::CHANNEL_WRITE,
        "llm",
        neograph::json{
                       {"channel", "messages"},
                       {"value",
             neograph::json::array({
                 neograph::json{
                     {"role", "assistant"},
                     {"content", "done"},
                     {"reasoning_content", "reasoned here"},
                 },
             })},
                       }
    });
    XX_TEST_EXPECT_EQ(session->viewMessages.size(), size_t{4});
    if (session->viewMessages.size() == 4) {
        const auto& thinking2 = session->viewMessages[2];
        XX_TEST_EXPECT_TRUE(thinking2.role == agentxx::agent::ViewMessage::Role::Think);
        XX_TEST_EXPECT_EQ(thinking2.text, std::string{"reasoned here"});
        XX_TEST_EXPECT_TRUE(thinking2.collapsed);
        const auto& assistant = session->viewMessages[3];
        XX_TEST_EXPECT_TRUE(assistant.role == agentxx::agent::ViewMessage::Role::Assistant);
        XX_TEST_EXPECT_EQ(assistant.text, std::string{"done"});
    }

    // ---- 3. 无 reasoning_content: 不产生 Think (空内容不应生成空消息) ----
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::CHANNEL_WRITE,
        "llm",
        neograph::json{
                       {"channel", "messages"},
                       {"value",
             neograph::json::array({
                 neograph::json{
                     {"role", "assistant"},
                     {"content", "plain"},
                 },
             })},
                       }
    });
    XX_TEST_EXPECT_EQ(session->viewMessages.size(), size_t{5});
    if (session->viewMessages.size() == 5) {
        XX_TEST_EXPECT_TRUE(
            session->viewMessages[4].role == agentxx::agent::ViewMessage::Role::Assistant
        );
    }

    // ---- 4. 加密 thinking (reasoning_content 为空, extra 含 responses_reasoning_items 与
    // reasoning_tokens) ----
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::CHANNEL_WRITE,
        "llm",
        neograph::json{
                       {"channel", "messages"},
                       {"value",
             neograph::json::array({
                 neograph::json{
                     {"role", "assistant"},
                     {"content", "answer"},
                     {"reasoning_content", ""},
                     {"extra",
                      neograph::json{
                          {"reasoning_tokens", 854},
                          {"responses_reasoning_items",
                           neograph::json::array({
                               neograph::
                                   json{{"type", "reasoning"}, {"encrypted_content", "enc_data"}},
                           })},
                      }},
                 },
             })},
                       }
    });
    XX_TEST_EXPECT_EQ(session->viewMessages.size(), size_t{7});
    if (session->viewMessages.size() == 7) {
        const auto& encThink = session->viewMessages[5];
        XX_TEST_EXPECT_TRUE(encThink.role == agentxx::agent::ViewMessage::Role::Think);
        XX_TEST_EXPECT_TRUE(encThink.text.empty()); // 保持空内容
        XX_TEST_EXPECT_TRUE(encThink.think.has_value());
        if (encThink.think) {
            XX_TEST_EXPECT_EQ(encThink.think->reasoningTokens, 854);
            XX_TEST_EXPECT_TRUE(encThink.think->isEncrypted);
        }
        const auto& assistant = session->viewMessages[6];
        XX_TEST_EXPECT_TRUE(assistant.role == agentxx::agent::ViewMessage::Role::Assistant);
        XX_TEST_EXPECT_EQ(assistant.text, std::string{"answer"});
    }

    co_return;
}

/// 验证 NODE_START/NODE_END -> NodeStart/NodeEnd delta
asio::awaitable<void> test_eventbridge_node_delta() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    auto session      = std::make_shared<agentxx::agent::Session>();
    auto io           = std::make_shared<TestEbIO>();

    auto bridge   = makeTestBridge(agentContext, session, io);
    auto bridgeCb = bridge->makeCallback();

    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::NODE_START,
        "llm",
        neograph::json::object()
    });
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::NODE_END,
        "llm",
        neograph::json::object()
    });

    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{2});
    if (io->deltas.size() == 2) {
        XX_TEST_EXPECT_TRUE(io->deltas[0].type == agentxx::agent::Delta::Type::NodeStart);
        XX_TEST_EXPECT_EQ(io->deltas[0].nodeName, std::string{"llm"});
        XX_TEST_EXPECT_TRUE(io->deltas[1].type == agentxx::agent::Delta::Type::NodeEnd);
        XX_TEST_EXPECT_EQ(io->deltas[1].nodeName, std::string{"llm"});
    }

    co_return;
}

/// 验证 ModelCall 流式期间 tps 统计:
/// - 从首个 token (节点开始后) 计时, 每 [tpsPushIntervalSec_] 秒推送一次平均速度
/// - 间隔未到不推送; 新流 (NODE_START 后) 重新计时
/// - token 估算复用 AgentContext::summarizationMiddleware 的 countTokensForUtf8Str 口径
asio::awaitable<void> test_eventbridge_tps() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    // 注入 summarization 中间件: countTokens 应复用其 token 计算口径
    agentContext->summarizationMiddleware
        = std::make_shared<agentxx::middleware::SummarizationMiddlewareHandle>(agentContext);
    auto session = std::make_shared<agentxx::agent::Session>();
    auto io      = std::make_shared<TestEbIO>();

    auto bridge = makeTestBridge(agentContext, session, io);
    // 缩短推送间隔 (默认 5 秒), 测试无需真实等待
    bridge->setTpsPushInterval(0.05);
    auto bridgeCb = bridge->makeCallback();

    // 节点开始: 重置 chunk 类型标记, 后续首个 token 作为新流计时起点
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::NODE_START,
        "llm",
        neograph::json::object()
    });

    // 首个 token: 流开始计时; 间隔未到 → 不推送
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::LLM_TOKEN,
        "llm",
        neograph::json(std::string{"Hello"})
    });
    XX_TEST_EXPECT_EQ(io->stats.size(), size_t{0});

    // 等待超过间隔后继续发 token → 触发推送, tps > 0
    co_await asio::steady_timer(co_await asio::this_coro::executor, std::chrono::milliseconds(80))
        .async_wait(asio::use_awaitable);
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::LLM_TOKEN,
        "llm",
        neograph::json(std::string{" World"})
    });
    XX_TEST_EXPECT_EQ(io->stats.size(), size_t{1});
    if (io->stats.size() == 1) {
        // "Hello" ≈ 5 个 ascii 字符 / 4 ≈ 1.25 token, 80ms 内 → tps 约 15.6
        XX_TEST_EXPECT_TRUE(io->stats[0].tps > 0.0);
        // 上下文统计字段透传 (默认 0)
        XX_TEST_EXPECT_EQ(io->stats[0].contextTokens, uint64_t{0});
    }

    // 新流 (NODE_START) 重新计时: 间隔从新流首个 token 起算, 立即发 token 不推送
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::NODE_START,
        "llm",
        neograph::json::object()
    });
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::LLM_TOKEN,
        "llm",
        neograph::json(std::string{"new stream"})
    });
    // 上一次推送时间戳已按新流重置: nowSec 重新从 0 起算, 未到 50ms 不推送
    XX_TEST_EXPECT_EQ(io->stats.size(), size_t{1});

    co_return;
}

/// 验证轮级 tps 统计 (TurnStart/TurnEnd 使用):
/// - 未开始轮次时 takeTurnTps 返回 0
/// - handleTurnStart 重置轮级统计
/// - 一轮内多个 ModelCall 流 (NODE_START → token → NODE_END) 的累计 token /
///   累计流式耗时在 takeTurnTps 时结算为平均速度; 空流 (无 token) 不计入
/// - takeTurnTps 取走后重置, 再次调用返回 0
/// - 新轮次无 LLM 输出直接结束 → 0
asio::awaitable<void> test_eventbridge_turn_tps() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    agentContext->summarizationMiddleware
        = std::make_shared<agentxx::middleware::SummarizationMiddlewareHandle>(agentContext);
    auto session = std::make_shared<agentxx::agent::Session>();
    auto io      = std::make_shared<TestEbIO>();

    auto bridge   = makeTestBridge(agentContext, session, io);
    auto bridgeCb = bridge->makeCallback();
    auto executor = co_await asio::this_coro::executor;

    // 初始状态: 未开始轮次时取平均应为 0
    XX_TEST_EXPECT_EQ(bridge->takeTurnTps(), 0.0);

    // 轮次开始: 重置统计
    bridge->handleTurnStart();

    // 第一个 ModelCall 流: 开始 → 发 token → 等待一小段 → 发 token → 结束 (结算)
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::NODE_START,
        "llm",
        neograph::json::object()
    });
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::LLM_TOKEN,
        "llm",
        neograph::json(std::string{"Hello"})
    });
    co_await asio::steady_timer(executor, std::chrono::milliseconds(50))
        .async_wait(asio::use_awaitable);
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::LLM_TOKEN,
        "llm",
        neograph::json(std::string{" World"})
    });
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::NODE_END,
        "llm",
        neograph::json::object()
    });

    // 第二个 ModelCall 流: 空流 (无 token) → 不结算, 不影响轮级统计
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::NODE_START,
        "llm",
        neograph::json::object()
    });
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::NODE_END,
        "llm",
        neograph::json::object()
    });

    // 轮次结束: 取走平均速度
    // "Hello World" ≈ 11 个 ascii 字符 / 4 ≈ 2.75 token, 流式耗时约 50ms → tps > 0
    const double tps = bridge->takeTurnTps();
    XX_TEST_EXPECT_TRUE(tps > 0.0);

    // 取走后已重置: 再次调用返回 0
    XX_TEST_EXPECT_EQ(bridge->takeTurnTps(), 0.0);

    // 新轮次: 无 LLM 输出直接结束 → 0
    bridge->handleTurnStart();
    XX_TEST_EXPECT_EQ(bridge->takeTurnTps(), 0.0);

    co_return;
}

asio::awaitable<TestResult> run_event_bridge_tests() {
    g_eb_passed = 0;
    g_eb_failed = 0;
    try {
        co_await test_eventbridge_token();
        co_await test_eventbridge_nullbus_passthrough();
        co_await test_eventbridge_error();
        co_await test_eventbridge_message_tip();
        co_await test_eventbridge_channel_write_messages();
        co_await test_eventbridge_channel_write_thinking();
        co_await test_eventbridge_node_delta();
        co_await test_eventbridge_tps();
        co_await test_eventbridge_turn_tps();
    } catch (const std::exception& e) {
        TEST_FAIL << "event_bridge suite exception: " << e.what() << std::endl;
        g_eb_failed++;
    }
    co_return TestResult{g_eb_passed, g_eb_failed};
}

} // namespace test
} // namespace agentxx
