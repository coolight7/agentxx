#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// `chatbot_core` —— chatbot 插件的纯逻辑 (切分 / 命令解析 / 输入块渲染 / 去重环 /
/// 引用编号 / 配置校验)
///
/// 直接 include 插件头 (`agent/plugins/` 已是测试 include 根), 不起 agent、不碰网络。
TestResult testChatbotCore();

} // namespace test
} // namespace agentxx
