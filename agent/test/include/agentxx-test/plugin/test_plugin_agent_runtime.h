#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 会话运行时接口表 (`agentxx.agent.runtime`) 全流程
///
/// 覆盖: 表头与版本门槛 / 配置派生失败 / 挂根 → 开通道 → READY / 自定义提示词与
/// 工具白名单只影响该根 / 输入与轮次事件 / 同 sessionId 冲突 / 两个根互不串话 /
/// 根卸载与通道关闭 / 禁用回收 / 根数预算 / 触发器三种策略与定时器 / 清单查询。
///
/// 用的是真实容器 (`AgentHost`) + 真实 IO 线程 + 真实 `AgentSessionRuntime`,
/// 只把"插件实例"换成手工构造的伪实例 (与 `plugin_runtime` 同一手法): 运行时表
/// 只要求交给自己一个有效的主程序视图, 不需要动态库。
asio::awaitable<TestResult> run_plugin_agent_runtime_tests();

} // namespace test
} // namespace agentxx
