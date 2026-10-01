#pragma once

#include "agentxx-test/test_framework.h"
#include "utilxx_base/asio_error.h"
#include <asio/awaitable.hpp>

namespace agentxx {
namespace test {

/// 用量账本端到端 (计划 STO-8): 真实轮次经 modelcall 记录用量
/// - 成功轮次: 账本记录 prompt/completion/total 用量与模型名
/// - 失败轮次 (API 持续 500 且不重试): 账本记录失败行与原因, 不丢统计
/// 需要本机 LLM 模拟器 (test_agent 提供的 DaSimServer), 不依赖真实网络
asio::awaitable<TestResult> test_usage_ledger();

} // namespace test
} // namespace agentxx
