#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 通知型功能埋点的派发 (广播频道 `agentxx.agent.point.*`)
///
/// 覆盖: 点清单 (id / 名称 / 顺序, 派发记录只在开发者模式下出现)、
/// 7 个点的派发与载荷字段、没有订阅方时的行为、主程序级单派发器的常驻
/// (插件禁用/卸载/轮末清理都不摘, 拆挂载才摘), 以及多挂载下的清单形状。
asio::awaitable<TestResult> run_plugin_dispatch_tests();

} // namespace test
} // namespace agentxx
