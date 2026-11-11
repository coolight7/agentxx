#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 插件广播面 (通用表 `agentxx.broadcasts` + SDK kit + 示例插件 `example_event`)
///
/// 覆盖: 声明 (短名补全 / 应用频道不能被插件改写 / 幂等)、发布 (属主与 `anyone` 的
/// 权限差别)、订阅 (载荷原样、`pushOnSubscribe` 补当前值、退订后不再收)、读当前值
/// (有值 / 无缓存 / 未声明三种返回码)、结束流 (发布与读被拒、`on_end` 恰好一次、
/// 再次声明开新流), 以及禁用/卸载后频道与订阅回到基线。
asio::awaitable<TestResult> run_plugin_broadcast_tests();

} // namespace test
} // namespace agentxx
