#pragma once

#include "agentxx/tools/tool.h"

namespace agentxx {
namespace tools {

/// git worktree 管理工具 (worktree 模式核心工具)
///
/// 单工具多操作 (opt 分派), 与 share_store/planning 风格一致:
/// - `create`: 创建独立 worktree ({repoRoot}/.agentxx/agent/worktrees/{name})
///   并【绑定当前会话】—— 绑定后该会话的相对路径解析基准、权限隔离边界
///   自动切换到 worktree, 后续所有文件操作与命令执行均落在其中
/// - `info`:   当前绑定状态 + 仓库全部 worktree 列表 (含脏标记)
/// - `status`: 当前 worktree 工作区摘要 (未提交变更/领先提交), 支撑收尾时
///   提醒用户审查/提交/合并
/// - `remove`: 删除指定 (或当前绑定的) worktree; 存在未提交工作且未显式
///   force 时拒绝并给出摘要 —— 默认保留, 仅用户明确要求时删除
///
/// 平台约束联动:
/// - 绑定时向 PermissionMiddleware 注册会话隔离边界: 主检出子树写 DENY,
///   worktree 子树放行 (读主检出不受限), 隔离优先于白名单/模式默认规则
/// - 每轮提示词注入由 WorktreeMiddlewareHandle 负责 (状态感知的行为规范)
class GitWorktreeTool : public XXToolBase {
public:

    explicit GitWorktreeTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

} // namespace tools
} // namespace agentxx
