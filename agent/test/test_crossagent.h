#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/events.h"
#include "agentxx/middlewares/subagent_supervisor.h"
#include "agentxx/tools/cross_agent_query.h"
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
#define XX_TEST_PASSED g_ca_passed
#define XX_TEST_FAILED g_ca_failed

namespace agentxx {
namespace test {

extern int g_ca_passed;
extern int g_ca_failed;

asio::awaitable<TestResult> run_crossagent_tests();

} // namespace test
} // namespace agentxx
