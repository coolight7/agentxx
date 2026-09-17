#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 中断 UI 描述 (schema) 单元测试:
/// - 描述块 JSON 往返 (text/markdown/diff/separator/gap/control/submit/custom)
/// - 预设模板 (preset::inputForm / permissionCard / confirmCard) 生成形状
/// - 结果契约与取值 helper (makeInterruptResult / interruptValue*)
/// - 纯文本降级渲染 (interruptUiPlainText)
TestResult testInterruptUi();

} // namespace test
} // namespace agentxx
