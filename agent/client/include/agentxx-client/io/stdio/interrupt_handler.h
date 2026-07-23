#pragma once

#include "agentxx/middlewares/events.h"
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <neograph/json.h>
#include <optional>
#include <string>
#include <string_view>

namespace agentxx {
namespace agent {
class AgentContext;
}

namespace middleware {
class InterruptHandleArg;
}
} // namespace agentxx

/// 中断 HIL 处理器 (CLI 实现)
/// - 注册为 EventBus 上 service.interrupt 的 server
/// - 收到 ReqInterrupt 后, 解析为 InterruptHandleArg, 然后由
///   execInterruptHandle 处理
/// - 把结果包装为 RespInterrupt 回填
class StdioInterruptHandler {
public:

    std::weak_ptr<agentxx::agent::AgentContext> agentContext;
    size_t                                      serverId   = 0;
    bool                                        registered = false;

    explicit StdioInterruptHandler(std::weak_ptr<agentxx::agent::AgentContext> ctx);

    /// 注册到总线
    asio::awaitable<void> start();

    /// 注销
    void stop();

    ~StdioInterruptHandler();

private:

    void registerInterruptHandles();

    asio::awaitable<std::optional<neograph::json>>
        execInterruptHandle(std::string_view                               name,
                            const agentxx::middleware::InterruptHandleArg& arg,
                            const std::string&                             threadId);

    asio::awaitable<agentxx::events::RespInterrupt>
        handle(const agentxx::events::ReqInterrupt& req);

    /// <name, handle>
    std::map<std::string,
             std::function<asio::awaitable<neograph::json>(
                 const agentxx::middleware::InterruptHandleArg&,
                 const std::string& threadId)>>
        interruptHandles{};
};
