#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 按模块日志级别测试 (计划 OBS-5)
/// - `logModuleOf`: 由 `__FILE__` 取模块名 (目录分隔符 / 扩展名处理)
/// - `applyLogModuleLevelSpec`: `前缀=级别,...` 解析 (空白/大小写/非法段)
/// - 过滤行为 (经日志捕获 sink 端到端验证): 未命中不过滤、命中即按级别丢弃、
///   最长前缀优先、`clearModuleLevels()` 恢复、`Out` 恒通过
TestResult testLogModuleLevels();

} // namespace test
} // namespace agentxx
