#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "test_framework.h"
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_cs_passed
#define XX_TEST_FAILED g_cs_failed

namespace agentxx {
namespace test {

extern int g_cs_passed;
extern int g_cs_failed;

/// InMemorySingleCheckpointStore 测试:
/// - 每个 thread 仅保留最新 checkpoint
/// - save 淘汰历史 checkpoint 时同步清理挂载其上的 pending writes
/// - 最新 checkpoint 的 pending writes 不受影响
/// - async 接口 (engine 实际调用路径) 桥接正确
asio::awaitable<TestResult> run_checkpoint_store_tests();

} // namespace test
} // namespace agentxx
