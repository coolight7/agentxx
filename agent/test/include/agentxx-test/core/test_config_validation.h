#pragma once

#include <string>

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 配置校验测试 (计划 CFG-1):
/// 结构化问题 (键路径 + 严重级别)、致命/警告分级、路径存在性与权限组合检查
TestResult testConfigValidation();

} // namespace test
} // namespace agentxx
