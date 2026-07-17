#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/event_bridge.h"
#include "agentxx/events.h"
#include "agentxx/middlewares/event_stream.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/use_awaitable.hpp"
#include <atomic>
#include <iostream>
#include <memory>
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
