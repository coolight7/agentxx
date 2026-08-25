#pragma once

#include "agentxx/agent/context.h"
#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"

namespace agentxx {
namespace test {

/// CodeGraph 插件集成测试:
/// - 加载 agentxx_codegraph 插件 (位于 {cwd}/plugins/agentxx_codegraph)
/// - 经 toolRegistry 执行 8 个 codegraph 工具 (索引/搜索/上下文/路径等)
asio::awaitable<TestResult>
    run_codegraph_tools_tests(std::weak_ptr<agentxx::agent::AgentContext> agentContext);

} // namespace test
} // namespace agentxx
