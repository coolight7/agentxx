#pragma once

#include "test_framework.h"

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_tui_input_passed
#define XX_TEST_FAILED g_tui_input_failed

namespace agentxx {
namespace test {

extern int g_tui_input_passed;
extern int g_tui_input_failed;

TestResult testTuiInput();

} // namespace test
} // namespace agentxx
