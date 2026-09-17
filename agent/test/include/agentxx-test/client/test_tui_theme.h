#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// TUI 主题 (TUITheme) 弱化文字装饰器测试
/// - 深色主题: 沿用终端 `dim` 属性 (SGR 2), 单元格 dim 标记置位、前景色不变
/// - 浅色主题: 不使用 SGR 2 (终端按"前景色亮度减半"实现, 浅色背景上文字反而更深),
///   改为把前景色向背景色混合, 断言混合后的具体颜色 (变淡)
/// - 单元格自带背景色时向其自身背景色混合 (而非主题背景色)
/// - 终端不报告颜色支持时退回终端 `dim` 属性
TestResult testTuiTheme();

} // namespace test
} // namespace agentxx
