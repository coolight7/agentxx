#pragma once

#include "agentxx/agent/context.h"
#include "test_framework.h"
#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

namespace agentxx {
namespace test {

/// git worktree 工具函数测试计数器 (头文件 extern 导出 + cpp 定义;
/// XX_TEST_* 宏映射在 cpp 内 #define, 避免宏经头文件泄漏到其他模块)
extern int g_wt_passed;
extern int g_wt_failed;

/// git worktree 工具函数测试 (真实 git 子进程; 无 git 环境自动跳过)
asio::awaitable<TestResult> run_worktree_tests();

} // namespace test
} // namespace agentxx
