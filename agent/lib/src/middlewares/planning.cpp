#include "agentxx/middlewares/planning.h"

#include <vector>

namespace agentxx {
namespace middleware {

asio::awaitable<void> PlanningMiddlewareHandle::onModelcallStartFunc(neograph::graph::NodeInput& in
) {
    auto  agentCtxPtr = agentContext.lock();
    auto& appendSystemPromptList
        = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<std::vector<std::string>>(
            in.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_systemMessage
        );
    appendSystemPromptList.push_back(agentCtxPtr->agentConfig->prompt.systemPlanningPrompt);
    co_return;
}

} // namespace middleware
} // namespace agentxx
