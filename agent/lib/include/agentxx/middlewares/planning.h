#pragma once

#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/string_util.h"
#include "asio/io_context.hpp"
#include "fmt/format.h"
#include "yaml-cpp/yaml.h"
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/neograph.h>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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
  onModelcallStartFunc(neograph::graph::NodeInput &in) override {
    auto agentCtxPtr = agentContext.lock();
    auto &appendSystemPromptList =
        agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<
            std::vector<std::string>>(
            in.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_systemMessage);
    appendSystemPromptList.push_back(
        agentCtxPtr->agentConfig->prompt.systemPlanningPrompt);
    co_return;
  }
};

} // namespace middleware
} // namespace agentxx