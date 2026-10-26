#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 多挂载全流程 (插件 `example_multi_mount`)
///
/// 覆盖: 一份主程序 + 两个 agent 各挂一次、计数按挂载隔离 (`plugin.mount[].args`
/// 各自生效)、拆掉一个挂载后另一个继续可用且主程序未关闭、重新挂载计数归零、
/// 上一轮的引用 (旧挂载序号) 被拒绝。
asio::awaitable<TestResult> run_plugin_multi_mount_tests();

} // namespace test
} // namespace agentxx
