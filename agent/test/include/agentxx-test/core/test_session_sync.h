#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 会话同步测试 (计划 STO-4):
/// 断线重连增量补拉 (hello.afterViewSeq)、序号范围校验与全量/尾窗回退
asio::awaitable<TestResult> run_session_sync_tests();

} // namespace test
} // namespace agentxx
