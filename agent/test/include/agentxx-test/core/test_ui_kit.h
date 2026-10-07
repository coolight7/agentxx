#pragma once
#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// agentxx 界面扩展 kit 与能力适配的用法测试 (agentxx/plugin/api/agentxx_ui_kit.g.h)
/// - 扩展 kit 组件的装配结果 (节点种类、留白规则、可选参数缺省时的省略)
/// - 带 env (客户端能力) 时的变体选择 (支持 / 不支持 Diff、Diagram 两条路)
/// - 同一份描述在完整能力 / 终端常用集 / 最小能力下的收敛 (adapt 结果只含声明支持的组件)
/// - kit 产出 → dump → parse 的往返 (字段不丢)
TestResult testUiKit();

} // namespace test
} // namespace agentxx
