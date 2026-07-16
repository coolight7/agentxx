#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/agent/subagent_supervisor.h"
#include "agentxx/events.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/tools/sub_agent.h"
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

inline static int g_sb_passed = 0;
inline static int g_sb_failed = 0;

#define SB_TEST(name) std::cout << "  [" << (name) << "]" << std::endl
#define SB_EXPECT_TRUE(expr)                                                   \
  do {                                                                         \
    if (expr) {                                                                \
      g_sb_passed++;                                                           \
    } else {                                                                   \
      g_sb_failed++;                                                           \
      std::cerr << "    FAIL at line " << __LINE__ << ": expected true"        \
                << std::endl;                                                  \
    }                                                                          \
  } while (0)
#define SB_EXPECT_EQ(a, b)                                                     \
  do {                                                                         \
    auto _a = (a);                                                             \
    auto _b = (b);                                                             \
    if (_a == _b) {                                                            \
      g_sb_passed++;                                                           \
    } else {                                                                   \
      g_sb_failed++;                                                           \
      std::cerr << "    FAIL at line " << __LINE__ << ": expected " << _b      \
                << ", got " << _a << std::endl;                                \
    }                                                                          \
  } while (0)

/// 验证: bus.request<ReqSubagentStart, RespSubagentResult> 请求-响应闭环
/// - 注册一个模拟 server, 验证请求参数传递与响应回填
inline asio::awaitable<void> test_subagent_bus_request_response() {
  SB_TEST("subagent bus: request -> mock server -> response");

  auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
  agentContext->bus = std::make_shared<agentxx::middleware::EventBus>(
      co_await asio::this_coro::executor);

  // 注册模拟 server
  auto &rr =
      agentContext->bus->getRR<events::ReqSubagentStart,
                                events::RespSubagentResult>(
          events::Topic::Subagent);
  rr.serve([](const events::ReqSubagentStart &req,
              size_t corrId) -> asio::awaitable<events::RespSubagentResult> {
    SB_EXPECT_TRUE(corrId > 0);
    SB_EXPECT_EQ(req.subagentName, std::string{"research"});
    SB_EXPECT_EQ(req.message, std::string{"find foo"});
    SB_EXPECT_EQ(req.resultId, std::string{"call_1"});
    co_return events::RespSubagentResult{
        .content = fmt::format("result_for_{}", req.subagentName),
    };
  });

  auto resp =
      co_await agentContext->bus->request<events::ReqSubagentStart,
                                           events::RespSubagentResult>(
          events::Topic::Subagent,
          events::ReqSubagentStart{
              .parentAgentName = "parent",
              .parentThreadId = "t1",
              .subagentName = "research",
              .systemPrompt = "",
              .message = "find foo",
              .resultId = "call_1",
          },
          std::chrono::seconds(5));

  SB_EXPECT_TRUE(resp.has_value());
  if (resp.has_value()) {
    SB_EXPECT_EQ(resp->content, std::string{"result_for_research"});
    SB_EXPECT_TRUE(!resp->hasError);
  }

  co_return;
}

/// 验证: SubagentProgress 事件发布与订阅
inline asio::awaitable<void> test_subagent_progress_events() {
  SB_TEST("subagent bus: SubagentProgress publish/subscribe");

  auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
  agentContext->bus = std::make_shared<agentxx::middleware::EventBus>(
      co_await asio::this_coro::executor);

  std::atomic<int> tokenCount{0};
  std::string lastToken;
  std::string lastSubagentId;

  agentContext->bus->get<events::EventSubagentProgress>(
      events::Topic::SubagentProgress)
      .subscribe([&](const events::EventSubagentProgress &e)
                     -> asio::awaitable<void> {
        if (e.kind == "token") {
          tokenCount++;
          lastToken = e.data;
          lastSubagentId = e.subagentId;
        }
        co_return;
      });

  // 发布几个 token 进度事件
  co_await agentContext->bus->publish<events::EventSubagentProgress>(
      events::Topic::SubagentProgress,
      events::EventSubagentProgress{
          .subagentId = "subagent_research",
          .agentName = "research",
          .kind = "token",
          .data = "Hello",
      });
  co_await agentContext->bus->publish<events::EventSubagentProgress>(
      events::Topic::SubagentProgress,
      events::EventSubagentProgress{
          .subagentId = "subagent_research",
          .agentName = "research",
          .kind = "token",
          .data = " World",
      });

  SB_EXPECT_EQ(tokenCount.load(), 2);
  SB_EXPECT_EQ(lastToken, std::string{" World"});
  SB_EXPECT_EQ(lastSubagentId, std::string{"subagent_research"});

  co_return;
}

