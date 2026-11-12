#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 功能埋点的插件面 (实现行 = 能力行; kit 的 `providePoint` / `pointAnswer`)
///
/// 覆盖: 插件声明功能埋点与登记实现行落到同一个能力表 (归属 / 作用域 / 层序 / 清单计数)、
/// 插件实现覆盖核心点与摘除后回落、按名调用的各条结果路径 (含超时与重入保护)、
/// `call` 的零副作用契约, 以及禁用/启用/卸载三个状态下的注册可逆性。
asio::awaitable<TestResult> run_plugin_feature_tests();

} // namespace test
} // namespace agentxx
