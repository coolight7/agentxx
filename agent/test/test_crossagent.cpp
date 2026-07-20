#include "test_crossagent.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/events.h"
#include "agentxx/middlewares/subagent_supervisor.h"
#include "agentxx/tools/cross_agent_query.h"
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

int g_ca_passed = 0;
int g_ca_failed = 0;

/// 验证: 跨 agent 查询请求-响应闭环 (模拟 server)
asio::awaitable<void> test_crossagent_request_response() {

  auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
  agentContext->bus = std::make_shared<agentxx::middleware::EventBus>(
      co_await asio::this_coro::executor);

  auto &rr =
      agentContext->bus->getRR<events::ReqCrossAgent, events::RespCrossAgent>(
          events::Topic::CrossAgent);
  rr.serve([](const events::ReqCrossAgent &req,
              size_t corrId) -> asio::awaitable<events::RespCrossAgent> {
    XX_TEST_EXPECT_TRUE(corrId > 0);
    XX_TEST_EXPECT_EQ(req.toAgent, std::string{"research"});
    XX_TEST_EXPECT_EQ(req.fromAgent, std::string{"coder"});
    XX_TEST_EXPECT_EQ(req.message, std::string{"what is foo?"});
    co_return events::RespCrossAgent{
        .content = fmt::format("answer from {}: foo is 42", req.toAgent),
    };
  });

  auto resp = co_await agentContext->bus
                  ->request<events::ReqCrossAgent, events::RespCrossAgent>(
                      events::Topic::CrossAgent,
                      events::ReqCrossAgent{
                          .fromAgent = "coder",
                          .fromThreadId = "t1",
                          .toAgent = "research",
                          .message = "what is foo?",
                      },
                      std::chrono::seconds(5));

  XX_TEST_EXPECT_TRUE(resp.has_value());
  if (resp.has_value()) {
    XX_TEST_EXPECT_EQ(resp->content,
                      std::string{"answer from research: foo is 42"});
    XX_TEST_EXPECT_TRUE(!resp->hasError);
  }

  co_return;
}

/// 验证: 跨 agent 查询无 server 时返回错误 (SubagentSupervisor 路由)
asio::awaitable<void> test_crossagent_no_running_agent() {

  auto agentConfig = std::make_shared<agentxx::agent::AgentConfig>();
  auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
  agentContext->agentConfig = agentConfig;
  agentContext->bus = std::make_shared<agentxx::middleware::EventBus>(
      co_await asio::this_coro::executor);

  agentxx::middleware::SubagentSupervisor supervisor{agentContext};
  co_await supervisor.start();

  // 无 subagent 运行中, 查询应返回错误
  auto resp = co_await agentContext->bus
                  ->request<events::ReqCrossAgent, events::RespCrossAgent>(
                      events::Topic::CrossAgent,
                      events::ReqCrossAgent{
                          .fromAgent = "coder",
                          .fromThreadId = "t1",
                          .toAgent = "nonexistent",
                          .message = "hello",
                      },
                      std::chrono::seconds(5));

  XX_TEST_EXPECT_TRUE(resp.has_value());
  if (resp.has_value()) {
    XX_TEST_EXPECT_TRUE(resp->hasError);
    XX_TEST_EXPECT_TRUE(resp->errorMessage.find("not running") !=
                        std::string::npos);
  }

  supervisor.stop();
  co_return;
}

