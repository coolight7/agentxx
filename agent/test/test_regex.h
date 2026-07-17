#pragma once

#include "agentxx/util/regex.h"
#include "test_framework.h"

#include <cassert>
#include <memory>
#include <string>
#include <vector>

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_regex_passed
#define XX_TEST_FAILED g_regex_failed

namespace agentxx {
namespace test {

using namespace agentxx::util;

extern int g_regex_passed;
extern int g_regex_failed;

TestResult testRegex();

} // namespace test
} // namespace agentxx
