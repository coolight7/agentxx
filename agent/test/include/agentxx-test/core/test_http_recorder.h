#pragma once

#include "agentxx-test/test_framework.h"
#include <asio/awaitable.hpp>
#include <neograph/api.h>

namespace agentxx {
namespace test {

/// HTTP 录制/回放夹具测试 (计划 LLM-13)
/// - 分帧/摘要: 请求摘要稳定性, 固定装置 JSON 与文件往返
/// - 端到端: 录制 (真实 provider → 本地录制器 → 本地 LLM 模拟器) → 回放 (provider → 回放器)
///   两次调用的结果一致; 固定装置里没有凭据取值
/// - 顺序与摘要校验: 请求与固定装置不符时回放器回 400 并记录不匹配原因;
///   固定装置用尽后再来请求同样被拒
/// - 脱敏: 头白名单 + 凭据只记存在标记; 正文按给定 secrets 与通用凭据形态屏蔽
asio::awaitable<TestResult> run_http_recorder_tests();

} // namespace test
} // namespace agentxx
