#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 假 provider 用例 (计划 LLM-5 / TST-1):
/// 固定流、错误注入与重试、工具循环、取消、用量记账, 全程不依赖网络
asio::awaitable<TestResult> run_fake_provider_tests();

} // namespace test
} // namespace agentxx