/// 验证: SubagentSupervisor 对不存在 subagent 的错误处理
inline asio::awaitable<void> test_subagent_supervisor_notfound() {
  SB_TEST("subagent supervisor: not-found error handling");

  auto agentConfig = std::make_shared<agentxx::agent::AgentConfig>();
  auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
  agentContext->agentConfig = agentConfig;
  agentContext->bus = std::make_shared<agentxx::middleware::EventBus>(
      co_await asio::this_coro::executor);

  // 不设置 subagentManagerToolPtr (或设为空)
  // SubagentSupervisor 应返回错误而非崩溃

  agentxx::agent::SubagentSupervisor supervisor{agentContext};
  co_await supervisor.start();

  auto resp =
      co_await agentContext->bus->request<events::ReqSubagentStart,
                                           events::RespSubagentResult>(
          events::Topic::Subagent,
          events::ReqSubagentStart{
              .parentAgentName = "parent",
              .parentThreadId = "t1",
              .subagentName = "nonexistent",
              .systemPrompt = "",
              .message = "test",
              .resultId = "call_1",
          },
          std::chrono::seconds(5));

  SB_EXPECT_TRUE(resp.has_value());
  if (resp.has_value()) {
    SB_EXPECT_TRUE(resp->hasError);
  }

  supervisor.stop();
  co_return;
}

/// 验证: subagent 总线超时返回 nullopt
inline asio::awaitable<void> test_subagent_bus_timeout() {
  SB_TEST("subagent bus: request timeout");

  auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
  agentContext->bus = std::make_shared<agentxx::middleware::EventBus>(
      co_await asio::this_coro::executor);

  // 注册一个永不响应的 server
  auto &rr =
      agentContext->bus->getRR<events::ReqSubagentStart,
                                events::RespSubagentResult>(
          events::Topic::Subagent);
  rr.serve([](const events::ReqSubagentStart &,
              size_t) -> asio::awaitable<events::RespSubagentResult> {
    auto timer = asio::steady_timer(co_await asio::this_coro::executor,
                                    std::chrono::seconds(1));
    co_await timer.async_wait(asio::use_awaitable);
    co_return events::RespSubagentResult{.content = "too late"};
  });

  auto resp =
      co_await agentContext->bus->request<events::ReqSubagentStart,
                                           events::RespSubagentResult>(
          events::Topic::Subagent,
          events::ReqSubagentStart{
              .parentAgentName = "p",
              .parentThreadId = "t",
              .subagentName = "x",
              .systemPrompt = "",
              .message = "m",
              .resultId = "r",
          },
          std::chrono::milliseconds(200));

  SB_EXPECT_TRUE(!resp.has_value());

  co_return;
}

inline asio::awaitable<void> run_subagent_bus_tests() {
  std::cout << "=== subagent_bus Tests ===" << std::endl;
  try {
    co_await test_subagent_bus_request_response();
    co_await test_subagent_progress_events();
    co_await test_subagent_supervisor_notfound();
    co_await test_subagent_bus_timeout();
  } catch (const std::exception &e) {
    std::cout << "[FAIL] subagent_bus suite exception: " << e.what()
              << std::endl;
    g_sb_failed++;
  }
  std::cout << "=== subagent_bus Tests DONE (pass=" << g_sb_passed
            << " fail=" << g_sb_failed << ") ===" << std::endl;
  co_return;
}

} // namespace test
} // namespace agentxx
