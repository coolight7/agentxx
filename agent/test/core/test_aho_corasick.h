#pragma once

#include "test_framework.h"

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_ac_passed
#define XX_TEST_FAILED g_ac_failed

namespace agentxx {
namespace test {

extern int g_ac_passed;
extern int g_ac_failed;

TestResult testAhoCorasick();

} // namespace test
} // namespace agentxx
