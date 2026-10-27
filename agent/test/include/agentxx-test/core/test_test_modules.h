#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 测试模块清单生成物 (`docs/zh-cn/design/test-modules.md`) 的新鲜度校验
/// - 清单从 `agent/test/test.cpp` 的注册调用直接生成, 不再手工维护
/// - `AGENTXX_UPDATE_TEST_MODULES=1` 时重生生成物
TestResult testTestModules();

} // namespace test
} // namespace agentxx
