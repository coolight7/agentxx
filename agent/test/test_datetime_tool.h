#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/tools/system.h"
#include "neograph/neograph.h"
#include "test_framework.h"
#include <asio/awaitable.hpp>
#include <iostream>
#include <regex>
#include <string>
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_dt_passed
#define XX_TEST_FAILED g_dt_failed

namespace agentxx {
namespace test {

extern int g_dt_passed;
extern int g_dt_failed;

asio::awaitable<TestResult> run_datetime_tool_tests(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext);

} // namespace test
} // namespace agentxx