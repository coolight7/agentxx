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

    inline static const auto defNodeType = std::string{"xx_ModelCallWrap"};

    /// NodeContext.extra_config 中标记是否启用运行时动态模型切换的 key
    /// - 仅主 agent 的 llm 节点启用; subagent 使用自身固定的 provider
    inline static const auto defUseModelRegistryKey = std::string{"xx_useModelRegistry"};

    /// NodeContext.extra_config 中标记是否由本节点自动给出循环路由的 key (默认 true)
    /// - true (默认图): 本节点按"是否有 tool_calls"返回 Command (tools / agent_end),
    ///   图定义只需给出静态默认边 (llm -> agent_end), 不再依赖读上下文的条件边
    /// - false: 不返回 Command, 完全交给图定义的边/条件边决定
    ///   (插件重写图定义并把 llm 接到自定义路由节点时使用)
    inline static const auto defAutoRouteKey = std::string{"xx_autoRoute"};

protected:

    /// 是否启用运行时动态模型切换 (经 agentContext->modelRegistry 解析)
    /// - false 时使用节点构造时 NodeContext 提供的固定 provider_/model_
    bool useDynamicModel_ = false;

    /// 是否由本节点给出循环路由 (见 [defAutoRouteKey]; 默认 true)
    bool autoRoute_ = true;

public:

    ModelCallWrapNode(
        std::string_view                            name,
        const neograph::graph::NodeContext&         ctx,
        std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
    );

    /// 解析指定会话使用的 Provider
    /// - 启用动态切换时按会话 (sessionId) 选择的模型经 modelRegistry 解析
    /// - 否则回退到节点构造时的 provider_
    std::shared_ptr<neograph::Provider> resolveCurrentProvider(std::string_view sessionId);

    /// 解析指定会话使用的模型名 (发送给 LLM api 的 model 字段)
    std::string resolveCurrentModelName(std::string_view sessionId) const;

    asio::awaitable<neograph::ChatCompletion> onReceiveToken(
        neograph::CompletionParams& params,
        neograph::graph::NodeInput& input
    ) override;

    neograph::CompletionParams build_params(std::string_view sessionId) const;

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

    void repairMessages(neograph::graph::NodeInput& in);

    asio::awaitable<void> baseRun(
        std::vector<std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface>>& handles,
        neograph::graph::NodeInput&                                                       in,
        neograph::graph::NodeOutput&                                                      result
    ) override;
};

} // namespace nodes
} // namespace agentxx
