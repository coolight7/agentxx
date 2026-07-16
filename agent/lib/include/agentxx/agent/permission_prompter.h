#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/agent/stdin_reader.h"
#include "agentxx/events.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "neograph/neograph.h"
#include <iostream>
#include <memory>
#include <string>

namespace agentxx {
namespace agent {

/// 权限询问 HIL 处理器 (CLI 实现)
/// - 注册为 EventBus 上 service.permission 的 server
/// - 收到 ReqPermission 后, 在终端询问用户 allow/deny
/// - 返回 RespPermission 决策
/// - 权限策略判定留在 PermissionMiddlewareHandle 栈内 (规则匹配),
///   仅当策略为 INTERRUPT 时走总线询问, 实现策略/机制分离
class CliPermissionPrompter {
public:
  std::weak_ptr<AgentContext> agentContext;
  size_t serverId = 0;
  bool registered = false;

  explicit CliPermissionPrompter(std::weak_ptr<AgentContext> ctx)
      : agentContext(std::move(ctx)) {}

  asio::awaitable<void> start() {
    if (registered) {
      co_return;
    }
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->bus) {
      XX_LOGE("CliPermissionPrompter: AgentContext or bus is null");
      co_return;
    }
    auto &rr =
        ctxPtr->bus->getRR<events::ReqPermission, events::RespPermission>(
            events::Topic::Permission);
    serverId = rr.serve(
        [this](const events::ReqPermission &req,
               size_t /*corrId*/) -> asio::awaitable<events::RespPermission> {
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
      auto &rr =
          ctxPtr->bus->getRR<events::ReqPermission, events::RespPermission>(
              events::Topic::Permission);
      rr.removeServer(serverId);
    }
    registered = false;
  }

  ~CliPermissionPrompter() { stop(); }

private:
  asio::awaitable<events::RespPermission>
  handle(const events::ReqPermission &req) {
    // CLI: 在终端展示请求并读取 y/n
    std::cout << fmt::format(R"(
┏━━━━━━ Permission Request ━━━━━━┓
┣━ Tool    : {}
┣━ Category: {}
┣━ Target  : {}
┣━ Allow? [y/N]: )",
                             req.toolName, req.category, req.target)
              << std::flush;

    // 异步读取 stdin, 不阻塞 io_context (经 StdinReader 独立线程)
    auto &stdinReader =
        StdinReader::instance(co_await asio::this_coro::executor);
    auto lineOpt = co_await stdinReader.readLine();
    if (!lineOpt.has_value()) {
      // 无输入流 (非交互环境/EOF), 默认拒绝
      co_return events::RespPermission{
          .decision = events::RespPermission::Decision::Deny,
          .reason = "no interactive input",
      };
    }
    auto line = std::move(lineOpt.value());
    agentxx::util::toLowerSelf(line);
    bool allow = (line == "y" || line == "yes");
    co_return events::RespPermission{
        .decision = allow ? events::RespPermission::Decision::Allow
                          : events::RespPermission::Decision::Deny,
        .reason = allow ? "" : "user denied",
    };
  }
};

} // namespace agent
} // namespace agentxx
