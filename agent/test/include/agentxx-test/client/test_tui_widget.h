#pragma once
#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// TUI 交互框架测试 (命中登记表 / 声明式条目列表)
/// - [UiHitRegistry] 帧首清空 + 渲染期登记: 未渲染的控件不占点击区域
///   (回归验证"消失的按钮仍能点中"、"默认构造的 ftxui::Box 被当成命中区域" 等问题)
/// - [UiActionList] 条目表 (label/value/onActivate) + 统一键盘/鼠标交互
/// - 组件级: 输入栏 [📎︎︎] / 状态栏按钮隐藏后原位置不再可点
TestResult testTuiWidget();

} // namespace test
} // namespace agentxx
