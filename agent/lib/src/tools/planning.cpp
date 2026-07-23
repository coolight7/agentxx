#include "agentxx/tools/planning.h"

#include "agentxx/middlewares/planning.h"
#include <string>

namespace agentxx {
namespace tools {

WritePlanningTool::WritePlanningTool(
    std::weak_ptr<agentxx::middleware::PlanningMiddlewareHandle> in_planningContext,
    std::weak_ptr<agentxx::agent::AgentContext>                  in_agentContext
) :
    XXToolBase("planning_write", in_agentContext, false, false),
    planningContext(in_planningContext) {}

neograph::ChatTool WritePlanningTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

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
            }, },
    };
}

std::optional<agentxx::middleware::SummarizationToolHandle>
    WritePlanningTool::createSummarizationToolHandle() const {
    return agentxx::middleware::SummarizationToolHandle{
        .generateDeduplicationKey = [](const neograph::json& args) -> std::optional<std::string> {
            return "planning_rw:";
        },
        .truncateRequest =
            [](neograph::ToolCall& toolcall) {
                toolcall.arguments = R"({"tip":"[Outdated Content Truncated]"})";
            },
        .truncateResponse = nullptr,
    };
}

asio::awaitable<std::string> WritePlanningTool::execute_async(const neograph::json& arguments) {
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
    planStore["roadmap"]     = roadmap;
    if (arguments.contains("todos")) {
        planStore["todos"] = arguments["todos"];
    }
    if (arguments.contains("notes")) {
        planStore["notes"] = arguments["notes"];
    }
    state->plannings[thread_id] = planStore;

    co_return "success";
}

} // namespace tools
} // namespace agentxx
