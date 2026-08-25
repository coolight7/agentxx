#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"

namespace agentxx {
namespace test {

asio::awaitable<TestResult> run_agent_host_tests();

/// 验证: 同上下文模式派生子代理 (messages 结构化透传 + sessionId 指定)
/// - messages 原样透传为子代理初始上下文 (含 system, 不做文本转录)
/// - 子代理运行在指定 thread, 强制使用父会话当前模型 (忽略 subagentModel)
/// - 三者共同保证"相同上下文前缀 + 相同 threadid + 相同模型"命中 KV cache
asio::awaitable<void> test_host_spawn_same_context();

/// 验证: 子代理工具策略 (tools 参数)
/// - [] = 无工具; ["agentxx_share_store"] = 自定义白名单;
///   ["*"] = 全量继承父 agent 工具; 缺省 = 子代理默认全量
asio::awaitable<void> test_host_spawn_tool_policy();

/// 验证: 子代理上下文压缩 (summarization) 开关
/// - enableSummarization=false → init 后无 summarization 中间件
/// - 缺省/true → 存在 (summarization 发起的压缩子代理必须显式 false)
asio::awaitable<void> test_subagent_summarization_switch();

} // namespace test
} // namespace agentxx
