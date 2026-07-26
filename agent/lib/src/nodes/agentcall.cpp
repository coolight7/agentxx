#include "agentxx/nodes/agentcall.h"

namespace agentxx {
namespace nodes {

AgentStartCallWrapNode::AgentStartCallWrapNode(
    const std::string&                          name,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    WrapHandleBaseNode<agentxx::nodes::WrapBaseNodeInterface>(name, in_agentContext) {}

asio::awaitable<void> AgentStartCallWrapNode::onHandleStart(
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    neograph::graph::NodeInput&                         in
) {
    {
        // 创建单次执行的临时数据
        auto ptr = agentContext.lock();
        ptr->middlewareHandleContext->graphData[in.ctx.thread_id].clear();
    }

    co_await item.onAgentcallStartFunc(in);
}

asio::awaitable<void> AgentStartCallWrapNode::onHandleEnd(
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    const neograph::graph::NodeInput&                   in,
    neograph::graph::NodeOutput&                        result
) {
    co_return;
}

MiddlewareWrapAgentEndCallNode::MiddlewareWrapAgentEndCallNode(
    const std::string&                          name,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    WrapHandleBaseNode<agentxx::nodes::WrapBaseNodeInterface>(name, in_agentContext) {}

asio::awaitable<void> MiddlewareWrapAgentEndCallNode::onHandleStart(
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    neograph::graph::NodeInput&                         in
) {
    co_return;
}

asio::awaitable<void> MiddlewareWrapAgentEndCallNode::onHandleEnd(
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    const neograph::graph::NodeInput&                   in,
    neograph::graph::NodeOutput&                        result
) {
    co_await item.onAgentcallEndFunc(in, result);

    {
        // 清理单次执行的临时数据
        auto ptr = agentContext.lock();
        auto it  = ptr->middlewareHandleContext->graphData.find(in.ctx.thread_id);
        if (it != ptr->middlewareHandleContext->graphData.end()) {
            ptr->middlewareHandleContext->graphData.erase(it);
        }
    }
}

void MiddlewareWrapAgentEndCallNode::onHandleEndError(
    bool                                                errorRethrow,
    bool                                                isCurrentError,
    std::string_view                                    exceptionStr,
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    const neograph::graph::NodeInput&                   in,
    neograph::graph::NodeOutput&                        result
) noexcept {
    {
        // 清理单次执行的临时数据
        auto ptr = agentContext.lock();
        auto it  = ptr->middlewareHandleContext->graphData.find(in.ctx.thread_id);
        if (it != ptr->middlewareHandleContext->graphData.end()) {
            ptr->middlewareHandleContext->graphData.erase(it);
        }
    }
}

} // namespace nodes
} // namespace agentxx
