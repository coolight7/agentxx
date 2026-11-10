#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 插件网络能力 (`<应用名>.net` 表 + `utilxx::NetHub`)
///
/// 覆盖: 服务按属主隔离的现状查询、HTTP 服务端挂静态路由 + 客户端取回 (回环往返)、
/// 异步请求的完成通知、事件走广播 `plugin.<属主>.net`、属主卸载时未完成请求按取消结算
/// 且服务与端口一起释放。
asio::awaitable<TestResult> run_plugin_net_tests();

} // namespace test
} // namespace agentxx
