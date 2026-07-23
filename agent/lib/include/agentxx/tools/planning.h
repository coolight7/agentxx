#pragma once

#include "agentxx/tools/tool.h"

namespace agentxx {
namespace middleware {
class PlanningMiddlewareHandle;
} // namespace middleware

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

    WritePlanningTool(
        std::weak_ptr<agentxx::middleware::PlanningMiddlewareHandle> in_planningContext,
        std::weak_ptr<agentxx::agent::AgentContext>                  in_agentContext);

    neograph::ChatTool get_definition() const override;

    std::optional<agentxx::middleware::SummarizationToolHandle>
        createSummarizationToolHandle() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

} // namespace tools
} // namespace agentxx
