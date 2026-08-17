#pragma once

#include "agentxx/agent/context.h"
#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_sum_passed
#define XX_TEST_FAILED g_sum_failed

namespace agentxx {
namespace test {

extern int g_sum_passed;
extern int g_sum_failed;

/// 上下文压缩中间件 (SummarizationMiddlewareHandle) 单元测试
/// - 覆盖: token 估算 / 文本转换 / 噪音清理 / toolcall 去重与探索折叠 /
///   token 预算切分 / 硬截断兜底 / 同上下文 LLM 压缩 (FakeProvider) /
///   onModelcallRunFunc 两级压缩流程与上下文统计
/// - 完整语义: system 消息不可变 / 确定性压缩先行 / 同上下文 LLM 压缩为一段总结
///   (thinking 保留, 长内容由模型经 agentxx_share_store 自主外置) /
///   消息角色顺序 (system|user 自动提示|assistant 总结 + 未压缩最近消息)
asio::awaitable<TestResult> run_summarization_tests();

} // namespace test
} // namespace agentxx
