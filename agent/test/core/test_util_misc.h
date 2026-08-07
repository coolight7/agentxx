#pragma once

#include "test_framework.h"

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_um_passed
#define XX_TEST_FAILED g_um_failed

namespace agentxx {
namespace test {

extern int g_um_passed;
extern int g_um_failed;

TestResult testUtilMisc();

} // namespace test
} // namespace agentxx
