#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 单次 provider 调用的取消域用例 (计划 LLM-7): 级联、隔离与 RAII 收尾语义
TestResult testProviderCallScope();

} // namespace test
} // namespace agentxx
