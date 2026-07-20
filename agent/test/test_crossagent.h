#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_ca_passed
#define XX_TEST_FAILED g_ca_failed

namespace agentxx {
namespace test {

extern int g_ca_passed;
extern int g_ca_failed;

asio::awaitable<TestResult> run_crossagent_tests();

} // namespace test
} // namespace agentxx
