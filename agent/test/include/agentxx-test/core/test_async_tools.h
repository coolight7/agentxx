#pragma once

#include "agentxx-test/test_framework.h"
#include "asio/awaitable.hpp"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 后台任务工具族 (`agentxx_async_start` / `_query` / `_await` / `_cancel`)
///
/// 覆盖: 开始 (工具/子代理两条路径、notify 默认与关闭、权限与参数校验)、
/// 查询 (六档 group、单条 id、归属隔离)、阻塞等待 (已完成/运行中/超时)、
/// 取消 (id / group / 终态拒绝 / 归属隔离)。
asio::awaitable<TestResult> run_async_tools_tests();

} // namespace test
} // namespace agentxx
