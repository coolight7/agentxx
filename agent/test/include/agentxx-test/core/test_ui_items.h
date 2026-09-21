#pragma once
#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 客户端 UI 组件描述 schema 单元测试 ([agentxx/ui/item.h])
/// - 解析与序列化往返 (含 canvas 原样保留、未知 kind 标记)
/// - 各 kind 的字段归一化 (progress → meter、按钮三种历史写法、表格单元格两种写法)
/// - 解析上限 (嵌套深度/元素数/文本长度) 与非法输入降级
/// - 纯文本降级 (文本/表格对齐/树/键值/趋势图/计量/控件/容器)
/// - 显示列宽工具 (宽字符/组合字符/截断/补齐)
TestResult testUiItems();

} // namespace test
} // namespace agentxx
