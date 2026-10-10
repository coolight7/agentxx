#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 功能点的插件面 (接口表 `agentxx.agent.feature` + SDK kit)
///
/// 覆盖: 插件声明点与登记实现落到同一份注册表 (归属 / 层序 / 清单计数)、
/// 插件实现覆盖核心点与摘除后回落、按名调用的各条结果路径 (含超时与重入保护)、
/// `call` 的零副作用契约, 以及禁用/启用/卸载三个状态下的注册可逆性。
asio::awaitable<TestResult> run_plugin_feature_tests();

} // namespace test
} // namespace agentxx
