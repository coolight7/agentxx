#include "agentxx-test/core/test_event_bridge.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/middlewares/summarization.h"
#include "agentxx/protocol/provider_common.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/use_awaitable.hpp"
#include "utilxx_base/json.h"
#include <atomic>
#include <iostream>
#include <memory>
#include <string>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_eb_passed = 0;
int g_eb_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_eb_passed
#define XX_TEST_FAILED g_eb_failed

namespace agentxx {
namespace test {

namespace {
/// 类型简称 (本文件大量断言增量类型与消息角色)
using DT = agentxx::agent::WireDelta::Type;
using VM = agentxx::agent::ViewMessage;
} // namespace

/// 测试用 IO: 覆写 sendToPeer 记录收到的 delta (等价于 server 端点缓冲行为)
class TestEbIO : public agentxx::agent::AgentIOBase {
public:

    std::vector<agentxx::agent::WireDelta>        deltas;
    std::vector<agentxx::agent::WireContextStats> stats;

    void sendToPeer(agentxx::agent::WireMessage msg) override {
        if (auto* d = std::get_if<agentxx::agent::WireDelta>(&msg)) {
            deltas.push_back(*d);
        } else if (auto* s = std::get_if<agentxx::agent::WireContextStats>(&msg)) {
            stats.push_back(*s);
        } else {
            agentxx::agent::AgentIOBase::sendToPeer(std::move(msg));
        }
    }

    void onDelta(const agentxx::agent::WireDelta& delta) override {
        deltas.push_back(delta);
    }

    void onSync(const agentxx::agent::WireSyncPayload&) override {}

    void onTurnResult(const agentxx::agent::WireTurnResult&) override {}

    void onContextStats(const agentxx::agent::WireContextStats&) override {}

    asio::awaitable<std::optional<std::string>> getInput() override {
        co_return std::nullopt;
    }

    asio::awaitable<utilxx_base::Json>
        handleInterrupt(std::string_view, std::string_view, std::string_view, std::string_view)
            override {
        co_return agentxx::middleware::makeInterruptResult(utilxx_base::Json::object());
    }
};

/// 构造一个完整的 EventBridge (含 bus/session/io)
static std::shared_ptr<agentxx::events::EventBridge> makeTestBridge(
    std::shared_ptr<agentxx::agent::AgentContext> agentContext,
    std::shared_ptr<agentxx::agent::Session>      session,
    std::shared_ptr<TestEbIO>                     io,
    neograph::graph::GraphStreamCallback          origCb = nullptr
) {
    return std::make_shared<agentxx::events::EventBridge>(
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
        = std::make_shared<agentxx::events::EventBus>(co_await asio::this_coro::executor);
    auto session = std::make_shared<agentxx::agent::Session>();
    auto io      = std::make_shared<TestEbIO>();
    session->io  = io; // 有对端才建直播槽并产出流式增量

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

    // 同时应产出 stream_begin + text_token 增量, 且 seq 会话级单调递增
    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{3});
    if (io->deltas.size() == 3) {
        XX_TEST_EXPECT_TRUE(io->deltas[0].type == DT::StreamBegin);
        XX_TEST_EXPECT_TRUE(io->deltas[0].streamRole == VM::Role::Assistant);
        XX_TEST_EXPECT_FALSE(io->deltas[0].msgId.empty());
        XX_TEST_EXPECT_EQ(io->deltas[0].seq, uint64_t{1});
        XX_TEST_EXPECT_TRUE(io->deltas[1].type == DT::TextToken);
        XX_TEST_EXPECT_EQ(io->deltas[1].msgId, io->deltas[0].msgId);
        XX_TEST_EXPECT_EQ(io->deltas[1].seq, uint64_t{2});
        XX_TEST_EXPECT_EQ(io->deltas[1].text, std::string{"Hello"});
        XX_TEST_EXPECT_TRUE(io->deltas[2].type == DT::TextToken);
        XX_TEST_EXPECT_EQ(io->deltas[2].seq, uint64_t{3});
        XX_TEST_EXPECT_EQ(io->deltas[2].text, std::string{" World"});
    }

    co_return;
}

/// 验证 bus 为空时, EventBridge 仍转发原始 callback 并产出 WireDelta
asio::awaitable<void> test_eventbridge_nullbus_passthrough() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    // 不设置 bus (bus == nullptr)
    auto session = std::make_shared<agentxx::agent::Session>();
    auto io      = std::make_shared<TestEbIO>();
    session->io  = io;

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
    // 无 bus 时 WireDelta 翻译仍正常 (stream_begin + text_token)
    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{2});

