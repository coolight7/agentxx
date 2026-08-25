#pragma once

#include "test_framework.h"

namespace agentxx {
namespace test {

/// 并发/无锁相关组件测试:
/// - LogDispatcher 无锁 copy-on-write 热路径
/// - ModelProviderRegistry getProvider 双重检查锁 + 迭代器安全
/// - McpServer 注册表读写并发
/// - AsyncMutex 协程互斥 (持锁跨 co_await 不死锁)
TestResult testConcurrency();

} // namespace test
} // namespace agentxx
