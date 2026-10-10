#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// MiddlewareContext graphData <-> Json / state 通道 序列化测试:
/// - anyToJson 与 jsonToValue 对同一批类型双向对称 (任一侧缺分支都会让值丢失)
/// - 动态段表 (xx_appendSystemMessage, map<string,string,less<>>) 经中断 checkpoint
///   快照往返后内容完整 (重启 resume 不丢段)
TestResult testGraphData();

} // namespace test
} // namespace agentxx
