#include "agentxx/middlewares/worktree.h"

#include <vector>

namespace agentxx {
namespace middleware {

asio::awaitable<void> WorktreeMiddlewareHandle::onAgentcallStartFunc(neograph::graph::NodeInput& in
) {
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->agentConfig) {
        co_return;
    }

    std::string tip;
    // 绑定状态判定: 会话级绑定 (主 agent) > 配置继承 (子代理) > 未绑定
    std::string sessionId = in.ctx.thread_id;
    auto        session   = ctxPtr->sessions->get(sessionId);
    if (session && !session->getWorktreeBinding().path.empty()) {
        const auto& wb = session->getWorktreeBinding();
        tip            = fmt::format(
            R"(## Git Worktree Isolation (ACTIVE)
You are working inside an isolated git worktree:
- worktree path: `{}` (branch: `{}`)
- main checkout (READ-ONLY for you): `{}`

Rules:
- File tools resolve relative paths inside the worktree automatically; absolute paths into the main checkout are DENIED for writes.
- Run all git commands from inside the worktree; commit your changes regularly.
- Do NOT reuse build directories from the main checkout — configure a fresh build directory inside the worktree (build caches embed absolute paths).
- Before finishing, run `agentxx_git_worktree` (opt=status), summarize pending changes, and remind the user to review / commit / merge them. Keep the worktree unless the user asks to delete it.)",
            wb.path,
            wb.branch.empty() ? "(detached)" : wb.branch,
            wb.repoRoot
        );
    } else if (!ctxPtr->agentConfig->inheritedWorktreePath.empty()) {
        tip = fmt::format(
            R"(## Git Worktree Isolation (INHERITED)
You are running inside the git worktree `{}` inherited from the parent agent.
All file operations and builds must stay inside this worktree; never modify the main checkout. Commit changes with git when a unit of work completes.)",
            ctxPtr->agentConfig->inheritedWorktreePath
        );
    } else {
        tip = R"(## Git Worktree Mode
This session supports isolated git worktrees (`agentxx_git_worktree` tool).
When the task modifies code, create an isolated worktree FIRST via opt=create, then do all edits/builds/tests inside it — this keeps parallel sessions from interfering with each other.
Read-only tasks (analysis/questions) don't need a worktree.)";
    }

    auto& appendSystemPromptList
        = ctxPtr->middlewareHandleContext->getGraphDataItemValue<std::vector<std::string>>(
            in.ctx.thread_id,
            agentxx::middleware::MiddlewareContext::graphDataKey_appendSystemMessage
        );
    appendSystemPromptList.push_back(std::move(tip));
    co_return;
}

} // namespace middleware
} // namespace agentxx
