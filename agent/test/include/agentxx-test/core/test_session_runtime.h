#pragma once

#include "agentxx-test/test_framework.h"
#include "asio/awaitable.hpp"

namespace agentxx {
namespace test {

/// 统一启动壳与会话输入解析测试
///
/// 覆盖:
/// - `parseUserInputJson` 的 8 类边界 (空串/非 JSON/非对象/空内容/附件形态/
///   附件条数/附件大小/正常与多字段)
/// - `AgentSessionRuntime` 的装配与关闭 (缺参数拒绝、幂等 start、init 失败上报、
///   stop 后 running 归零)
/// - 已初始化 agent 上投递一条输入并走完一轮 (插件通道走的就是这条路)
::boost::asio::awaitable<TestResult> run_session_runtime_tests();

} // namespace test
} // namespace agentxx
