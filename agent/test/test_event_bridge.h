#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/event_bridge.h"
#include "agentxx/events.h"
#include "agentxx/middlewares/event_stream.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/use_awaitable.hpp"
#include <atomic>
#include <iostream>
#include <memory>
#include <string>

#include "test_framework.h"

namespace agentxx {
namespace test {

inline static int g_eb_passed = 0;
inline static int g_eb_failed = 0;

#define EB_EXPECT_TRUE(expr)                                                   \
  do {                                                                         \
    if (expr) {                                                                \
      g_eb_passed++;                                                           \
    } else {                                                                   \
      g_eb_failed++;                                                           \
      TEST_FAIL << "expected true at line " << __LINE__ << std::endl;          \
    }                                                                          \
  } while (0)
#define EB_EXPECT_EQ(a, b)                                                     \
  do {                                                                         \
    auto _a = (a);                                                             \
    auto _b = (b);                                                             \
    if (_a == _b) {                                                            \
      g_eb_passed++;                                                           \
    } else {                                                                   \
      g_eb_failed++;                                                           \
      TEST_FAIL << "expected " << _b << ", got " << _a << " at line "          \
                << __LINE__ << std::endl;                                      \
    }                                                                          \
  } while (0)

/// 验证 EventBridge 把 GraphEvent::LLM_TOKEN 翻译成 EventModelToken 发布到 bus,
/// 同时保留原始 callback 的转发行为
inline asio::awaitable<void> test_eventbridge_token() {

  auto agentConfig = std::make_shared<agentxx::agent::AgentConfig>();
  auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
  agentContext->agentConfig = agentConfig;
  agentContext->bus = std::make_shared<agentxx::middleware::EventBus>(
      co_await asio::this_coro::executor);

  // 订阅 ModelToken 事件
  std::atomic<int> tokenEventCount{0};
  std::string lastToken;
  std::string lastAgentName;
  std::string lastThreadId;
  agentContext->bus
      ->get<agentxx::events::EventModelToken>(
          agentxx::events::Topic::ModelToken)
      .subscribe([&](const agentxx::events::EventModelToken &e)
                     -> asio::awaitable<void> {
        tokenEventCount++;
        lastToken = e.token;
        lastAgentName = e.agentName;
        lastThreadId = e.threadId;
        co_return;
      });

  // 原始 callback 计数 (验证 passthrough)
  std::atomic<int> origCbCount{0};
  neograph::graph::GraphStreamCallback origCb =
      [&](const neograph::graph::GraphEvent &event) {
        if (event.type == neograph::graph::GraphEvent::Type::LLM_TOKEN) {
          origCbCount++;
        }
      };

  auto bridgeCb = agentxx::agent::EventBridge::make("testAgent", "thread_42",
                                                    agentContext, origCb);

  // 触发 LLM_TOKEN 事件
  bridgeCb(
      neograph::graph::GraphEvent{neograph::graph::GraphEvent::Type::LLM_TOKEN,
                                  "llm", neograph::json(std::string{"Hello"})});

  // fire-and-forget co_spawn 异步发布; 让出一次让协程跑完
  auto timer = asio::steady_timer(co_await asio::this_coro::executor,
                                  std::chrono::milliseconds(10));
  co_await timer.async_wait(asio::use_awaitable);

  EB_EXPECT_EQ(origCbCount.load(), 1);
  EB_EXPECT_EQ(tokenEventCount.load(), 1);
  EB_EXPECT_EQ(lastToken, std::string{"Hello"});
  EB_EXPECT_EQ(lastAgentName, std::string{"testAgent"});
  EB_EXPECT_EQ(lastThreadId, std::string{"thread_42"});

  // 再触发一次, 验证多次
  bridgeCb(neograph::graph::GraphEvent{
      neograph::graph::GraphEvent::Type::LLM_TOKEN, "llm",
      neograph::json(std::string{" World"})});
  co_await asio::steady_timer(co_await asio::this_coro::executor,
                              std::chrono::milliseconds(10))
      .async_wait(asio::use_awaitable);
  EB_EXPECT_EQ(origCbCount.load(), 2);
  EB_EXPECT_EQ(tokenEventCount.load(), 2);
  EB_EXPECT_EQ(lastToken, std::string{" World"});

  co_return;
}

/// 验证 bus 为空时, EventBridge 仍转发原始 callback
inline asio::awaitable<void> test_eventbridge_nullbus_passthrough() {

  auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
  // 不设置 bus (bus == nullptr)

  std::atomic<int> origCbCount{0};
  neograph::graph::GraphStreamCallback origCb =
      [&](const neograph::graph::GraphEvent &event) {
        if (event.type == neograph::graph::GraphEvent::Type::LLM_TOKEN) {
          origCbCount++;
        }
      };

  auto bridgeCb =
      agentxx::agent::EventBridge::make("a", "t", agentContext, origCb);
  bridgeCb(
      neograph::graph::GraphEvent{neograph::graph::GraphEvent::Type::LLM_TOKEN,
                                  "llm", neograph::json(std::string{"x"})});
  EB_EXPECT_EQ(origCbCount.load(), 1);

  co_return;
}

/// 验证 ERROR 事件发布
inline asio::awaitable<void> test_eventbridge_error() {

  auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
  agentContext->bus = std::make_shared<agentxx::middleware::EventBus>(
      co_await asio::this_coro::executor);

  std::atomic<int> errCount{0};
  std::string lastMsg;
  std::string lastWhere;
  agentContext->bus
      ->get<agentxx::events::EventError>(agentxx::events::Topic::Error)
      .subscribe(
          [&](const agentxx::events::EventError &e) -> asio::awaitable<void> {
            errCount++;
            lastMsg = e.message;
            lastWhere = e.where;
            co_return;
          });

  auto bridgeCb =
      agentxx::agent::EventBridge::make("a", "t", agentContext, nullptr);
  bridgeCb(neograph::graph::GraphEvent{neograph::graph::GraphEvent::Type::ERROR,
                                       "tool_x",
                                       neograph::json(std::string{"boom"})});

  co_await asio::steady_timer(co_await asio::this_coro::executor,
                              std::chrono::milliseconds(10))
      .async_wait(asio::use_awaitable);
  EB_EXPECT_EQ(errCount.load(), 1);
  EB_EXPECT_EQ(lastMsg, std::string{"boom"});
  EB_EXPECT_EQ(lastWhere, std::string{"tool_x"});

  co_return;
}

inline asio::awaitable<TestResult> run_event_bridge_tests() {
  try {
    co_await test_eventbridge_token();
    co_await test_eventbridge_nullbus_passthrough();
    co_await test_eventbridge_error();
  } catch (const std::exception &e) {
    TEST_FAIL << "event_bridge suite exception: " << e.what() << std::endl;
    g_eb_failed++;
  }
  co_return TestResult{g_eb_passed, g_eb_failed};
}

} // namespace test
} // namespace agentxx
