#pragma once

#include <asio/awaitable.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "test_framework.h"

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_mcp_passed
#define XX_TEST_FAILED g_mcp_failed

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

extern int g_mcp_passed;
extern int g_mcp_failed;

asio::awaitable<TestResult> run_mcp_tests();

} // namespace test
} // namespace agentxx