/// 验证: 批量 subagent 请求-响应闭环 (模拟 server)
asio::awaitable<void> test_subagent_batch_request_response() {

  auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
  agentContext->bus = std::make_shared<agentxx::middleware::EventBus>(
      co_await asio::this_coro::executor);

  auto &batchRR =
      agentContext->bus
          ->getRR<events::ReqSubagentBatch, events::RespSubagentBatch>(
              events::Topic::SubagentBatch);
  batchRR.serve([](const events::ReqSubagentBatch &req,
                   size_t) -> asio::awaitable<events::RespSubagentBatch> {
    events::RespSubagentBatch resp;
    XX_TEST_EXPECT_EQ(req.tasks.size(), size_t{3});
    for (const auto &t : req.tasks) {
      resp.results.push_back(events::RespSubagentBatchItem{
          .resultId = t.resultId,
          .content = fmt::format("out_{}", t.subagentName),
      });
    }
    co_return resp;
  });

  events::ReqSubagentBatch batchReq{
      .parentAgentName = "parent",
      .parentThreadId = "t1",
  };
  batchReq.tasks.push_back(events::SubagentBatchItem{
      .subagentName = "a",
      .message = "m1",
      .resultId = "call_1",
  });
  batchReq.tasks.push_back(events::SubagentBatchItem{
      .subagentName = "b",
      .message = "m2",
      .resultId = "call_2",
  });
  batchReq.tasks.push_back(events::SubagentBatchItem{
      .subagentName = "c",
      .message = "m3",
      .resultId = "call_3",
  });

  auto resp =
      co_await agentContext->bus
          ->request<events::ReqSubagentBatch, events::RespSubagentBatch>(
              events::Topic::SubagentBatch, std::move(batchReq),
              std::chrono::seconds(5));

  XX_TEST_EXPECT_TRUE(resp.has_value());
  if (resp.has_value()) {
    XX_TEST_EXPECT_EQ(resp->results.size(), size_t{3});
    if (resp->results.size() == 3) {
      XX_TEST_EXPECT_EQ(resp->results[0].content, std::string{"out_a"});
      XX_TEST_EXPECT_EQ(resp->results[0].resultId, std::string{"call_1"});
      XX_TEST_EXPECT_EQ(resp->results[1].content, std::string{"out_b"});
      XX_TEST_EXPECT_EQ(resp->results[2].content, std::string{"out_c"});
    }
  }

  co_return;
}

/// 验证: CrossAgentQueryTool 工具定义正确
void test_crossagent_query_tool_definition() {

  auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
  agentxx::tools::CrossAgentQueryTool tool{agentContext};

  XX_TEST_EXPECT_EQ(tool.get_name(), std::string{"cross_agent_query"});
  auto def = tool.get_definition();
  XX_TEST_EXPECT_EQ(std::string{def.name}, std::string{"cross_agent_query"});
  // definition schema 应包含 to_agent 和 message
  auto props = def.parameters["properties"];
  XX_TEST_EXPECT_TRUE(props.contains("to_agent"));
  XX_TEST_EXPECT_TRUE(props.contains("message"));
}

/// 验证: 批量 subagent 空任务返回空结果
asio::awaitable<void> test_subagent_batch_empty() {

  auto agentContext = std::make_shared<agentxx::agent::AgentContext>();
  agentContext->bus = std::make_shared<agentxx::middleware::EventBus>(
      co_await asio::this_coro::executor);

  // SubagentSupervisor 的 batch server 处理空任务
  agentxx::middleware::SubagentSupervisor supervisor{agentContext};
  co_await supervisor.start();

  auto resp =
      co_await agentContext->bus
          ->request<events::ReqSubagentBatch, events::RespSubagentBatch>(
              events::Topic::SubagentBatch,
              events::ReqSubagentBatch{
                  .parentAgentName = "p",
                  .parentThreadId = "t",
              },
              std::chrono::seconds(5));

  XX_TEST_EXPECT_TRUE(resp.has_value());
  if (resp.has_value()) {
    XX_TEST_EXPECT_EQ(resp->results.size(), size_t{0});
  }

  supervisor.stop();
  co_return;
}

asio::awaitable<TestResult> run_crossagent_tests() {
  try {
    co_await test_crossagent_request_response();
    co_await test_crossagent_no_running_agent();
    co_await test_subagent_batch_request_response();
    test_crossagent_query_tool_definition();
    co_await test_subagent_batch_empty();
  } catch (const std::exception &e) {
    TEST_FAIL << "crossagent suite exception: " << e.what() << std::endl;
    g_ca_failed++;
  }
  co_return TestResult{g_ca_passed, g_ca_failed};
}

} // namespace test
} // namespace agentxx
