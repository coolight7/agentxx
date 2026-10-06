#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 持久化语义分级与降级提示测试 (计划 STO-5 / STO-9):
/// 立即落盘 / 节流落盘的调用边界, 写失败只提示一次, 恢复后不再提示
TestResult testPersistSemantics();

} // namespace test
} // namespace agentxx
