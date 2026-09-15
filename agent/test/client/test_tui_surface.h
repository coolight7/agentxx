#pragma once
#include "test_framework.h"

namespace agentxx {
namespace test {

/// TUI 弹窗面性风格测试
/// - 外框: 四角圆角 (角格字符/前景/背景), 上下左右各 1 格内边距且为纯填充,
///   标题栏/内容区/底部提示栏之间各 1 行间距
/// - 区域背景色 (标题栏/内容区/底部提示栏) 与主题 surface* 配色一致, 弹窗内不出现边框/
///   分割线字符
/// - 标题与内容行直接贴外框左内边距 (行内不再自带首尾空格)
/// - 主题 token 齐备且各区域背景色可区分 (Dark/Light)
TestResult testTuiSurface();

} // namespace test
} // namespace agentxx
