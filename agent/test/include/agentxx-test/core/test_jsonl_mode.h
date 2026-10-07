#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {
/// JSONL stdio 一次性运行模式测试 (计划 PRO-8)
/// - 分帧编解码: 合法行 / 空行 / 非法 JSON / 未知消息类型 / 超长行
/// - 传输与真实 agent 联调: 喂入 `hello` + `user_input` 行, 检查 stdout 侧只有协议行
/// - 轮次语义: 采集到 `input_ack` 时轮次尚未结束, `turn_result` 才是结束标志
/// - 一次性运行收尾: stdin EOF 后仍能写出结果行, 轮次跑完自动收尾返回
asio::awaitable<TestResult> run_jsonl_mode_tests();

} // namespace test
} // namespace agentxx
