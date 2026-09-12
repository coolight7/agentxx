#pragma once

#include "test_framework.h"

namespace agentxx {
namespace test {

/// 测试 ToolcallWrapNode::autoFixArgsType 参数类型自动修正
/// (类型互转 string<->number/bool/数组 + 枚举字符串值大小写规范化)
TestResult testToolcallArgs();

} // namespace test
} // namespace agentxx
