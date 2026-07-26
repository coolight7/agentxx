#pragma once

#include "agentxx/nodes/wrap_handle.h"
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace nodes {

class NEOGRAPH_API ToolcallWrapNode : public WrapHandleBaseNode<neograph::graph::ToolDispatchNode> {
protected:
public:

    inline static constexpr auto defNodeType = std::string_view{"xx_Toolcall"};

    ToolcallWrapNode(
        const std::string&                          in_name,
        const neograph::graph::NodeContext&         in_ctx,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    );

    void onHandleStartError(
        bool                                                errorRethrow,
        bool                                                isCurrentError,
        std::string_view                                    exceptionStr,
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        neograph::graph::NodeInput&                         in,
        neograph::graph::NodeOutput&                        result
    ) noexcept override;

    void onHandleBaseRunError(
        bool                         errorRethrow,
        bool                         isCurrentError,
        std::string_view             exceptionStr,
        neograph::graph::NodeInput&  in,
        neograph::graph::NodeOutput& result
    ) noexcept override;

    asio::awaitable<void> onHandleStart(
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        neograph::graph::NodeInput&                         in
    ) override;

    asio::awaitable<void> onHandleEnd(
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        const neograph::graph::NodeInput&                   in,
        neograph::graph::NodeOutput&                        result
    ) override;

    asio::awaitable<std::string> execTool(neograph::Tool* tool, neograph::json& args) const;

    asio::awaitable<void> baseRun(
        std::vector<std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface>>& handles,
        neograph::graph::NodeInput&                                                       in,
        neograph::graph::NodeOutput&                                                      out
    ) override;

    static asio::awaitable<void>
        defStdoutLogOnToolcallStart(neograph::graph::NodeInput& in, size_t limitOutput = 0);

    static asio::awaitable<void> defStdoutLogOnToolcallEnd(
        const neograph::graph::NodeInput& in,
        neograph::graph::NodeOutput&      result,
        size_t                            limitOutput = 0
    );
};
} // namespace nodes
} // namespace agentxx
