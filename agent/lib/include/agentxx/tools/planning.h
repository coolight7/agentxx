#pragma once

#include "agentxx/middlewares/planning.h"
#include "agentxx/tools/tool.h"
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/neograph.h>
#include <sstream>
#include <string>
#include <vector>

namespace agentxx {
namespace tools {

/// 两层任务规划 + 备忘录
/// - 提高模型在长流程中的注意力
/// - 目标层：Mermaid stateDiagram-v2 状态图描述整体工作流（大方向、依赖关系）
/// - 执行层：近期 todo list 追踪当前及下一步任务（执行细节、经验总结）
/// - 备忘录：可记录一些需要留意、记住的信息
// Two-level task planning to maintain focus in long workflows:
//   - Strategic: Mermaid stateDiagram-v2 captures the overall workflow
//     (high-level phases, dependencies, branching, recovery paths)
//   - Tactical: Near-term todo list tracks current + next-step tasks
//     (execution details, lessons learned, re-planning hints)
// Features:
//   - State diagram shows the big picture: what to do, in what order
//   - Todo items focus on immediate execution: what's happening now
//   - Persisted in agent state per thread
//   - Helps agent organize complex multi-step work
class WritePlanningTool : public XXToolBase {
protected:
  std::weak_ptr<agentxx::middleware::PlanningMiddlewareHandle> planningContext;

public:
  WritePlanningTool(std::weak_ptr<agentxx::middleware::PlanningMiddlewareHandle>
                        in_planningContext,
                    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : XXToolBase("planning_write", in_agentContext, false, false),
        planningContext(in_planningContext) {}

  neograph::ChatTool get_definition() const override {
    auto agentPtr = agentContext.lock();
    const auto &prompt = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
            {"type", "object"},
            {"required", neograph::json::array({"roadmap"})},
            {
                "properties",
                {
                    {
                        "roadmap",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("roadmap")},
                        },
                    },
                    {
                        "todos",
                        {
                            {"type", "array"},
                            {"items", {{"type", "object"}}},
                            {"description", prompt.getArg("todos")},
                        },
                    },
                    {
                        "notes",
                        {
                            {"type", "string"},
                            {"description", prompt.getArg("notes")},
                        },
                    },
                },
            },
        },
    };
  }

  std::optional<agentxx::middleware::SummarizationToolHandle>
  createSummarizationToolHandle() const override {
    return agentxx::middleware::SummarizationToolHandle{
        .generateDeduplicationKey = [](const neograph::json &args)
            -> std::optional<std::string> { return "planning_rw:"; },
        .truncateRequest =
            [](neograph::ToolCall &toolcall) {
              toolcall.arguments = R"({"tip":"[Outdated Content Truncated]"})";
            },
        .truncateResponse = nullptr,
    };
  }

  asio::awaitable<std::string>
  execute_async(const neograph::json &arguments) override {
    auto thread_id = arguments.value("thread_id", std::string{});
    if (thread_id.empty()) {
      co_return R"({"error":"Toolcall inner exec failed, need `thread_id`"})";
    }

    auto roadmap = arguments.value("roadmap", std::string{});
    if (roadmap.empty()) {
      co_return R"({"error":"Arg `roadmap` is empty, must provide a stateDiagram-v2 planning string"})";
    }

    auto handlePtr = planningContext.lock();
    if (nullptr == handlePtr) {
      co_return R"({"error":"planningContext is null"})";
    }

    auto state = co_await handlePtr->getStateItem(thread_id);

    neograph::json planStore = neograph::json::object();
    planStore["roadmap"] = roadmap;
    if (arguments.contains("todos")) {
      planStore["todos"] = arguments["todos"];
    }
    if (arguments.contains("notes")) {
      planStore["notes"] = arguments["notes"];
    }
    state->plannings[thread_id] = planStore;

    co_return "success";
  }
};

} // namespace tools
} // namespace agentxx