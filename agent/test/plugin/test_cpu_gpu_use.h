#pragma once

#include "agentxx/agent/context.h"
#include "asio/awaitable.hpp"
#include <neograph/api.h>

#include "test_framework.h"

namespace agentxx {
namespace test {

/// 系统资源监控插件 (agentxx_system_monitor) 集成测试:
/// - 加载插件 (从 expand 拆分后经 plugins/ 加载)
/// - 工具 agentxx_get_system_core_info (原内置工具迁移)
/// - 能力 agentxx.system_usage (agent 侧周期采集 publish usage 事件的数据源)
asio::awaitable<TestResult>
    run_cpu_gpu_use_tests(std::weak_ptr<agentxx::agent::AgentContext> agentContext);

} // namespace test
} // namespace agentxx
