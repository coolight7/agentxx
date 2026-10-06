#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 提示词稳定段测试 (计划 PRM-1 / PRM-2 / PRM-7):
/// 段落 order 排序、稳定段与动态段分离、请求体结构断言、稳定段哈希断言
TestResult testPromptSectionOrder();

asio::awaitable<TestResult> run_prompt_stability_tests();

} // namespace test
} // namespace agentxx
