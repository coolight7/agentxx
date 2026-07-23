#pragma once

#include "agentxx/util/log.h"
#include "asio/awaitable.hpp"
#include "asio/this_coro.hpp"
#include "fmt/format.h"
#include "neograph/api.h"
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace agentxx {
namespace agent {

class AgentIOBase {
public:

    virtual ~AgentIOBase() = default;

    virtual void onToken(const std::string& token, const std::string& kind) = 0;

    virtual void onDisplay(const std::string& level, const std::string& content) = 0;

    virtual asio::awaitable<std::optional<std::string>> getInput() = 0;

    virtual asio::awaitable<bool> promptPermission(
        const std::string& toolName,
        const std::string& category,
        const std::string& target
    ) = 0;

    virtual void onInterrupt(
        const std::string& node,
        const std::string& value,
        const std::string& handleName
    ) = 0;

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
