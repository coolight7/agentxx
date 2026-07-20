#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_http_passed
#define XX_TEST_FAILED g_http_failed

#undef XX_TEST_EXPECT_HAS_VALUE
#define XX_TEST_EXPECT_HAS_VALUE(expr)                                         \
  expect_has_value_impl(expr, __FILE__, __LINE__)

namespace agentxx {
namespace test {

extern int g_http_passed;
extern int g_http_failed;

template <typename T>
void expect_has_value_impl(T &&expr, const char *file, int line);

asio::awaitable<TestResult> run_http_client_tests();

} // namespace test
} // namespace agentxx