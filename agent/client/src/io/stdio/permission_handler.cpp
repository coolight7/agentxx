#include "agentxx-client/io/stdio/permission_handler.h"

#include "agentxx/agent/agent_io.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/util/log.h"
#include <utility>

StdioPermissionPrompter::StdioPermissionPrompter(std::weak_ptr<agentxx::agent::AgentContext> ctx) :
    agentContext(std::move(ctx)) {}

asio::awaitable<void> StdioPermissionPrompter::start() {
    if (registered) {
        co_return;
    }
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->bus) {
        XX_LOGE("StdioPermissionPrompter: AgentContext or bus is null");
        co_return;
    }
    auto& rr = ctxPtr->bus->getRR<agentxx::events::ReqPermission, agentxx::events::RespPermission>(
        agentxx::events::Topic::Permission
    );
    serverId = rr.serve(
        [this](const agentxx::events::ReqPermission& req, size_t /*corrId*/)
            -> asio::awaitable<agentxx::events::RespPermission> {
            co_return co_await handle(req);
        }
    );
    registered = true;
    co_return;
}

void StdioPermissionPrompter::stop() {
    if (!registered) {
        return;
    }
    auto ctxPtr = agentContext.lock();
    if (ctxPtr && ctxPtr->bus) {
        auto& rr
            = ctxPtr->bus->getRR<agentxx::events::ReqPermission, agentxx::events::RespPermission>(
                agentxx::events::Topic::Permission
            );
        rr.removeServer(serverId);
    }
    registered = false;
}

StdioPermissionPrompter::~StdioPermissionPrompter() {
    stop();
}

asio::awaitable<agentxx::events::RespPermission>
    StdioPermissionPrompter::handle(const agentxx::events::ReqPermission& req) {
    auto ctxPtr  = agentContext.lock();
    auto session = ctxPtr ? ctxPtr->sessions->get(req.threadId) : nullptr;
    auto io      = session ? session->io : nullptr;
    if (!io) {
        co_return agentxx::events::RespPermission{
            .decision = agentxx::events::RespPermission::Decision::Deny,
            .reason   = "no IO available",
        };
    }

    bool allow = co_await io->promptPermission(req.toolName, req.category, req.target);

    co_return agentxx::events::RespPermission{
        .decision = allow ? agentxx::events::RespPermission::Decision::Allow
                          : agentxx::events::RespPermission::Decision::Deny,
        .reason   = allow ? "" : "user denied",
    };
}
