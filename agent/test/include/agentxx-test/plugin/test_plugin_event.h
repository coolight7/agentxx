#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 事件中枢的插件面 (接口表 `agentxx.agent.event` + SDK kit + 示例插件)
///
/// 覆盖: 插件声明事件源 / 发布值 / 结束流 / 移除记录落到同一份中枢记录表 (含归属)、
/// 插件替会话登记等待并按投递模式变成会话输入、模型侧 `watch` 能回放插件值、
/// 参数与冲突/预算/未启用等各条失败路径, 以及禁用/启用/卸载三个状态下的注册可逆性。
asio::awaitable<TestResult> run_plugin_event_tests();

} // namespace test
} // namespace agentxx
