#include "agentxx/nodes/agentcall.h"

namespace agentxx {
namespace nodes {

AgentStartCallWrapNode::AgentStartCallWrapNode(
    std::string_view                            name,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    WrapHandleBaseNode<agentxx::nodes::WrapBaseNodeInterface>(name, in_agentContext) {}

asio::awaitable<void> AgentStartCallWrapNode::onNodeStart(neograph::graph::NodeInput& in) {
    {
        // 创建单次执行的临时数据
        auto ptr = agentContext.lock();
        ptr->middlewareHandleContext->graphData[in.ctx.thread_id].clear();
    }
    co_return;
}

asio::awaitable<void> AgentStartCallWrapNode::onNodeEnd(
    const neograph::graph::NodeInput& in,
    neograph::graph::NodeOutput&      result
) {
    co_return;
}

asio::awaitable<void> AgentStartCallWrapNode::onHandleStart(
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    neograph::graph::NodeInput&                         in
) {
    co_await item.onAgentcallStartFunc(in);
}

asio::awaitable<void> AgentStartCallWrapNode::onHandleEnd(
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    const neograph::graph::NodeInput&                   in,
    neograph::graph::NodeOutput&                        result
) {
    co_return;
}

AgentEndCallWrapNode::AgentEndCallWrapNode(
    std::string_view                            name,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    WrapHandleBaseNode<agentxx::nodes::WrapBaseNodeInterface>(name, in_agentContext) {}

asio::awaitable<void> AgentEndCallWrapNode::onNodeStart(neograph::graph::NodeInput& in) {
    co_return;
}

asio::awaitable<void> AgentEndCallWrapNode::onNodeEnd(
    const neograph::graph::NodeInput& in,
    neograph::graph::NodeOutput&      result
) {
    // {
    //     // 清理单次执行的临时数据
    //     auto ptr = agentContext.lock();
    //     auto it  = ptr->middlewareHandleContext->graphData.find(in.ctx.thread_id);
    //     if (it != ptr->middlewareHandleContext->graphData.end()) {
    //         ptr->middlewareHandleContext->graphData.erase(it);
    //     }
    // }
    co_return;
}

asio::awaitable<void> AgentEndCallWrapNode::onHandleStart(
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    neograph::graph::NodeInput&                         in
) {
    co_return;
}

asio::awaitable<void> AgentEndCallWrapNode::onHandleEnd(
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    const neograph::graph::NodeInput&                   in,
    neograph::graph::NodeOutput&                        result
) {
    co_await item.onAgentcallEndFunc(in, result);
}

void AgentEndCallWrapNode::onHandleEndError(
    bool                                                errorRethrow,
    bool                                                isCurrentError,
    std::string_view                                    exceptionStr,
    agentxx::middleware::BaseMiddlewareHandleInterface& item,
    const neograph::graph::NodeInput&                   in,
    neograph::graph::NodeOutput&                        result
) noexcept {
    return;
}

} // namespace nodes
} // namespace agentxx
