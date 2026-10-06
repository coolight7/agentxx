#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 后台任务登记表 (TaskScope) 的独立语义测试 (同步)
TestResult testTaskScopeSemantics();

/// 分阶段关闭测试 (计划 ARC-5, 异步): 停止受理输入 / 后台任务取消收敛 /
/// 插件关闭 / 会话刷盘, 且不等待当前轮次结束
asio::awaitable<TestResult> run_shutdown_stage_tests();

} // namespace test
} // namespace agentxx
