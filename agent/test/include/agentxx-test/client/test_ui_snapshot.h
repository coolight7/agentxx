#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// UI 快照测试 (计划 UI-2 / TST-5):
/// 固定尺寸纯文本画面 + 命中区清单与仓库基线比较 (一键更新见
/// AGENTXX_UPDATE_UI_SNAPSHOTS=1), 覆盖表格/树/差异/Markdown/表单/窄终端/
/// CJK/超长内容/未知组件降级
TestResult testUiSnapshots();

} // namespace test
} // namespace agentxx
