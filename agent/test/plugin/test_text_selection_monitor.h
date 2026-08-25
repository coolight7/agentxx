#pragma once

#include "agentxx/agent/context.h"
#include "asio/awaitable.hpp"
#include <neograph/api.h>

#include "test_framework.h"

namespace agentxx {
namespace test {

/// 文本选择监控插件 (agentxx_text_selection_monitor) 集成测试:
/// - 加载插件 (从 expand 拆分后经 plugins/ 加载)
/// - 工具 agentxx_text_selection_monitor (start/stop/status)
asio::awaitable<TestResult>
    run_text_selection_monitor_tests(std::weak_ptr<agentxx::agent::AgentContext> agentContext);

} // namespace test
} // namespace agentxx
