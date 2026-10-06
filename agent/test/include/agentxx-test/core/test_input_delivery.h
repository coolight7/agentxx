#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 输入投递测试 (计划 LOOP-1/2/3/4/11):
/// 队列状态机、受理回执、next-step/inject 注入、持久化收件箱恢复、collect 合并
asio::awaitable<TestResult> run_input_delivery_tests();

} // namespace test
} // namespace agentxx
