#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// `chatbot_qq` —— QQ 网关事件归一化 (`platform/qq/qq_normalize.cpp`)
///
/// 用**真实抓到的事件 JSON** 做回归: 平台对不同附件 (图片/语音/文件/引用) 的字段
/// 布局并不一致, 只按文档猜会整类漏掉。这里留存的用例来自真机联调日志
/// (见 `resource/history/chatbot/work.md`)。
TestResult testChatbotQq();

} // namespace test
} // namespace agentxx
