#include "test_event_bridge.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/events.h"
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

    std::vector<agentxx::agent::Delta> deltas;

    void sendToPeer(agentxx::agent::WireMessage msg) override {
        if (auto* d = std::get_if<agentxx::agent::Delta>(&msg)) {
            deltas.push_back(*d);
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
static std::shared_ptr<agentxx::middleware::EventBridge> makeTestBridge(
    std::shared_ptr<agentxx::agent::AgentContext> agentContext,
    std::shared_ptr<agentxx::agent::Session>      session,
    std::shared_ptr<TestEbIO>                     io,
    neograph::graph::GraphStreamCallback          origCb = nullptr
) {
    return std::make_shared<agentxx::middleware::EventBridge>(
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
        = std::make_shared<agentxx::middleware::EventBus>(co_await asio::this_coro::executor);
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
            lastThreadId  = e.threadId;
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
        = std::make_shared<agentxx::middleware::EventBus>(co_await asio::this_coro::executor);
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

/// 验证 MessageTip: CHANNEL_WRITE "message_tip" -> Delta::MessageTip (warning/error/info)
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
                 {"tip_type", "warning"},
                 {"text", "LLM API 调用失败，6 秒后自动重试 (2/5)"},
             }},
                       }
    });
    XX_TEST_EXPECT_EQ(io->deltas.size(), size_t{1});
    if (!io->deltas.empty()) {
        XX_TEST_EXPECT_TRUE(io->deltas[0].type == agentxx::agent::Delta::Type::MessageTip);
        XX_TEST_EXPECT_TRUE(io->deltas[0].tipType == agentxx::agent::Delta::TipType::Warning);
        XX_TEST_EXPECT_EQ(
            io->deltas[0].text,
            std::string{"LLM API 调用失败，6 秒后自动重试 (2/5)"}
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
                 {"tip_type", "error"},
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

asio::awaitable<TestResult> run_event_bridge_tests() {
    g_eb_passed = 0;
    g_eb_failed = 0;
    try {
        co_await test_eventbridge_token();
        co_await test_eventbridge_nullbus_passthrough();
        co_await test_eventbridge_error();
        co_await test_eventbridge_message_tip();
        co_await test_eventbridge_channel_write_messages();
        co_await test_eventbridge_node_delta();
    } catch (const std::exception& e) {
        TEST_FAIL << "event_bridge suite exception: " << e.what() << std::endl;
        g_eb_failed++;
    }
    co_return TestResult{g_eb_passed, g_eb_failed};
}

} // namespace test
} // namespace agentxx
