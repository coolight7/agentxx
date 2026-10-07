#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// JSONL stdio 一次性运行模式 (client 侧运行器) 测试 (计划 PRO-8)
/// - 输入全部来自内存行缓冲, 输出同样写到内存 (不碰真实 stdin/stdout)
/// - 断言一次性运行收尾: 输入结束后仍写出轮次结果, 轮次跑完自动收尾并返回
/// - 断言 stdout 里只有协议行, 且 `turn_result` 一定排在受理回执之后
TestResult testJsonlRunner();

} // namespace test
} // namespace agentxx
