#pragma once

#include "agentxx/agent/context.h"
#include <asio/awaitable.hpp>
#include <string>

#include "test_framework.h"
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_sc_passed
#define XX_TEST_FAILED g_sc_failed

namespace agentxx {
namespace test {

extern int g_sc_passed;
extern int g_sc_failed;

/// ScreenCapture 插件集成测试 (agentxx_computer_use):
/// - 非 Windows 平台: 跳过 (screen_capture/ui_control 仅 Windows 实现)
/// - Windows: 加载插件, 验证 agentxx_screen_capture /
///   agentxx_ui_control_keyboard_mouse 工具注册与执行
asio::awaitable<agentxx::test::TestResult>
    run_screen_capture_tests(std::weak_ptr<agentxx::agent::AgentContext> agentContext);

} // namespace test
} // namespace agentxx
