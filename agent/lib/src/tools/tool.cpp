#include "agentxx/tools/tool.h"

#include <string>
#include <utility>

namespace agentxx {
namespace tools {

std::shared_ptr<neograph::graph::CancelToken> getSessionCancelToken(
    const std::shared_ptr<agentxx::agent::AgentContext>& agentCtx,
    const neograph::json&                                args
) {
    if (nullptr == agentCtx || nullptr == agentCtx->sessions) {
        return nullptr;
    }
    auto sessionId = args.value("sessionId", std::string{});
    if (sessionId.empty()) {
        return nullptr;
    }
    auto sess = agentCtx->sessions->get(sessionId);
    if (nullptr == sess) {
        return nullptr;
    }
    return sess->getCancelToken();
}

XXToolBase::XXToolBase(
    std::string_view                            in_name,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext,
    bool                                        in_autoSummaryOutput,
    bool                                        in_canDelayLoad,
    size_t                                      in_maxRetry,
    bool                                        in_repeatCallCheck
) :
    name(in_name),
    agentContext(in_agentContext),
    autoSummaryOutput(in_autoSummaryOutput),
    canDelayLoad(in_canDelayLoad),
    maxRetry(in_maxRetry),
    repeatCallCheck(in_repeatCallCheck) {
    extra["autoSummaryOutput"] = autoSummaryOutput ? "true" : "false";
    extra["canDelayLoad"]      = canDelayLoad ? "true" : "false";
    extra["maxRetry"]          = std::to_string(maxRetry);
    extra["repeatCallCheck"]   = repeatCallCheck ? "true" : "false";
}

std::string XXToolBase::get_name() const {
    return name;
}

std::optional<agentxx::middleware::SummarizationToolHandle>
    XXToolBase::createSummarizationToolHandle() const {
    return std::nullopt;
    // return agentxx::middleware::SummarizationToolHandle{
    //     .generateDeduplicationKey =
    //         [](const neograph::json &args) -> std::optional<std::string> {
    //           return "tool_name:unique_key";
    //         },
    //     .truncateRequest =
    //         [](neograph::ToolCall &toolcall) {
    //           toolcall.arguments =
    //               R"({"tip":"[Outdated Message Truncated]"})";
    //         },
    //     .truncateResponse =
    //         [](neograph::ChatMessage &msg) {
    //           msg.content = "[Outdated Content truncated]";
    //         },
    // };
}

XXToolWrap::XXToolWrap(
    std::unique_ptr<neograph::Tool>&&                           in_inner,
    std::weak_ptr<agentxx::agent::AgentContext>                 in_agentContext,
    bool                                                        in_autoSummaryOutput,
    bool                                                        in_canDelayLoad,
    size_t                                                      in_maxRetry,
    std::optional<agentxx::middleware::SummarizationToolHandle> in_summarizationHandle,
    bool                                                        in_repeatCallCheck
) :
    XXToolBase(
        in_inner->get_name(),
        in_agentContext,
        in_autoSummaryOutput,
        in_canDelayLoad,
        in_maxRetry,
        in_repeatCallCheck
    ),
    inner(std::move(in_inner)),
    summarizationHandle(in_summarizationHandle) {}

std::string XXToolWrap::get_name() const {
    return inner->get_name();
}

neograph::ChatTool XXToolWrap::get_definition() const {
    return inner->get_definition();
}

asio::awaitable<std::string> XXToolWrap::execute_async(const neograph::json& arguments) {
    co_return co_await inner->real_execute_async(arguments);
}

} // namespace tools
} // namespace agentxx
