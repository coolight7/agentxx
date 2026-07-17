#pragma once

#include <asio/awaitable.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/redirect_error.hpp>

#include "agentxx/tools/rag_search.h"
#include "agentxx/util/string_util.h"
#include "test_framework.h"
#include <iostream>
#include <string>

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_rag_passed
#define XX_TEST_FAILED g_rag_failed

namespace agentxx {
namespace test {

extern int g_rag_passed;
extern int g_rag_failed;

asio::awaitable<TestResult> run_rag_search_tools_tests(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext);

} // namespace test
} // namespace agentxx
