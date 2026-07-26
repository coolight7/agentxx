#pragma once

#include "agentxx/nodes/wrap_handle.h"
#include <string>
#include <string_view>

namespace agentxx {
namespace nodes {

class NEOGRAPH_API AgentStartCallWrapNode
    : public WrapHandleBaseNode<agentxx::nodes::WrapBaseNodeInterface> {
protected:
public:

    inline static constexpr auto defNodeType = std::string_view{"xx_MiddlewareWrapAgentStartCall"};

    AgentStartCallWrapNode(
        std::string_view                            name,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    );

    asio::awaitable<void> onHandleStart(
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        neograph::graph::NodeInput&                         in
    ) override;

    asio::awaitable<void> onHandleEnd(
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        const neograph::graph::NodeInput&                   in,
        neograph::graph::NodeOutput&                        result
    ) override;
};

class NEOGRAPH_API MiddlewareWrapAgentEndCallNode
    : public WrapHandleBaseNode<agentxx::nodes::WrapBaseNodeInterface> {
protected:
public:

    inline static constexpr auto defNodeType = std::string_view{"xx_MiddlewareWrapAgentEndCall"};

    MiddlewareWrapAgentEndCallNode(
        std::string_view                            name,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    );

    asio::awaitable<void> onHandleStart(
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        neograph::graph::NodeInput&                         in
    ) override;

    asio::awaitable<void> onHandleEnd(
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        const neograph::graph::NodeInput&                   in,
        neograph::graph::NodeOutput&                        result
    ) override;

    void onHandleEndError(
        bool                                                errorRethrow,
        bool                                                isCurrentError,
        std::string_view                                    exceptionStr,
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        const neograph::graph::NodeInput&                   in,
        neograph::graph::NodeOutput&                        result
    ) noexcept override;
};

} // namespace nodes
} // namespace agentxx
