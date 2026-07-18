#pragma once

#include "agentxx/server/acp_server.h"
#include "agentxx/tools/tool.h"
#include "agentxx/util/http_client.h"
#include <asio/awaitable.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <iostream>
#include <neograph/graph/engine.h>
#include <string>
#include <thread>

#include "test_framework.h"

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_acp_passed
#define XX_TEST_FAILED g_acp_failed

namespace agentxx {
namespace test {

extern int g_acp_passed;
extern int g_acp_failed;

asio::awaitable<TestResult> run_acp_tests();

} // namespace test
} // namespace agentxx
