#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 示例插件 `example_runtime` 全流程 (插件内驱动 agent 会话)
///
/// 与 `plugin_agent_runtime` 的分工: 那边用手工构造的伪实例直接打 C ABI 表, 验证
/// 主程序侧语义; 这里走**真实插件加载** (真实动态库 + 真实 create/start/mount),
/// 验证插件作者按 SDK 写出来的一条完整路径能跑通:
///
/// 1. `start` 里挂自己的根 (自定义提示词 + 工具白名单) → 容器里出现该根;
/// 2. 在根上开会话通道 → 收到 `READY`;
/// 3. 插件在 agent 上注册的三个工具可用 (`say` / `ping` / `status`);
/// 4. `say` 投一条输入 → 跑一轮 → 通道收了正文与最终回答;
/// 5. `ping` 连发三条 → COALESCE 触发器合并成一轮;
/// 6. 卸载插件 → 通道关闭、根被回收, 运行时清单清零。
asio::awaitable<TestResult> run_plugin_example_runtime_tests();

} // namespace test
} // namespace agentxx
