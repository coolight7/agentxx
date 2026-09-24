#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// markdown 块渲染测试 (代码块折行 / 块引用缩进 / 高度估算一致性)
TestResult testMarkdownBlock();

} // namespace test
} // namespace agentxx
