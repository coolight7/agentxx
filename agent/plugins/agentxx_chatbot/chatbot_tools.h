#pragma once

/// `chatbot_*` 工具的**纯逻辑实现** (5 个动作)
///
/// 工具是**回复的唯一出口** (见 `resource/history/chatbot/plan.md` §8.1):
/// agent 的正文输出默认不自动发到群里, 发消息必须显式调 `chatbot_send_message`。
///
/// 本头不含任何 SDK 代码: 每个函数都假定「已经在插件 io 线程上」执行 —— 插件入口把
/// 工作体投递到插件 io 线程再取结果 (工具本身跑在主程序工作线程池, 而调度状态只属于
/// 插件 io 线程, 不能跨线程直接读)。测试可以直接调这些函数验证行为。
#include "chatbot_engine.h"
#include "chatbot_outbound.h"
#include "chatbot_router.h"
#include "platform/platform.h"

#include <functional>
#include <string>
#include <string_view>

namespace agentxx {
namespace chatbot {

/// 工具依赖的运行期对象 (全部由插件入口持有, 生命周期覆盖工具)
struct ChatbotToolDeps {
    ChatbotEngine*   engine   = nullptr;
    ChatbotOutbound* outbound = nullptr;
    ChatbotRouter*   router   = nullptr;
    /// 平台对象查询 (按账号 id 取; 出站发送要靠它)
    std::function<IPlatform*(const std::string& accountId)> platformOf;
};

/// 定位会话键: 先按 sessionId (= threadId) 反查, 再尝试直接解析 `account|type|chatId`
///
/// - `explicitChat`: 工具参数里的 `chat` (非空时优先用它; 内置 root 调试场景必填)
std::string resolveChat(
    const ChatbotToolDeps& deps,
    std::string_view       threadId,
    std::string_view       explicitChat,
    std::string&           err
);

/// `chatbot_send_message`
///
/// - `threadId`: 工具的作用域键 (就是会话 id); 空 = 只能靠 `chat` 参数定位
/// - 返回给模型看的文本 (成功给"已排队"摘要, 失败给原因)
std::string toolSendMessage(
    const ChatbotToolDeps& deps,
    std::string_view       threadId,
    std::string_view       argsJson
);

/// `chatbot_history`
std::string toolHistory(
    const ChatbotToolDeps& deps,
    std::string_view       threadId,
    std::string_view       argsJson
);

/// `chatbot_chat_info`
std::string toolChatInfo(
    const ChatbotToolDeps& deps,
    std::string_view       threadId,
    std::string_view       argsJson
);

/// `chatbot_status`
std::string toolStatus(
    const ChatbotToolDeps& deps,
    std::string_view       threadId,
    std::string_view       argsJson
);

/// `chatbot_set_mode`
std::string toolSetMode(
    const ChatbotToolDeps& deps,
    std::string_view       threadId,
    std::string_view       argsJson
);

/// 工具名清单 (测试断言与文档用)
inline constexpr const char* kChatbotToolNames[] = {
    "chatbot_send_message",
    "chatbot_history",
    "chatbot_chat_info",
    "chatbot_status",
    "chatbot_set_mode",
};

} // namespace chatbot
} // namespace agentxx
