#pragma once

#include "agentxx/agent/config.h"
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace agentxx {
namespace middleware {
class MiddlewareContext;
class PermissionMiddlewareHandle;
class EventBus;
} // namespace middleware

namespace tools {
class SubAgentManagerTool;
}

namespace agent {

class AgentIOBase;

class AgentContext {
public:
  std::shared_ptr<agentxx::agent::AgentConfig> agentConfig = nullptr;
  std::shared_ptr<agentxx::middleware::MiddlewareContext>
      middlewareHandleContext = nullptr;
  std::shared_ptr<agentxx::middleware::PermissionMiddlewareHandle>
      permissionMiddleware = nullptr;
  agentxx::tools::SubAgentManagerTool *subagentManagerToolPtr = nullptr;
  /// 事件总线
  /// - 由 DeepAgent 在 init() 中创建并注入; 节点/middleware/tool 经
  ///   weak_ptr<AgentContext> 取用
  /// - 完整定义在使用点 (deepagent.h) 引入
  std::shared_ptr<agentxx::middleware::EventBus> bus = nullptr;
  /// 当前轮次的 IO
  /// - 由 runConversationTurnAsync 在每轮开始时设置
  /// - 中断处理/权限询问/subagent 等经此统一输入输出
  std::shared_ptr<AgentIOBase> io = nullptr;
};

} // namespace agent
} // namespace agentxx