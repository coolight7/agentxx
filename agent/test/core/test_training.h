#pragma once

#include "test_framework.h"

namespace agentxx {
namespace test {

/// 训练模块测试: 覆盖 training 纯逻辑部分
/// (markdown/JSON 解析, 测试用例加载与重名唯一化, prompt patch 规范化,
///  UTF-8 安全变异, 变体序列化往返, 去重与预去重, 取消轮询)
TestResult testTraining();

} // namespace test
} // namespace agentxx
