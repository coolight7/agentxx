#pragma once

#include "agentxx/agent/context.h"
#include <asio/awaitable.hpp>
#include <string>

#include "test_framework.h"

namespace agentxx {
namespace test {

/// ScreenCapture 插件集成测试 (agentxx_screen_capture):
/// - 非 Windows 平台: 跳过 (screen_capture 仅 Windows 实现)
/// - Windows: 加载 agentxx_screen_capture 插件, 验证 agentxx_screen_capture
///   工具注册与执行; 并验证 agentxx_computer_use 依赖它 (depends 声明)
asio::awaitable<agentxx::test::TestResult>
    run_screen_capture_tests(std::weak_ptr<agentxx::agent::AgentContext> agentContext);

} // namespace test
} // namespace agentxx
