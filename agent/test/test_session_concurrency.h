#pragma once

#include "test_framework.h"

namespace agentxx {
namespace test {

/// Session 只读访问测试
/// - 测试多个线程并发读取 Session 状态的线程安全性
TestResult testSessionConcurrentAccess();

} // namespace test
} // namespace agentxx
