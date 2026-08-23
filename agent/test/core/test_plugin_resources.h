#pragma once

#include "agentxx/agent/context.h"
#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_res_passed
#define XX_TEST_FAILED g_res_failed

namespace agentxx {
namespace test {

extern int g_res_passed;
extern int g_res_failed;

/// 插件会话资源扩展测试模块 (plugin_api v8: Skill/Memory/MCP 声明式 + 运行时)
asio::awaitable<TestResult> run_plugin_resource_tests();

} // namespace test
} // namespace agentxx
