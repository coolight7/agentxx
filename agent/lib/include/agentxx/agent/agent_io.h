#pragma once

#include "agentxx/util/log.h"
#include "asio/awaitable.hpp"
#include "asio/this_coro.hpp"
#include "fmt/format.h"
#include "neograph/json.h"
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace agentxx::middleware {
class EventBus;
} // namespace agentxx::middleware

namespace agentxx {
namespace agent {

class AgentIOBase {
public:

    virtual ~AgentIOBase() = default;

    virtual void onToken(const std::string& token, const std::string& kind) = 0;

    virtual asio::awaitable<std::optional<std::string>> getInput() = 0;

    /// 统一的 HIL 处理: 用于权限询问、中断输入收集等所有用户交互场景
    /// - interruptArgJson: 单个 InterruptHandleArg 的 JSON 序列化
    ///   权限场景时 name="permission", inputs 含单个 bool 项
    /// - 返回结果 JSON (对应 resumeValues 中的一项)
    virtual asio::awaitable<neograph::json> handleInterrupt(
        const std::string& threadId,
        const std::string& interruptNode,
        const std::string& interruptValue,
        const std::string& interruptArgJson
    ) = 0;

    /// 在会话总线上注册本 IO 的 interrupt/permission 处理器
    virtual void registerOnBus(std::shared_ptr<agentxx::middleware::EventBus> sessionBus);

    /// toolcall 开始 (每个 tool 调用一次); 默认空实现, 由具体 IO 决定如何展示
    virtual void onToolStart(
        const std::string& toolName,
        const std::string& toolCallId,
        const std::string& arguments
    );

    /// toolcall 结束 (每个 tool 调用一次); result 为执行结果, hasError 标记失败
    virtual void onToolEnd(
        const std::string& toolName,
        const std::string& toolCallId,
        const std::string& result,
        bool               hasError
    );
};

} // namespace agent
} // namespace agentxx
