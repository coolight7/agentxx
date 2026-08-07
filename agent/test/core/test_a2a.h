#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>

#include "test_framework.h"

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_a2a_passed
#define XX_TEST_FAILED g_a2a_failed

namespace agentxx {
namespace test {

extern int g_a2a_passed;
extern int g_a2a_failed;

asio::awaitable<TestResult> run_a2a_tests();

} // namespace test
} // namespace agentxx
