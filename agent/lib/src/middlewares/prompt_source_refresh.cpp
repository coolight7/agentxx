#include "agentxx/middlewares/prompt_source_refresh.h"

#include "agentxx/agent/context.h"
#include "agentxx/agent/prompt_sources.h"
#include "agentxx/util/observability.h"
#include "utilxx_base/log.h"

namespace agentxx {
namespace middleware {

PromptSourceRefreshHandle::PromptSourceRefreshHandle(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    BaseMiddlewareHandle<BaseMiddlewareState>(
        "PromptSourceRefreshHandle",
        std::move(in_agentContext)
    ) {}

asio::awaitable<void> PromptSourceRefreshHandle::onModelcallStartFunc(
    neograph::graph::NodeInput& in
) {
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->promptSources) {
        co_return;
    }

    // 运行信息: 会话 id / 会话工作目录 (相对路径的解析基准) / 取消令牌
    agentxx::agent::PromptRefreshContext rc;
    rc.sessionId   = std::string{in.ctx.thread_id};
    rc.workDir     = ctxPtr->getSessionWorkDir(in.ctx.thread_id);
    rc.cancelToken = in.ctx.cancel_token;

    // 原因由注册表按会话内的标记决定 (Initial / ResourcesChanged /
    // AfterCompaction / Explicit); 没有标记时立即返回, 不产生 I/O
    co_await ctxPtr->promptSources->ensureFresh(rc.sessionId, rc);

    if (ctxPtr->metrics) {
        auto stats = ctxPtr->promptSources->takeRefreshStats();
        if (stats.refreshes > 0 || stats.failures > 0) {
            ctxPtr->metrics->notePromptRefresh(
                stats.lastMs,
                static_cast<int64_t>(stats.changedSections),
                static_cast<int64_t>(stats.failures),
                static_cast<int64_t>(stats.sectionChars)
            );
        }
    }
    co_return;
}

} // namespace middleware
} // namespace agentxx
