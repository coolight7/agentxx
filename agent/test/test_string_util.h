#pragma once

#include "agentxx/util/string_util.h"
#include "test_framework.h"

#include <cassert>

using namespace agentxx::util;

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_su_passed
#define XX_TEST_FAILED g_su_failed

extern int g_su_passed;
extern int g_su_failed;

#define shiftCompareExtend(left, right, sub)                                   \
  XX_TEST_EXPECT_EQ(agentxx::util::compareExtend(left, right), sub);           \
  XX_TEST_EXPECT_EQ(agentxx::util::compareExtend(right, left), -(sub));

namespace agentxx {
namespace test {

TestResult testStringUtil();

} // namespace test
} // namespace agentxx