    co_return;
}

/// 验证 ERROR 事件发布
asio::awaitable<void> test_eventbridge_error() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    agentContext->bus
        = std::make_shared<agentxx::events::EventBus>(co_await asio::this_coro::executor);
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
    // ERROR 不产出 WireDelta
    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{0});

    co_return;
}

/// 验证 MessageUITip: CHANNEL_WRITE "message_tip" -> WireDelta::MessageUITip (warning/error/info)
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
        XX_TEST_EXPECT_TRUE(io->deltas[0].type == agentxx::agent::WireDelta::Type::MessageUITip);
        XX_TEST_EXPECT_TRUE(io->deltas[0].tipType == agentxx::agent::WireDelta::TipType::Warning);
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
        XX_TEST_EXPECT_TRUE(io->deltas[1].tipType == agentxx::agent::WireDelta::TipType::Error);
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
        XX_TEST_EXPECT_TRUE(io->deltas[2].tipType == agentxx::agent::WireDelta::TipType::Info);
    }

    co_return;
}

/// 验证 CHANNEL_WRITE messages: assistant(tool_calls) -> 工具行 insert_message + 历史追加
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

    // 工具行就是普通历史行: 整条 insert_message (未完成状态)
    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{1});
    if (!io->deltas.empty()) {
        const auto& d = io->deltas[0];
        XX_TEST_EXPECT_TRUE(d.type == DT::InsertMessage);
        if (d.message) {
            XX_TEST_EXPECT_TRUE(d.message->role == VM::Role::Tool);
            XX_TEST_EXPECT_EQ(d.message->text, std::string{"{\"cmd\":\"ls\"}"});
            XX_TEST_EXPECT_TRUE(d.message->tool.has_value());
            if (d.message->tool) {
                XX_TEST_EXPECT_EQ(d.message->tool->toolName, std::string{"bash"});
                XX_TEST_EXPECT_EQ(d.message->tool->toolCallId, std::string{"call_1"});
                XX_TEST_EXPECT_FALSE(d.message->tool->toolFinished);
            }
        }
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

    // ---- 5. 思考内容只有空白/零宽占位符: 不生成空的 Think 消息 ----
    // 这种消息渲染出来只剩一个 `[思考]` 头部行, 看上去就是一条空消息,
    // 而且它不追加尾部空行, 会和后面的消息贴在一起 (实况与会话重拉历史两种情况
    // 原先不一致, 现在写入侧就不生成)
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::CHANNEL_WRITE,
        "llm",
        neograph::json{
                       {"channel", "messages"},
                       {"value",
             neograph::json::array({
                 neograph::json{
                     {"role", "assistant"},
                     {"content", "after blank think"},
                     {"reasoning_content", "\xE2\x80\x8B \n"},
                 },
             })},
                       }
    });
    XX_TEST_EXPECT_EQ(session->viewMessages.size(), size_t{8});
    if (session->viewMessages.size() == 8) {
        XX_TEST_EXPECT_TRUE(
            session->viewMessages[7].role == agentxx::agent::ViewMessage::Role::Assistant
        );
        XX_TEST_EXPECT_EQ(session->viewMessages[7].text, std::string{"after blank think"});
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
        XX_TEST_EXPECT_TRUE(io->deltas[0].type == agentxx::agent::WireDelta::Type::NodeStart);
        XX_TEST_EXPECT_EQ(io->deltas[0].nodeName, std::string{"llm"});
        XX_TEST_EXPECT_TRUE(io->deltas[1].type == agentxx::agent::WireDelta::Type::NodeEnd);
        XX_TEST_EXPECT_EQ(io->deltas[1].nodeName, std::string{"llm"});
    }

    co_return;
}

