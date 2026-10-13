#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 可观测性 (关键指标 / 诊断包导出)
/// - KeyMetrics 计数与计时语义
/// - 诊断包内容边界 (不含凭据/默认不含消息正文) 与日志尾部脱敏
TestResult testObservability();

} // namespace test
} // namespace agentxx
