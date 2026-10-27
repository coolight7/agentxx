#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 把本程序当 MCP stdio 服务器子进程运行 (阻塞到 stdin 关闭)
/// - 入口在 `test.cpp` 的 `--mcp-stdio-server` 参数
void runMcpStdioServerProcess();

asio::awaitable<TestResult> run_mcp_tests();

} // namespace test
} // namespace agentxx
