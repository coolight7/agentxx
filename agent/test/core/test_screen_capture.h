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

/// ScreenCapture 插件集成测试 (agentxx_screen_capture):
/// - 非 Windows 平台: 跳过 (screen_capture 仅 Windows 实现)
/// - Windows: 加载 agentxx_screen_capture 插件, 验证 agentxx_screen_capture
///   工具注册与执行; 并验证 agentxx_computer_use 依赖它 (depends 声明)
asio::awaitable<agentxx::test::TestResult>
    run_screen_capture_tests(std::weak_ptr<agentxx::agent::AgentContext> agentContext);

} // namespace test
} // namespace agentxx
