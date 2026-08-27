#pragma once

#include "agentxx/middlewares/middleware.h"
#include "asio/io_context.hpp"
#include <map>
#include <neograph/neograph.h>
#include <string>

namespace agentxx {
namespace middleware {

class PlanningMiddlewareState : public BaseMiddlewareState {
public:

    /// <sessionId, todoListJson>
    /// [会话独立] 任务规划列表，由 `agentxx_planning` 工具 (write 模式) 读写
    std::map<std::string, neograph::json> plannings{};

    PlanningMiddlewareState() {}
};

class PlanningMiddlewareHandle : public BaseMiddlewareHandle<PlanningMiddlewareState> {
protected:
public:

    PlanningMiddlewareHandle(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext) :
        BaseMiddlewareHandle<PlanningMiddlewareState>("PlanningMiddlewareHandle", in_agentContext) {
    }

    asio::awaitable<void> onAgentcallStartFunc(neograph::graph::NodeInput& in) override;
};

} // namespace middleware
} // namespace agentxx