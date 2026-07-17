#pragma once

#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/use_awaitable.hpp"
#include "asio/as_tuple.hpp"

#include "agentxx/agent/context.h"
#include "agentxx/agent/interrupt_handler.h"
#include "agentxx/agent/permission_prompter.h"
#include "agentxx/events.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/middleware.h"
#include <iostream>
#include <memory>

#include "test_framework.h"
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_ib_passed
#define XX_TEST_FAILED g_ib_failed

namespace agentxx {
namespace test {

extern int g_ib_passed;
extern int g_ib_failed;

asio::awaitable<TestResult> run_interrupt_bus_tests();

} // namespace test
} // namespace agentxx
