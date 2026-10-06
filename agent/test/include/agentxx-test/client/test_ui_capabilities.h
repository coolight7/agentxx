#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 客户端界面能力段测试 (计划 UI-9):
/// 组件/控件能力来自唯一的 [tuiUiCapabilities], 体验级别字段 (表单提交方式 /
/// 尺寸形态 / 终端能力) 如实上报且不影响描述层解析
TestResult testUiCapabilities();

} // namespace test
} // namespace agentxx
