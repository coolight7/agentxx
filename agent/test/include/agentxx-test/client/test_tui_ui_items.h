#pragma once
#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 共享 UI 组件渲染层测试 ([agentxx-client/io/tui/ui_components.h])
/// - 各组件 kind 的屏幕渲染断言 (宽字符、表格对齐、容器嵌套、控件与提交行)
/// - 高度估算与真实布局一致 (测量 = 渲染同一实现, 避免滚动位置漂移)
/// - 元素内可命中区域 (表格单元格 / 折叠标题 / 控件与提交行) 的坐标与标识
/// - 滚动容器命中映射 ([Scrollable::hitTestItem]): 滚动后仍指向正确子项,
///   未渲染(视口外)的子项不参与命中
TestResult testTuiUiItems();

} // namespace test
} // namespace agentxx