/// 验证 ModelCall 流式期间 tps 统计:
/// - 从首个 token (节点开始后) 计时, 每 [tpsPushIntervalSec_] 秒推送一次平均速度
/// - 默认间隔 1 秒 (流式 token 到达 1 秒后界面即可显示 tps)
/// - 间隔未到不推送; 新流 (NODE_START 后) 重新计时
/// - token 估算复用 EventBus 上注册的 TokenCount 服务
asio::awaitable<void> test_eventbridge_tps() {
    auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
    agentContext->bus
        = std::make_shared<agentxx::events::EventBus>(co_await asio::this_coro::executor);
    // 注入 summarization 中间件: countTokens 应复用它的 token 计算方式
    auto summarizationMiddleware
        = std::make_shared<agentxx::middleware::SummarizationMiddlewareHandle>(agentContext);
    summarizationMiddleware->registerOnBus(agentContext->bus);
    auto session = std::make_shared<agentxx::agent::Session>();
    auto io      = std::make_shared<TestEbIO>();

    // 会话累计用量 (界面显示已消耗/输入/输出/缓存 token): 应随每次统计推送一并下发
    session->contextStats->usage.promptTokens       = 800;
    session->contextStats->usage.completionTokens   = 200;
    session->contextStats->usage.totalTokens        = 1000;
    session->contextStats->usage.cachedPromptTokens = 600;

    auto bridge = makeTestBridge(agentContext, session, io);
    // 默认推送间隔 = 1 秒 (界面在流式开始 1 秒后显示速度), 测试中不改动默认值
    XX_TEST_EXPECT_EQ(bridge->tpsPushInterval(), 1.0);
    auto bridgeCb = bridge->makeCallback();
    auto executor = co_await asio::this_coro::executor;

    // 节点开始: 重置 chunk 类型标记, 后续首个 token 作为新流计时起点
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::NODE_START,
        "llm",
        neograph::json::object()
    });

    // 首个 token: 流开始计时; 默认 1 秒间隔未到 → 不推送 (界面此时还没有速度可显示)
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::LLM_TOKEN,
        "llm",
        neograph::json(std::string{"Hello"})
    });
    XX_TEST_EXPECT_EQ(io->stats.size(), size_t{0});

    // 等待超过默认间隔 (1 秒) 后继续发 token → 触发推送, 即"流式 token 到达 1 秒后
    // 界面就能看到生成速度"
    co_await asio::steady_timer(executor, std::chrono::milliseconds(1050))
        .async_wait(asio::use_awaitable);
    bridgeCb(neograph::graph::GraphEvent{
        neograph::graph::GraphEvent::Type::LLM_TOKEN,
        "llm",
        neograph::json(std::string{" World"})
    });
    XX_TEST_EXPECT_EQ(io->stats.size(), size_t{1});
    if (io->stats.size() == 1) {
        // "Hello World" ≈ 11 个 ascii 字符 / 4 ≈ 2.75 token, 窗口约 1.05 秒 → tps 约 2.6
        XX_TEST_EXPECT_TRUE(io->stats[0].tps > 0.0);
        // 上下文统计字段原样转发 (默认 0)
        XX_TEST_EXPECT_EQ(io->stats[0].contextTokens, uint64_t{0});
        // 会话累计用量随同一条消息下发 (界面显示已消耗/输入/输出/缓存 token)
        XX_TEST_EXPECT_EQ(io->stats[0].promptTokens, uint64_t{800});
        XX_TEST_EXPECT_EQ(io->stats[0].completionTokens, uint64_t{200});
        XX_TEST_EXPECT_EQ(io->stats[0].totalTokens, uint64_t{1000});
        XX_TEST_EXPECT_EQ(io->stats[0].cachedPromptTokens, uint64_t{600});
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
    // 上一次推送时间戳已按新流重置: nowSec 重新从 0 起算, 未到 1 秒不推送
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
    agentContext->bus
        = std::make_shared<agentxx::events::EventBus>(co_await asio::this_coro::executor);
    auto summarizationMiddleware
        = std::make_shared<agentxx::middleware::SummarizationMiddlewareHandle>(agentContext);
    summarizationMiddleware->registerOnBus(agentContext->bus);
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

/// 验证思考/正文分段与定稿 (直播槽 -> 正式行):
/// - 一段一行: 思考 -> 正文是一次段切换, 上一段先定稿成行 (insert_message) 再开新段
/// - 段内 token 只带文本; 耗时在定稿时写进行 (durationMs), 协议不再有"结算包"增量
/// - 思考后直接 tool_calls (无正文): 思考段在 CHANNEL_WRITE 时定稿
/// - 纯思考流 (无正文无工具): 思考段在 NodeEnd 前定稿
/// - 纯正文流: 只有正文行
/// - CHANNEL_WRITE 的定稿文本与已有段一致时不额外发整条覆盖 (幂等)
asio::awaitable<void> test_eventbridge_think_duration() {
    auto makeChunk = [](int type, std::string data) {
        return neograph::json{
            {"type", type           },
            {"data", std::move(data)}
        };
    };
    using ET = neograph::graph::GraphEvent::Type;
    using DT = agentxx::agent::WireDelta::Type;
    using VM = agentxx::agent::ViewMessage;

    auto sleepMs = [](int ms) -> asio::awaitable<void> {
        co_await asio::steady_timer(
            co_await asio::this_coro::executor,
            std::chrono::milliseconds(ms)
        )
            .async_wait(asio::use_awaitable);
    };

    // ---- 流程 1: think -> 正文: 切段时思考行定稿, 节点结束时正文行定稿 ----
    {
        auto agentContext         = std::make_shared<agentxx::agent::AgentContext>();
        agentContext->agentConfig = std::make_shared<agentxx::agent::AgentConfig>();
        auto session              = std::make_shared<agentxx::agent::Session>();
        auto io                   = std::make_shared<TestEbIO>();
        session->io               = io;
        auto bridge               = makeTestBridge(agentContext, session, io);
        auto bridgeCb             = bridge->makeCallback();

        bridgeCb(neograph::graph::GraphEvent{ET::NODE_START, "llm", neograph::json::object()});
        bridgeCb(neograph::graph::GraphEvent{
            ET::LLM_TOKEN,
            "llm",
            makeChunk(neograph::ChatStreamChunk::TYPE_THINKING, "a")
        });
        co_await sleepMs(60);
        bridgeCb(neograph::graph::GraphEvent{
            ET::LLM_TOKEN,
            "llm",
            makeChunk(neograph::ChatStreamChunk::TYPE_THINKING, "b")
        });
        bridgeCb(neograph::graph::GraphEvent{
            ET::LLM_TOKEN,
            "llm",
            makeChunk(neograph::ChatStreamChunk::TYPE_CONTENT, "x")
        });

        // NodeStart, StreamBegin(Think), Think(a), Think(b),
        // InsertMessage(思考行), StreamBegin(Assistant), Text(x)
        XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{7});
        if (io->deltas.size() == 7) {
            XX_TEST_EXPECT_TRUE(io->deltas[1].type == DT::StreamBegin);
            XX_TEST_EXPECT_TRUE(io->deltas[1].streamRole == VM::Role::Think);
            const auto thinkId = io->deltas[1].msgId;
            XX_TEST_EXPECT_FALSE(thinkId.empty());
            XX_TEST_EXPECT_TRUE(io->deltas[2].type == DT::ThinkToken);
            XX_TEST_EXPECT_EQ(io->deltas[2].msgId, thinkId);
            XX_TEST_EXPECT_EQ(io->deltas[2].text, std::string{"a"});
            // 流式 token 不带耗时 (耗时在定稿行上)
            XX_TEST_EXPECT_EQ(io->deltas[2].durationMs, int64_t{0});
            XX_TEST_EXPECT_EQ(io->deltas[3].durationMs, int64_t{0});
            // 定稿行: 全文 + 段耗时 (>= 50ms) + 沿用流式段的 id
            XX_TEST_EXPECT_TRUE(io->deltas[4].type == DT::InsertMessage);
            if (io->deltas[4].message) {
                XX_TEST_EXPECT_TRUE(io->deltas[4].message->role == VM::Role::Think);
                XX_TEST_EXPECT_EQ(io->deltas[4].message->id, thinkId);
                XX_TEST_EXPECT_EQ(io->deltas[4].message->text, std::string{"ab"});
                XX_TEST_EXPECT_TRUE(io->deltas[4].message->collapsed);
                XX_TEST_EXPECT_TRUE(io->deltas[4].message->startTimeMs > 0);
                XX_TEST_EXPECT_TRUE(
                    io->deltas[4].message->durationMs >= 50
                    && io->deltas[4].message->durationMs < 10000
                );
            }
            XX_TEST_EXPECT_TRUE(io->deltas[5].type == DT::StreamBegin);
            XX_TEST_EXPECT_TRUE(io->deltas[5].streamRole == VM::Role::Assistant);
            XX_TEST_EXPECT_TRUE(io->deltas[6].type == DT::TextToken);
            XX_TEST_EXPECT_EQ(io->deltas[6].text, std::string{"x"});
        }

        // 节点结束: 未收尾的正文段定稿成行, 之后才是 NodeEnd
        bridgeCb(neograph::graph::GraphEvent{ET::NODE_END, "llm", neograph::json::object()});
        XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{9});
        if (io->deltas.size() == 9) {
            XX_TEST_EXPECT_TRUE(io->deltas[7].type == DT::InsertMessage);
            if (io->deltas[7].message) {
                XX_TEST_EXPECT_TRUE(io->deltas[7].message->role == VM::Role::Assistant);
                XX_TEST_EXPECT_EQ(io->deltas[7].message->text, std::string{"x"});
            }
            XX_TEST_EXPECT_TRUE(io->deltas[8].type == DT::NodeEnd);
        }
        XX_TEST_EXPECT_EQ(session->viewMessages.size(), size_t{2});
        if (session->viewMessages.size() == 2) {
            XX_TEST_EXPECT_TRUE(session->viewMessages[0].role == VM::Role::Think);
            XX_TEST_EXPECT_EQ(session->viewMessages[0].text, std::string{"ab"});
            XX_TEST_EXPECT_TRUE(session->viewMessages[1].role == VM::Role::Assistant);
            XX_TEST_EXPECT_EQ(session->viewMessages[1].text, std::string{"x"});
        }
    }

    // ---- 流程 2: think -> tool_calls (无正文): 思考行在 CHANNEL_WRITE 前定稿 ----
    {
        auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
        auto session      = std::make_shared<agentxx::agent::Session>();
        auto io           = std::make_shared<TestEbIO>();
        session->io       = io;
        auto bridge       = makeTestBridge(agentContext, session, io);
        auto bridgeCb     = bridge->makeCallback();

        bridgeCb(neograph::graph::GraphEvent{ET::NODE_START, "llm", neograph::json::object()});
        bridgeCb(neograph::graph::GraphEvent{
            ET::LLM_TOKEN,
            "llm",
            makeChunk(neograph::ChatStreamChunk::TYPE_THINKING, "t")
        });
        co_await sleepMs(50);

        neograph::json msgJson{
            {"role",       "assistant"},
            {"content",    ""         },
            {"tool_calls",
             neograph::json::array({
                 neograph::json{{"id", "call_1"}, {"name", "bash"}, {"arguments", "{}"}},
             })                       },
        };
        bridgeCb(neograph::graph::GraphEvent{
            ET::CHANNEL_WRITE,
            "llm",
            neograph::json{{"channel", "messages"}, {"value", neograph::json::array({msgJson})}}
        });

        // NodeStart, StreamBegin, Think(t), InsertMessage(思考行), InsertMessage(工具行)
        XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{5});
        if (io->deltas.size() == 5) {
            XX_TEST_EXPECT_TRUE(io->deltas[3].type == DT::InsertMessage);
            if (io->deltas[3].message) {
                XX_TEST_EXPECT_TRUE(io->deltas[3].message->role == VM::Role::Think);
                XX_TEST_EXPECT_TRUE(io->deltas[3].message->durationMs >= 40);
            }
            XX_TEST_EXPECT_TRUE(io->deltas[4].type == DT::InsertMessage);
            if (io->deltas[4].message) {
                XX_TEST_EXPECT_TRUE(io->deltas[4].message->role == VM::Role::Tool);
                XX_TEST_EXPECT_EQ(io->deltas[4].message->tool->toolName, std::string{"bash"});
            }
        }
        XX_TEST_EXPECT_EQ(session->viewMessages.size(), size_t{2});
    }

    // ---- 流程 3: 纯思考流 -> NodeEnd 前定稿 ----
    {
        auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
        auto session      = std::make_shared<agentxx::agent::Session>();
        auto io           = std::make_shared<TestEbIO>();
        session->io       = io;
        auto bridge       = makeTestBridge(agentContext, session, io);
        auto bridgeCb     = bridge->makeCallback();

        bridgeCb(neograph::graph::GraphEvent{ET::NODE_START, "llm", neograph::json::object()});
        bridgeCb(neograph::graph::GraphEvent{
            ET::LLM_TOKEN,
            "llm",
            makeChunk(neograph::ChatStreamChunk::TYPE_THINKING, "z")
        });
        co_await sleepMs(50);
        bridgeCb(neograph::graph::GraphEvent{ET::NODE_END, "llm", neograph::json::object()});

        // NodeStart, StreamBegin, Think(z), InsertMessage(思考行), NodeEnd
        XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{5});
        if (io->deltas.size() == 5) {
            XX_TEST_EXPECT_TRUE(io->deltas[3].type == DT::InsertMessage);
            if (io->deltas[3].message) {
                XX_TEST_EXPECT_TRUE(io->deltas[3].message->role == VM::Role::Think);
                XX_TEST_EXPECT_EQ(io->deltas[3].message->text, std::string{"z"});
                XX_TEST_EXPECT_TRUE(io->deltas[3].message->durationMs >= 40);
            }
            XX_TEST_EXPECT_TRUE(io->deltas[4].type == DT::NodeEnd);
        }
    }

    // ---- 流程 4: 纯正文流: 只有正文行 ----
    {
        auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
        auto session      = std::make_shared<agentxx::agent::Session>();
        auto io           = std::make_shared<TestEbIO>();
        session->io       = io;
        auto bridge       = makeTestBridge(agentContext, session, io);
        auto bridgeCb     = bridge->makeCallback();

        bridgeCb(neograph::graph::GraphEvent{ET::NODE_START, "llm", neograph::json::object()});
        bridgeCb(neograph::graph::GraphEvent{
            ET::LLM_TOKEN,
            "llm",
            makeChunk(neograph::ChatStreamChunk::TYPE_CONTENT, "y")
        });
        bridgeCb(neograph::graph::GraphEvent{ET::NODE_END, "llm", neograph::json::object()});

        // NodeStart, StreamBegin, Text(y), InsertMessage(正文行), NodeEnd
        XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{5});
        for (const auto& d : io->deltas) {
            // 没有思考段: 不产出 think_token
            XX_TEST_EXPECT_FALSE(d.type == DT::ThinkToken);
        }
        if (io->deltas.size() == 5 && io->deltas[3].message) {
            XX_TEST_EXPECT_TRUE(io->deltas[3].message->role == VM::Role::Assistant);
            XX_TEST_EXPECT_EQ(io->deltas[3].message->text, std::string{"y"});
        }
    }

    // ---- 流程 5: 流式文本与 CHANNEL_WRITE 一致: 不额外发整条覆盖 ----
    {
        auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
        auto session      = std::make_shared<agentxx::agent::Session>();
        auto io           = std::make_shared<TestEbIO>();
        session->io       = io;
        auto bridge       = makeTestBridge(agentContext, session, io);
        auto bridgeCb     = bridge->makeCallback();

        bridgeCb(neograph::graph::GraphEvent{ET::NODE_START, "llm", neograph::json::object()});
        bridgeCb(neograph::graph::GraphEvent{
            ET::LLM_TOKEN,
            "llm",
            makeChunk(neograph::ChatStreamChunk::TYPE_THINKING, "thinking...")
        });
        co_await sleepMs(50);

        // 切换到正文: 思考段先定稿
        bridgeCb(neograph::graph::GraphEvent{
            ET::LLM_TOKEN,
            "llm",
            makeChunk(neograph::ChatStreamChunk::TYPE_CONTENT, "ans")
        });

        neograph::json msgJson{
            {"role",              "assistant"  },
            {"content",           "ans"        },
            {"reasoning_content", "thinking..."},
        };
        bridgeCb(neograph::graph::GraphEvent{
            ET::CHANNEL_WRITE,
            "llm",
            neograph::json{{"channel", "messages"}, {"value", neograph::json::array({msgJson})}}
        });

        // NodeStart, StreamBegin(Think), Think, InsertMessage(思考行),
        // StreamBegin(Assistant), Text(ans), InsertMessage(正文行)
        XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{7});
        for (const auto& d : io->deltas) {
            // 文本与段一致: 不再追加整条覆盖
            XX_TEST_EXPECT_FALSE(d.type == DT::UpdateMessage);
        }

        // session->viewMessages 包含 Think + Assistant
        XX_TEST_EXPECT_EQ(session->viewMessages.size(), size_t{2});
        if (session->viewMessages.size() == 2) {
            const auto& th = session->viewMessages[0];
            XX_TEST_EXPECT_TRUE(th.role == agentxx::agent::ViewMessage::Role::Think);
            XX_TEST_EXPECT_EQ(th.text, std::string{"thinking..."});
            XX_TEST_EXPECT_TRUE(th.startTimeMs > 0);
            XX_TEST_EXPECT_TRUE(th.durationMs >= 40);

            const auto& asst = session->viewMessages[1];
            XX_TEST_EXPECT_TRUE(asst.role == agentxx::agent::ViewMessage::Role::Assistant);
            XX_TEST_EXPECT_EQ(asst.text, std::string{"ans"});
            XX_TEST_EXPECT_TRUE(asst.startTimeMs > 0);
        }
    }

    co_return;
}

