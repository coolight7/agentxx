#pragma once

#include "test_framework.h"

#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_training_passed
#define XX_TEST_FAILED g_training_failed

extern int g_training_passed;
extern int g_training_failed;

namespace agentxx {
namespace test {

/// 训练模块测试: 覆盖 training 纯逻辑部分
/// (markdown/JSON 解析, 测试用例加载与重名唯一化, prompt patch 规范化,
///  UTF-8 安全变异, 变体序列化往返, 去重与预去重, 取消轮询)
TestResult testTraining();

} // namespace test
} // namespace agentxx
