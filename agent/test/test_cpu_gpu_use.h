#pragma once

#include "agentxx/expand/get_cpu_gpu_use.h"
#include <iostream>

#include "asio/awaitable.hpp"
#include "test_framework.h"

#if XX_IS_WIN_D
#include <windows.h>
#endif

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_cpu_passed
#define XX_TEST_FAILED g_cpu_failed

namespace agentxx {
namespace test {

extern int g_cpu_passed;
extern int g_cpu_failed;

asio::awaitable<TestResult> run_cpu_gpu_use_tests();

} // namespace test
} // namespace agentxx