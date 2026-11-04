#pragma once

#include "agentxx-test/test_framework.h"
#include "asio/awaitable.hpp"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 内置异步任务模块 (AsyncWork) 测试
///
/// 覆盖: 每队列 FIFO 与额度、队列之间互不影响、不限排队条数、
/// 台账 linger 清扫、`sessionId == asyncId`、取消 (排队/运行中)、
/// 按创建者批量取消、`kind=tool` 明确拒绝。
asio::awaitable<TestResult> run_async_work_tests();

} // namespace test
} // namespace agentxx
