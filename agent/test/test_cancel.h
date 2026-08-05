#pragma once

#include <asio/awaitable.hpp>

#include "test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

extern int g_cancel_passed;
extern int g_cancel_failed;

asio::awaitable<TestResult> run_cancel_tests();

} // namespace test
} // namespace agentxx

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_cancel_passed
#define XX_TEST_FAILED g_cancel_failed
