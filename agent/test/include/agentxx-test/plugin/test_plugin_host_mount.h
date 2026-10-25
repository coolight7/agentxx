#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 一主程序多 agent 挂载 (阶段 F 的挂载矩阵)
///
/// 覆盖: 插件只加载一次 / 多个 agent 各自挂载、工具隔离、提示词只影响本 agent、
/// 钩子只在本 agent 派发、事件订阅按挂载分开并在拆挂载时撤销、重复挂载幂等、
/// 工具名冲突只影响本 agent、第三个 agent、挂载失败只回滚该 agent、
/// 无引用/未知 id/过期序号的注册被拒 (且无副作用)、版本门槛、
/// 21 张接口表可查、挂载表 JSON, 以及拆挂载与主程序关闭的收尾。
asio::awaitable<TestResult> run_plugin_host_mount_tests();

} // namespace test
} // namespace agentxx
