#pragma once

#include "test_framework.h"

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_mf_passed
#define XX_TEST_FAILED g_mf_failed

namespace agentxx {
namespace test {

extern int g_mf_passed;
extern int g_mf_failed;

TestResult testMiscFixes();

} // namespace test
} // namespace agentxx
