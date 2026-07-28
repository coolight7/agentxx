#pragma once

#include "test_framework.h"

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_sc_passed
#define XX_TEST_FAILED g_sc_failed

namespace agentxx {
namespace test {

extern int g_sc_passed;
extern int g_sc_failed;

/// Session 跨线程只读快照测试:
/// - writer 线程绑定 io 线程后持续 appendHistory
/// - 多个 reader 线程并发调用 getFullHistoryCopy / getHashInfo / activity.load
/// - 验证快照一致性 (size 单调递增、内容连续完整)
TestResult testSessionConcurrentAccess();

} // namespace test
} // namespace agentxx
