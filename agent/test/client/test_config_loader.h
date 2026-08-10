#pragma once

#include "test_framework.h"

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_config_loader_passed
#define XX_TEST_FAILED g_config_loader_failed

namespace agentxx {
namespace test {

extern int g_config_loader_passed;
extern int g_config_loader_failed;

TestResult testConfigLoader();

} // namespace test
} // namespace agentxx
