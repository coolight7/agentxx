#pragma once
#include "test_framework.h"

namespace agentxx {
namespace test {

/// TUI 弹窗  面性风格测试
/// - 区域背景色 (标题栏/内容区/底部提示栏) 与主题 surface* 配色一致
/// - 弹窗内不出现边框/分割线字符
/// - 主题 token 齐备且各区域背景色可区分 (Dark/Light)
TestResult testTuiSurface();

} // namespace test
} // namespace agentxx
