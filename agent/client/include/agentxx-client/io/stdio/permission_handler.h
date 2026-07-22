#pragma once

#include "agentxx/middlewares/events.h"
#include "neograph/define.h"
#include <cstddef>
#include <memory>
#include <string>

namespace agentxx {
namespace agent {
class AgentContext;
}
} // namespace agentxx

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
      std::weak_ptr<agentxx::agent::AgentContext> ctx);

  asio::awaitable<void> start();

  void stop();

  ~StdioPermissionPrompter();

private:
  asio::awaitable<agentxx::events::RespPermission>
  handle(const agentxx::events::ReqPermission &req);
};
