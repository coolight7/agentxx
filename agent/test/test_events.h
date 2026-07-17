#pragma once

#include "agentxx/events.h"
#include "test_framework.h"

#include <cassert>

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_ev_passed
#define XX_TEST_FAILED g_ev_failed

namespace agentxx {
namespace test {

extern int g_ev_passed;
extern int g_ev_failed;

TestResult test_events();

} // namespace test
} // namespace agentxx
