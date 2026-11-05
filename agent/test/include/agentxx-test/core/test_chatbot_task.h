#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// `chatbot_task` —— chatbot 后台任务的业务层纯逻辑
///
/// 覆盖 `ChatbotTaskManager` 的提交/限额/事件状态机/取消/文本渲染, 以及面板
/// 「后台任务」段的纹理与快照往返。不碰网络、不碰主程序 (执行与额度在主程序那边,
/// 全流程在 `plugin_example_chatbot` 里测)。
TestResult testChatbotTask();

} // namespace test
} // namespace agentxx
