#pragma once

#include "agentxx/agent/conversation_types.h"
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

    /// 增量事件推送 (流式 token、tool 生命周期、轮次边界)
    virtual void onDelta(const Delta& delta) = 0;

    /// 全量/部分同步 (从 fullHistory 校准 client 状态)
    virtual void onSync(const SyncPayload& payload) = 0;

    virtual asio::awaitable<std::optional<std::string>> getInput() = 0;

    /// 统一的 HIL 处理: 用于权限询问、中断输入收集等所有用户交互场景
    virtual asio::awaitable<neograph::json> handleInterrupt(
        const std::string& threadId,
        const std::string& interruptNode,
        const std::string& interruptValue,
        const std::string& interruptArgJson
    ) = 0;

    /// 在会话总线上注册本 IO 的事件处理器 (interrupt / permission)
    virtual void registerOnBus(std::shared_ptr<agentxx::middleware::EventBus> sessionBus);
};

} // namespace agent
} // namespace agentxx
