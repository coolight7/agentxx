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
  /// <thread_id, todoListJson>
  /// [会话独立] 任务规划列表，由 `planning_write` 读写
  std::map<std::string, neograph::json> plannings{};

  PlanningMiddlewareState() {}
};

class PlanningMiddlewareHandle
    : public BaseMiddlewareHandle<PlanningMiddlewareState> {
protected:
public:
  PlanningMiddlewareHandle(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : BaseMiddlewareHandle<PlanningMiddlewareState>(
            "PlanningMiddlewareHandle", in_agentContext) {}

  asio::awaitable<void>
  onModelcallStartFunc(neograph::graph::NodeInput &in) override;
};

} // namespace middleware
} // namespace agentxx