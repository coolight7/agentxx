#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 启动装配快照测试 (计划 ARC-6 / TOOL-12 / CFG-3 / PLG-10):
/// 配置侧快照 (模型/路径/权限/特性/插件声明, 不泄漏 API key)、
/// 运行侧快照 (模型注册表/中间件顺序/工具来源与被过滤原因/插件/执行图/持久化)、
/// 渲染文本与启动日志摘要
TestResult testAssemblySnapshotConfig();

asio::awaitable<TestResult> run_assembly_snapshot_tests();

} // namespace test
} // namespace agentxx
