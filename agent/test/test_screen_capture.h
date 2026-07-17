#pragma once

#include "agentxx/expand/screen_capture.h"
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "test_framework.h"

#if XX_IS_WIN_D
#include <windows.h>
#endif

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_sc_passed
#define XX_TEST_FAILED g_sc_failed

agentxx::test::TestResult test_screen_capture();