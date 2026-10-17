#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 压缩请求的前缀对齐与模拟缓存命中测试
///
/// 覆盖:
/// - 压缩请求的 system 文本与父会话上一次请求逐字节相同 (保留调用方给的 system)
/// - 压缩请求的工具定义与父会话上一次请求逐字节相同 (继承上一次请求的工具)
/// - 压缩请求的消息是父会话消息前缀 + 末尾压缩指令
/// - 模拟前缀缓存报告 cached/prompt >= 0.9 (长历史)
/// - 不执行工具: 模型返回 tool_calls 时忽略, 不会进入 tools 节点
asio::awaitable<TestResult> run_prompt_cache_prefix_tests();

} // namespace test
} // namespace agentxx
