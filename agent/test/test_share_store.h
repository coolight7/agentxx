#pragma once

#include "agentxx/agent/context.h"
#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_ss_passed
#define XX_TEST_FAILED g_ss_failed

namespace agentxx {
namespace test {

extern int g_ss_passed;
extern int g_ss_failed;

asio::awaitable<TestResult> run_share_store_tests();

} // namespace test
} // namespace agentxx
