#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/expand/codegraph_manager.h"
#include "agentxx/tools/codegraph_tool.h"
#include "neograph/neograph.h"
#include "test_framework.h"
#include <asio/awaitable.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#if AGENTXX_ENABLE_CODEGRAPH

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_cg_passed
#define XX_TEST_FAILED g_cg_failed

namespace agentxx {
namespace test {

extern int g_cg_passed;
extern int g_cg_failed;

asio::awaitable<TestResult> run_codegraph_tools_tests(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext);

} // namespace test
} // namespace agentxx

#else

namespace agentxx {
namespace test {

inline asio::awaitable<TestResult> run_codegraph_tools_tests(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  co_return TestResult{0, 0};
}

} // namespace test
} // namespace agentxx

#endif