/// 验证生成阶段占位提示 (GenStatus): 模型节点开始执行 / 工具调用开始 / 生成结束
/// - 生成开始标记 (模型调用节点在请求发出后下发) -> GenStatus::Waiting
/// - 工具调用开始标记 (Provider 首个 tool_call 分片) -> GenStatus::ToolCall (含工具名)
/// - 工具调用开始时思考段已定稿: 思考行先于占位提示到达
/// - 节点结束 -> GenStatus::Done (清除占位提示)
/// - 生成阶段标记不是模型输出文本: 不产出文本 token, 不参与 tps 统计
asio::awaitable<void> test_eventbridge_gen_status() {
    auto makeChunk = [](int type, std::string data) {
        return neograph::json{
            {"type", type           },
            {"data", std::move(data)}
        };
    };
    using ET = neograph::graph::GraphEvent::Type;
    using DT = agentxx::agent::WireDelta::Type;
    using GP = agentxx::agent::WireDelta::GenPhase;

    auto agentContext         = std::make_shared<agentxx::agent::AgentContext>();
    agentContext->agentConfig = std::make_shared<agentxx::agent::AgentConfig>();
    agentContext->bus
        = std::make_shared<agentxx::events::EventBus>(co_await asio::this_coro::executor);
    auto session  = std::make_shared<agentxx::agent::Session>();
    auto io       = std::make_shared<TestEbIO>();
    session->io   = io;
    auto bridge   = makeTestBridge(agentContext, session, io);
    auto bridgeCb = bridge->makeCallback();

    bridgeCb(neograph::graph::GraphEvent{ET::NODE_START, "llm", neograph::json::object()});
    // 生成开始 (请求已发出, 首个 token 未到达)
    bridgeCb(neograph::graph::GraphEvent{
        ET::LLM_TOKEN,
        "llm",
        makeChunk(agentxx::protocol::chunk_type::kGenerationStart, "")
    });
    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{2});
    if (io->deltas.size() == 2) {
        XX_TEST_EXPECT_TRUE(io->deltas[1].type == DT::GenStatus);
        XX_TEST_EXPECT_TRUE(io->deltas[1].genPhase == GP::Waiting);
        XX_TEST_EXPECT_TRUE(io->deltas[1].text.empty());
    }

    // 思考 token: 占位提示之后仍是正常的思考流 (stream_begin + think_token)
    bridgeCb(neograph::graph::GraphEvent{
        ET::LLM_TOKEN,
        "llm",
        makeChunk(neograph::ChatStreamChunk::TYPE_THINKING, "thinking")
    });
    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{4});
    if (io->deltas.size() == 4) {
        XX_TEST_EXPECT_TRUE(io->deltas[2].type == DT::StreamBegin);
        XX_TEST_EXPECT_TRUE(io->deltas[2].streamRole == VM::Role::Think);
        XX_TEST_EXPECT_TRUE(io->deltas[3].type == DT::ThinkToken);
        XX_TEST_EXPECT_EQ(io->deltas[3].text, std::string{"thinking"});
    }

    // 工具调用开始 (思考后直接 tool_calls): 先定稿思考行, 再下发占位提示
    bridgeCb(neograph::graph::GraphEvent{
        ET::LLM_TOKEN,
        "llm",
        makeChunk(agentxx::protocol::chunk_type::kToolCallStart, "read_file")
    });
    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{6});
    if (io->deltas.size() == 6) {
        // 定稿行: 思考全文 + 段耗时
        XX_TEST_EXPECT_TRUE(io->deltas[4].type == DT::InsertMessage);
        if (io->deltas[4].message) {
            XX_TEST_EXPECT_TRUE(io->deltas[4].message->role == VM::Role::Think);
            XX_TEST_EXPECT_EQ(io->deltas[4].message->text, std::string{"thinking"});
        }
        XX_TEST_EXPECT_TRUE(io->deltas[5].type == DT::GenStatus);
        XX_TEST_EXPECT_TRUE(io->deltas[5].genPhase == GP::ToolCall);
        XX_TEST_EXPECT_EQ(io->deltas[5].toolName, std::string{"read_file"});
    }

    // 节点结束: 生成结束, 先下发 Done 清除占位提示, 再发 NodeEnd
    bridgeCb(neograph::graph::GraphEvent{ET::NODE_END, "llm", neograph::json::object()});
    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{8});
    if (io->deltas.size() == 8) {
        XX_TEST_EXPECT_TRUE(io->deltas[6].type == DT::GenStatus);
        XX_TEST_EXPECT_TRUE(io->deltas[6].genPhase == GP::Done);
        XX_TEST_EXPECT_TRUE(io->deltas[7].type == DT::NodeEnd);
    }

    // 生成阶段标记不计入 tps 输出 (只统计真实输出 token)
    XX_TEST_EXPECT_TRUE(bridge->takeTurnTps() >= 0.0);

    co_return;
}

