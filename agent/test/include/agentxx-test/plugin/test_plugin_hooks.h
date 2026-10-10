#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 钩子处理器清单与优先级 (接口表 `agentxx.agent.hooks` / `agentxx.agent.hooks_ex`)
///
/// 覆盖: 注册表顺序 (层 -> 优先级 -> 登记序号; 不声明优先级时 = 登记顺序)、
/// 同一实例同一个点多个处理器与按句柄撤销、优先级越界裁剪、基础登记与扩展登记
/// 的互相独立、宿主级单派发器的挂载与摘除、实际派发的执行顺序 (含 `core` 层)、
/// 禁用/启用/卸载三态的可逆性, 以及开发者模式对派发记录的开关作用。
asio::awaitable<TestResult> run_plugin_hooks_tests();

} // namespace test
} // namespace agentxx
