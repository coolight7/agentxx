#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 真实插件 `agentxx_chatbot` 全流程 (假平台 + 模拟模型)
///
/// 与 `chatbot_core` 的分工:
/// - 那边只测纯函数 (切分 / 命令解析 / 配置校验 / 路由表), 不碰网络也不碰主程序;
/// - 这里走**真实插件加载** (真实动态库 + 真实 `AgentHost` + 假平台 + 模拟模型),
///   验证插件按设计文档写出来的一条完整路径能跑通:
///
/// 1. `start` 里挂人设根 → 根上有 5 个 `chatbot_*` 工具 (`includeCreator = true` 生效);
/// 2. 假平台注入一条私聊 → 通道懒创建 → 模型收到输入 (含说话人前缀);
/// 3. 模型调 `chatbot_send_message` → 假平台收到出站 (带被动回复 id);
/// 4. `mode: mention`: 无 @ 只进缓冲, 被 @ 时一次投递带上全部;
/// 5. 同 `msg_id` 注入两次只处理一次 (`duplicated` 计数 +1);
/// 6. 长文本切分成多条 (分段标记齐全);
/// 7. `stop` → 通道关闭 → 根回收。
asio::awaitable<TestResult> run_plugin_example_chatbot_tests();

} // namespace test
} // namespace agentxx
