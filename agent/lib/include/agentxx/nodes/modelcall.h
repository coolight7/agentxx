#pragma once

#include "agentxx/nodes/wrap_handle.h"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace nodes {

class NEOGRAPH_API ModelCallWrapNode : public WrapHandleBaseNode<neograph::graph::LLMCallNode> {
protected:
public:

    inline static constexpr auto defNodeType = std::string_view{"xx_ModelCallWrap"};

    /// NodeContext.extra_config 中标记是否启用运行时动态模型切换的 key
    /// - 仅主 agent 的 llm 节点启用; subagent 使用自身固定的 provider
    inline static constexpr auto defUseModelRegistryKey = std::string_view{"xx_useModelRegistry"};

protected:

    /// 是否启用运行时动态模型切换 (经 agentContext->modelRegistry 解析)
    /// - false 时使用节点构造时 NodeContext 提供的固定 provider_/model_
    bool useDynamicModel_ = false;

public:

    ModelCallWrapNode(
        std::string_view                            name,
        const neograph::graph::NodeContext&         ctx,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    );

    /// 解析指定会话使用的 Provider
    /// - 启用动态切换时按会话 (thread_id) 选择的模型经 modelRegistry 解析
    /// - 否则回退到节点构造时的 provider_
    std::shared_ptr<neograph::Provider> resolveCurrentProvider(std::string_view threadId);

    /// 解析指定会话使用的模型名 (发送给 LLM api 的 model 字段)
    std::string resolveCurrentModelName(std::string_view threadId) const;

    asio::awaitable<neograph::ChatCompletion>
        onReceiveToken(neograph::CompletionParams& params, neograph::graph::NodeInput& input);

    neograph::CompletionParams
        build_params(const neograph::graph::GraphState& state, std::string_view threadId) const;

    asio::awaitable<neograph::graph::NodeOutput> callLLM(neograph::graph::NodeInput& in);

    asio::awaitable<void> onHandleStart(
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        neograph::graph::NodeInput&                         in
    ) override;

    asio::awaitable<void> onHandleEnd(
        agentxx::middleware::BaseMiddlewareHandleInterface& item,
        const neograph::graph::NodeInput&                   in,
        neograph::graph::NodeOutput&                        result
    ) override;

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

    void repairMessages(neograph::graph::NodeInput& in);

    asio::awaitable<void> baseRun(
        std::vector<std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface>>& handles,
        neograph::graph::NodeInput&                                                       in,
        neograph::graph::NodeOutput&                                                      result
    ) override;
};

} // namespace nodes
} // namespace agentxx
