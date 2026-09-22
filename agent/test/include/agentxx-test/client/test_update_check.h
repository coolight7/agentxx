#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 启动更新检查 (版本标签解析/比较 + 请求失败路径)
/// - 走本地 HTTP 服务端到端验证的部分在 `http` 模块 (需要异步服务器)
TestResult testUpdateCheck();

} // namespace test
} // namespace agentxx
