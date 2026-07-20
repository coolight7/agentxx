#pragma once

#include "agentxx/util/http_server.h"
#include <asio/awaitable.hpp>
#include <memory>
#include <neograph/api.h>
#include <neograph/json.h>
#include <string>
#include <thread>

#include "test_framework.h"
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_da_passed
#define XX_TEST_FAILED g_da_failed

namespace agentxx {
namespace test {

extern int g_da_passed;
extern int g_da_failed;

// ===========================================================================
// Local LLM Simulator — an OpenAI-compatible HTTP server
// ===========================================================================
extern std::string g_da_sim_response_content;
extern int g_da_sim_prompt_tokens;
extern int g_da_sim_completion_tokens;
extern neograph::json g_da_sim_tool_calls;

struct DaSimServer {
  std::unique_ptr<agentxx::util::HttpServer> svr;
  std::thread thr;
  uint16_t port = 0;

  DaSimServer() = default;
  DaSimServer(DaSimServer &&o) noexcept;
  DaSimServer &operator=(DaSimServer &&o) noexcept;
  DaSimServer(const DaSimServer &) = delete;
  DaSimServer &operator=(const DaSimServer &) = delete;
  ~DaSimServer();

  void stop();
};

asio::awaitable<TestResult> run_deepagent_tests();

} // namespace test
} // namespace agentxx
