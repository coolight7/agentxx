#pragma once

#include "agentxx/agent/context.h"
#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"

namespace agentxx {
namespace test {

/// 插件会话资源扩展测试模块 (plugin_api v8: Skill/Memory/MCP 声明式 + 运行时)
asio::awaitable<TestResult> run_plugin_resource_tests();

} // namespace test
} // namespace agentxx
