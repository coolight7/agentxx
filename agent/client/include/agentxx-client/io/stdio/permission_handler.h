#pragma once

#include "agentxx/agent/agent_io.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/events.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include <memory>
#include <neograph/json.h>
#include <neograph/types.h>
#include <string>

/// 权限询问 HIL 处理器 (CLI 实现)
/// - 注册为 EventBus 上 service.permission 的 server
/// - 收到 ReqPermission 后, 在终端询问用户 allow/deny
/// - 返回 RespPermission 决策
/// - 权限策略判定留在 PermissionMiddlewareHandle 栈内 (规则匹配),
///   仅当策略为 INTERRUPT 时走总线询问, 实现策略/机制分离
class StdioPermissionPrompter {
public:
  std::weak_ptr<agentxx::agent::AgentContext> agentContext;
  size_t serverId = 0;
  bool registered = false;

  explicit StdioPermissionPrompter(
      std::weak_ptr<agentxx::agent::AgentContext> ctx)
      : agentContext(std::move(ctx)) {}

  asio::awaitable<void> start() {
    if (registered) {
      co_return;
    }
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->bus) {
      XX_LOGE("StdioPermissionPrompter: AgentContext or bus is null");
      co_return;
    }
    auto &rr = ctxPtr->bus->getRR<agentxx::events::ReqPermission,
                                  agentxx::events::RespPermission>(
        agentxx::events::Topic::Permission);
    serverId = rr.serve(
        [this](const agentxx::events::ReqPermission &req, size_t /*corrId*/)
            -> asio::awaitable<agentxx::events::RespPermission> {
          co_return co_await handle(req);
        });
    registered = true;
    co_return;
  }

  void stop() {
    if (!registered) {
      return;
    }
    auto ctxPtr = agentContext.lock();
    if (ctxPtr && ctxPtr->bus) {
      auto &rr = ctxPtr->bus->getRR<agentxx::events::ReqPermission,
                                    agentxx::events::RespPermission>(
          agentxx::events::Topic::Permission);
      rr.removeServer(serverId);
    }
    registered = false;
  }

  ~StdioPermissionPrompter() { stop(); }

private:
  asio::awaitable<agentxx::events::RespPermission>
  handle(const agentxx::events::ReqPermission &req) {
    auto ctxPtr = agentContext.lock();
    auto session = ctxPtr ? ctxPtr->sessions->get(req.threadId) : nullptr;
    auto io = session ? session->io : nullptr;
    if (!io) {
      co_return agentxx::events::RespPermission{
          .decision = agentxx::events::RespPermission::Decision::Deny,
          .reason = "no IO available",
      };
    }

    bool allow =
        co_await io->promptPermission(req.toolName, req.category, req.target);

    co_return agentxx::events::RespPermission{
        .decision = allow ? agentxx::events::RespPermission::Decision::Allow
                          : agentxx::events::RespPermission::Decision::Deny,
        .reason = allow ? "" : "user denied",
    };
  }
};