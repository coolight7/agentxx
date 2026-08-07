#pragma once

#include "agentxx/util/diff_util.h"
#include "test_framework.h"

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_diff_passed
#define XX_TEST_FAILED g_diff_failed

extern int g_diff_passed;
extern int g_diff_failed;

namespace agentxx {
namespace test {

TestResult testDiffUtil();

} // namespace test
} // namespace agentxx
