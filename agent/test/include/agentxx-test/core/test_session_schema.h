#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 会话库 schema 版本与迁移 (agentxx::agent::SessionStore) 测试:
/// 新库建表并写版本、老库补齐列/表并备份、重复打开幂等、
/// 高版本库拒绝读写、用量账本存取、写租约与写路径集成、
/// 会话标题 (自动/改名) 与关键词检索
TestResult testSessionSchema();

} // namespace test
} // namespace agentxx
