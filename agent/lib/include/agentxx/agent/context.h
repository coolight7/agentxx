#pragma once

#include "agentxx/agent/config.h"
#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace neograph::graph {
class CancelToken;
}

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
class ModelProviderRegistry;

/// 上下文统计 (供 UI 显示上下文占用)
/// - 由 SummarizationMiddlewareHandle 在每次 modelcall 前更新
struct ContextStats {
  /// 当前上下文占用的 token 数
  std::atomic<size_t> contextTokens{0};
  /// 模型支持的最大 token 数
  std::atomic<size_t> maxContextTokens{0};
};

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

  /// 模型 Provider 注册表
  /// - 由 DeepAgent 在 init() 中创建并注入
  /// - 支持运行时切换 modelcall 使用的模型
  std::shared_ptr<ModelProviderRegistry> modelRegistry = nullptr;

  /// 上下文统计; 供 UI 显示上下文占用百分比
  std::shared_ptr<ContextStats> contextStats = std::make_shared<ContextStats>();

  /// 设置当前运行轮次的取消令牌 (线程安全)
  /// - 由 runConversationTurnAsync 在每轮开始时设置
  void setCancelToken(std::shared_ptr<neograph::graph::CancelToken> token) {
    std::lock_guard<std::mutex> lock(cancelTokenMutex_);
    cancelToken_ = std::move(token);
  }

  /// 获取当前运行轮次的取消令牌 (线程安全)
  /// - UI 经此取消正在执行的轮次
  std::shared_ptr<neograph::graph::CancelToken> getCancelToken() {
    std::lock_guard<std::mutex> lock(cancelTokenMutex_);
    return cancelToken_;
  }

private:
  std::mutex cancelTokenMutex_;
  std::shared_ptr<neograph::graph::CancelToken> cancelToken_ = nullptr;
};

} // namespace agent
} // namespace agentxx