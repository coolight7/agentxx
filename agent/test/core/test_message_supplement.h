#pragma once

#include <asio/awaitable.hpp>

#include "test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 中断 / 取消(超时停止) 后 BaseAgent 自动补充消息的 E2E 测试:
/// - 中断: 触发中断的 tool 自动补充 [Interrupt] tool 消息 (role/tool_call_id/tool_name/flags)
/// - 取消/超时停止: 未完成的 tool 自动补充 [User canceled] tool 消息 (AutoInserted)
/// 验证补充后消息的角色顺序与内容正确 (每条 assistant tool_call 都有对应 tool 回复)
asio::awaitable<TestResult> run_message_supplement_tests();

} // namespace test
} // namespace agentxx

