#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_eb_passed
#define XX_TEST_FAILED g_eb_failed

namespace agentxx {
namespace test {

extern int g_eb_passed;
extern int g_eb_failed;

asio::awaitable<TestResult> run_event_bridge_tests();

} // namespace test
} // namespace agentxx
