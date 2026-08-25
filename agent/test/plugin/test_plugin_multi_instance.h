// 多实例回归测试模块声明
#pragma once

#include "agentxx/agent/context.h"
#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"

namespace agentxx {
namespace test {

asio::awaitable<TestResult> run_plugin_multi_instance_tests();

} // namespace test
} // namespace agentxx
