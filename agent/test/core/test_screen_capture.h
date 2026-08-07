#pragma once

#include <string>

#include "test_framework.h"
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_sc_passed
#define XX_TEST_FAILED g_sc_failed

namespace agentxx {
namespace test {

agentxx::test::TestResult test_screen_capture();

}
} // namespace agentxx