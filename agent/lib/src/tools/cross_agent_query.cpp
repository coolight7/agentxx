#include "agentxx/tools/cross_agent_query.h"

#include "agentxx/agent/context.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/events.h"
#include "agentxx/util/log.h"
#include "fmt/format.h"
#include <chrono>
#include <string>

namespace agentxx {
namespace tools {

CrossAgentQueryTool::CrossAgentQueryTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("cross_agent_query", in_agentContext, true, false, 0) {}

std::string CrossAgentQueryTool::get_name() const {
    return "cross_agent_query";
}

neograph::ChatTool CrossAgentQueryTool::get_definition() const {
    return {
        "cross_agent_query",
        "Query another agent (by name) with a message and receive its "
        "response. Use this to ask another sub-agent or the main agent for "
        "information.",
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {
                    {
                        "to_agent",
                        {
                            {"type", "string"},
                            {"description", "Name of the target agent to query."},
                        },
                    },
                    {
                        "message",
                        {
                            {"type", "string"},
                            {"description", "Query message content."},
                        },
                    },
                },
            }, {
                "required",
                neograph::json::array({"to_agent", "message"}),
            }, },
    };
}

asio::awaitable<std::string> CrossAgentQueryTool::execute_async(const neograph::json& arguments) {
    auto toAgent = arguments.value("to_agent", std::string{});
    if (toAgent.empty()) {
        co_return R"({"error":"Arg `to_agent` is empty"})";
    }
    auto message = arguments.value("message", std::string{});
    if (message.empty()) {
        co_return R"({"error":"Arg `message` is empty"})";
    }

    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->bus) {
        co_return R"({"error":"EventBus not available"})";
    }

    auto thread_id = arguments.value("thread_id", std::string{});
    auto selfName  = ctxPtr->agentConfig ? ctxPtr->agentConfig->agentName : std::string{"unknown"};

    auto resp = co_await ctxPtr->bus
                    ->request<agentxx::events::ReqCrossAgent, agentxx::events::RespCrossAgent>(
                        agentxx::events::Topic::CrossAgent,
                        agentxx::events::ReqCrossAgent{
                            .fromAgent    = selfName,
                            .fromThreadId = thread_id,
                            .toAgent      = toAgent,
                            .message      = message,
                        },
                        std::chrono::seconds(60)
                    );

    if (!resp.has_value()) {
        co_return fmt::format(
            R"({{"error":"Cross-agent query to `{}` timed out or no server"}})",
            toAgent
        );
    }
    if (resp->hasError) {
        co_return fmt::format(R"({{"error":"{}"}})", resp->errorMessage);
    }
    co_return resp->content;
}

} // namespace tools
} // namespace agentxx
