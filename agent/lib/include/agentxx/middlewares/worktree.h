#pragma once

#include "agentxx/middlewares/middleware.h"

namespace agentxx {
namespace middleware {

/// worktree 模式提示词中间件 (yaml `worktree.enable` 开启时由 CodeAgent 注册)
///
/// 每轮 agent 调用开始时向系统提示词追加当前会话的 worktree 状态与行为规范:
/// - 未绑定: 提示模型在涉及代码修改的任务开始时先调用 agentxx_git_worktree
///   创建独立 worktree (创建即绑定)
/// - 已绑定: 展示 worktree 路径/分支/主检出路径, 规范提交与收尾行为
///   (阶段性 commit、完成前提醒用户审查合并、禁止修改主检出)
/// - 子代理继承: AgentConfig::inheritedWorktreePath 非空时按已绑定语义展示
///
/// 实现方式与 PlanningMiddlewareHandle 一致: 经 graphData 的
/// xx_appendSystemMessage 通道追加, 由 ModelCallWrapNode 合并进 system 消息
class WorktreeMiddlewareHandle : public BaseMiddlewareHandle<BaseMiddlewareState> {
public:

    explicit WorktreeMiddlewareHandle(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext) :
        BaseMiddlewareHandle<BaseMiddlewareState>("WorktreeMiddlewareHandle", in_agentContext) {}

    asio::awaitable<void> onAgentcallStartFunc(neograph::graph::NodeInput& in) override;
};

} // namespace middleware
} // namespace agentxx
