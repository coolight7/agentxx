#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 事件中枢 (EventHub) 自身的状态机 / 等待 / 投递决策 / 预算 / 定时源
asio::awaitable<TestResult> run_event_hub_tests();

/// 事件工具 (agentxx_event) 的发布值与结束流 (emit / end)
asio::awaitable<TestResult> run_event_tools_tests();

/// 事件投递路径 (会话端点 + 轮次状态)
asio::awaitable<TestResult> run_event_delivery_tests();

/// 异步子代理 (agentxx_subagent async) 的结果事件与取消
asio::awaitable<TestResult> run_event_subagent_tests();

} // namespace test
} // namespace agentxx
