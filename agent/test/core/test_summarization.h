#pragma once

#include "agentxx/agent/context.h"
#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"

namespace agentxx {
namespace test {

/// 上下文压缩中间件 (SummarizationMiddlewareHandle) 单元测试
/// - 覆盖: token 估算 / 文本转换 / 噪音清理 / toolcall 去重与探索折叠 /
///   token 预算切分 / 硬截断兜底 / 同上下文 LLM 压缩 (FakeProvider) /
///   onModelcallRunFunc 压缩流程与上下文统计 / 压缩后仍超限降级硬截断
/// - 完整语义: system 消息不可变 / 确定性压缩先行 / 同上下文 LLM 压缩为一段总结
///   (thinking 保留, 压缩 subagent 不传任何工具, 原样压缩) /
///   消息角色顺序 (system|user 自动提示|assistant 总结 + 未压缩最近消息)
asio::awaitable<TestResult> run_summarization_tests();

} // namespace test
} // namespace agentxx
