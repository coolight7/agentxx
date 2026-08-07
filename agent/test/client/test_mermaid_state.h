#pragma once

#include "test_framework.h"

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_mermaid_state_passed
#define XX_TEST_FAILED g_mermaid_state_failed

namespace agentxx {
namespace test {

extern int g_mermaid_state_passed;
extern int g_mermaid_state_failed;

TestResult testMermaidState();

} // namespace test
} // namespace agentxx
