#pragma once

#include "agentxx/agent/context.h"
#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_sum_passed
#define XX_TEST_FAILED g_sum_failed

namespace agentxx {
namespace test {

extern int g_sum_passed;
extern int g_sum_failed;

/// 上下文压缩中间件 (SummarizationMiddlewareHandle) 单元测试
/// - 覆盖: token 估算 / 文本转换 / 长内容暂存 share_store /
///   toolcall 去重截断 / onModelcallRunFunc 两级压缩流程与上下文统计
asio::awaitable<TestResult> run_summarization_tests();

} // namespace test
} // namespace agentxx
