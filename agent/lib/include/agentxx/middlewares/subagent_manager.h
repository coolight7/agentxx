#pragma once

#include "agentxx/middlewares/middleware.h"
#include "agentxx/tools/subagent.h"
#include "asio/io_context.hpp"
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace agentxx {
namespace middleware {

class _SubagentManagerMiddlewareState : public BaseMiddlewareState {
public:

    _SubagentManagerMiddlewareState() {}
};

/// subagent 委派管理中间件
/// - 持有 [agentxx::tools::SubAgentManagerTool] (`agentxx_subagent`), 并负责
///   其生命周期: 经 [registerOnBus] 在 EventBus 上注册 subagent 执行服务
///   (service.subagent.execute), 供 summarization 压缩等非 toolcall 路径调用
/// - 按 [agentxx::agent::AgentConfig::enableSubagent] 决定是否把
///   `agentxx_subagent` 注入到 toolcalls (由 BaseAgent::initMiddlewareTools
///   自动收集进工具集, 模型可见即可发起委派):
///   - true  (默认): tool 注入, 模型可发起子代理委派
///   - false: tool 不注入 (模型不可见), 但事件总线服务仍注册,
///     程序内部路径 (如上下文压缩) 不受影响; AgentRunner 中断路径
///     `subagent` 在禁用时直接返回错误 (见 agent_runner.cpp)
class SubagentManagerMiddlewareHandle
    : public BaseMiddlewareHandle<_SubagentManagerMiddlewareState> {
public:

    SubagentManagerMiddlewareHandle(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    ~SubagentManagerMiddlewareHandle() override;

    /// 实际 subagent 管理工具 (名称/定义/执行均委托给它)
    std::shared_ptr<agentxx::tools::SubAgentManagerTool> tool = nullptr;

    /// 根据配置 (AgentConfig::enableSubagent) 决定是否注入 tool:
    /// - true: tool 放入 toolcalls, 由 BaseAgent 自动收集注入给模型
    /// - false: 不注入 (模型不可见), 事件总线服务不受影响
    void applyConfig(const std::shared_ptr<agentxx::agent::AgentConfig>& config);

    /// 在 EventBus 上注册 subagent 执行服务 (委托 tool)
    void registerOnBus(const std::shared_ptr<agentxx::event::EventBus>& bus);

    /// 从 EventBus 注销
    void unregisterFromBus();

private:

    std::weak_ptr<agentxx::event::EventBus> registeredBus_;
};

} // namespace middleware
} // namespace agentxx
