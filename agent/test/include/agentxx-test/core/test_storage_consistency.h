#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 存储一致性契约 (计划 TST-6)
///
/// 用同一份"存一条/读回来/覆盖/重开/写失败"语义断言跑多个存储后端:
/// settings_db(全局设置库)、SessionStore(会话库 store 表)、
/// share_store 内存替身、share_store 缓存+回库两条路径。
TestResult testStorageConsistency();

} // namespace test
} // namespace agentxx
