#pragma once

#include "agentxx/agent/context.h"
#include <asio/awaitable.hpp>
#include <memory>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_ws_passed
#define XX_TEST_FAILED g_ws_failed

namespace agentxx {
namespace test {

extern int g_ws_passed;
extern int g_ws_failed;

asio::awaitable<TestResult> run_web_search_tools_tests(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext);

} // namespace test
} // namespace agentxx