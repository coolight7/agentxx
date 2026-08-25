#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"

namespace agentxx {
namespace test {

/// SubAgentManagerTool (`agentxx_subagent` 工具) 单元测试
/// - 覆盖: 可用性 (注册/定义 schema) / 参数校验错误兼容 /
///   NodeInterrupt 中断流程与参数存储 / resume 结果提取 (单发纯文本、
///   批量 json 数组、resultId 缺失按序号兜底) / parseSubagentBatchFromInterrupt
///   批量与旧单发格式解析 / makeSubagentResumeKey + buildSubagentResumeValues
///   写入-读取 key 规则闭环
asio::awaitable<TestResult> run_subagent_tool_tests();

} // namespace test
} // namespace agentxx
