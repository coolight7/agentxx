#pragma once
#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 表单交互专项测试 (控件状态机 + 各接入点的结果回传契约)
///
/// 覆盖:
/// - 控件初始化/点击/键盘/校验/取值 (与中断、插件面板、overlay 共用同一实现)
/// - 面板接入点: 控件点击 → 动作通道 (`__submit` / `__cancel` / `commitOnPick`)
/// - overlay 接入点: 自定义弹窗内的表单提交同样经动作通道
/// - 中断接入点: 提交/取消经中断结果通道 (见 `tui_interrupt`)
///
/// 本模块只放"表单"相关用例; 组件本身的渲染/测量在 `tui_ui_items`,
/// 面板/Info 的接入点渲染在 `tui_widget`。
TestResult testTuiForm();

} // namespace test
} // namespace agentxx
