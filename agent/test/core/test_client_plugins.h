#pragma once

#include "agentxx/agent/context.h"
#include <asio/awaitable.hpp>
#include <string>

#include "test_framework.h"
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_client_plugin_passed
#define XX_TEST_FAILED g_client_plugin_failed

namespace agentxx {
namespace test {

extern int g_client_plugin_passed;
extern int g_client_plugin_failed;

asio::awaitable<TestResult> run_client_plugin_tests();

} // namespace test
} // namespace agentxx
