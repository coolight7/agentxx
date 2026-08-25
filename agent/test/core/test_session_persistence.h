#pragma once

#include "agentxx/agent/context.h" // asio 命名空间别名定义来源
#include "test_framework.h"
#include <asio/awaitable.hpp>
#include <string>

namespace agentxx {
namespace test {

/// 会话 SQLite 持久化测试: viewMessages/LLM 上下文/share store 的
/// 落库与重启恢复、id 延续、链式哈希一致性、sessionId 清洗,
/// 以及真实 BaseAgent 端到端持久化
asio::awaitable<TestResult> run_session_persistence_tests();

} // namespace test
} // namespace agentxx