/// 验证出错时清除生成占位提示: 中途出错 -> 未定稿的段先定稿 + GenStatus::Done
asio::awaitable<void> test_eventbridge_gen_status_error() {
    auto makeChunk = [](int type, std::string data) {
        return neograph::json{
            {"type", type           },
            {"data", std::move(data)}
        };
    };
    using ET = neograph::graph::GraphEvent::Type;
    using DT = agentxx::agent::WireDelta::Type;
    using GP = agentxx::agent::WireDelta::GenPhase;

    auto agentContext         = std::make_shared<agentxx::agent::AgentContext>();
    agentContext->agentConfig = std::make_shared<agentxx::agent::AgentConfig>();
    auto session              = std::make_shared<agentxx::agent::Session>();
    auto io                   = std::make_shared<TestEbIO>();
    auto bridge               = makeTestBridge(agentContext, session, io);
    auto bridgeCb             = bridge->makeCallback();

    bridgeCb(neograph::graph::GraphEvent{ET::NODE_START, "llm", neograph::json::object()});
    bridgeCb(neograph::graph::GraphEvent{
        ET::LLM_TOKEN,
        "llm",
        makeChunk(agentxx::protocol::chunk_type::kGenerationStart, "")
    });
    bridgeCb(neograph::graph::GraphEvent{ET::ERROR, "llm", neograph::json(std::string{"boom"})});

    // NodeStart, GenStatus(Waiting), 结算包 (无思考段, 不产生), GenStatus(Done)
    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{3});
    if (io->deltas.size() == 3) {
        XX_TEST_EXPECT_TRUE(io->deltas[1].genPhase == GP::Waiting);
        XX_TEST_EXPECT_TRUE(io->deltas[2].type == DT::GenStatus);
        XX_TEST_EXPECT_TRUE(io->deltas[2].genPhase == GP::Done);
    }

    // 未下发过占位提示的轮次: 出错时不额外下发 Done (避免无意义增量)
    auto io2       = std::make_shared<TestEbIO>();
    auto bridge2   = makeTestBridge(agentContext, session, io2);
    auto bridgeCb2 = bridge2->makeCallback();
    bridgeCb2(neograph::graph::GraphEvent{ET::NODE_START, "llm", neograph::json::object()});
    bridgeCb2(neograph::graph::GraphEvent{ET::ERROR, "llm", neograph::json(std::string{"boom"})});
    XX_TEST_EXPECT_EQ(io2->deltas.size(), size_t{1});

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
        co_await test_eventbridge_think_duration();
        co_await test_eventbridge_tps();
        co_await test_eventbridge_gen_status();
        co_await test_eventbridge_gen_status_error();
        co_await test_eventbridge_turn_tps();
    } catch (const std::exception& e) {
        TEST_FAIL << "event_bridge suite exception: " << e.what() << std::endl;
        g_eb_failed++;
    }
    co_return TestResult{g_eb_passed, g_eb_failed};
}

} // namespace test
} // namespace agentxx
