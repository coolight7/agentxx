#pragma once

#include "test_framework.h"

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_thread_id_passed
#define XX_TEST_FAILED g_thread_id_failed

namespace agentxx {
namespace test {

extern int g_thread_id_passed;
extern int g_thread_id_failed;

TestResult testSessionId();

} // namespace test
} // namespace agentxx
