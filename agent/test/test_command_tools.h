#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/tools/execute_command.h"
#include "asio/dispatch.hpp"
#include "test_framework.h"
#include <iostream>
#include <string>

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_cmd_passed
#define XX_TEST_FAILED g_cmd_failed

namespace agentxx {
namespace test {

extern int g_cmd_passed;
extern int g_cmd_failed;

asio::awaitable<TestResult> run_command_tools_tests(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext);

} // namespace test
} // namespace agentxx