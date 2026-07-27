#pragma once

#include "test_framework.h"

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_lockless_passed
#define XX_TEST_FAILED g_lockless_failed

extern int g_lockless_passed;
extern int g_lockless_failed;

namespace agentxx {
namespace test {

TestResult testLockless();

} // namespace test
} // namespace agentxx
