#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 并发与竞态清单 (计划 TST-4)
///
/// 覆盖"取消 vs 工具结算/中断应答/持久化节流"与"并行结果乱序提交"、
/// "工具执行中被注销"这几类时序竞态的不变量。
asio::awaitable<TestResult> run_race_guard_tests();

} // namespace test
} // namespace agentxx
