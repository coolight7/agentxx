#pragma once

#include "agentxx/agent/context.h"
#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_wt_passed
#define XX_TEST_FAILED g_wt_failed

namespace agentxx {
namespace test {

extern int g_wt_passed;
extern int g_wt_failed;

/// git worktree 工具函数测试 (真实 git 子进程; 无 git 环境自动跳过)
asio::awaitable<TestResult> run_worktree_tests();

} // namespace test
} // namespace agentxx
