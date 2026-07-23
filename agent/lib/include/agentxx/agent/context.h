#pragma once

#include "agentxx/agent/config.h"
#include <atomic>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agentxx::middleware {
class EventBus;
} // namespace agentxx::middleware

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

/// 会话当前活动状态
enum class Activity : uint8_t {
    Idle,
    Streaming,     /// LLM 正在输出 token
    ExecutingTool, /// 工具正在执行
    WaitingInput,  /// 等待用户输入 (中断/权限)
};

/// 单个会话的独立状态 (按 thread_id 区分)
/// - 设计目标: 单线程/多协程交错执行多会话, 会话间状态彼此隔离
/// - io/contextStats 在 agent 线程(io_context)上访问
/// - cancelToken/modelName 可能被 UI 线程访问, 故加锁保护
class Session {
public:

    /// 本会话的 IO
    std::shared_ptr<AgentIOBase> io = nullptr;
    /// 本会话的事件总线 (会话级事件: interrupt/permission/tool 等)
    std::shared_ptr<agentxx::middleware::EventBus> bus = nullptr;
    /// 本会话的上下文统计 (内部原子, 跨线程安全)
    std::shared_ptr<ContextStats> contextStats = std::make_shared<ContextStats>();
    /// 当前活动状态 (IO 通过此字段感知状态变化)
    std::atomic<Activity> activity{Activity::Idle};

    /// 设置本会话当前轮次的取消令牌 (线程安全)
    void setCancelToken(std::shared_ptr<neograph::graph::CancelToken> token);
    /// 获取本会话当前轮次的取消令牌 (线程安全)
    std::shared_ptr<neograph::graph::CancelToken> getCancelToken();

    /// 设置本会话选择的模型名 (线程安全)
    /// - 为空表示使用 ModelProviderRegistry 的默认模型
    void setModelName(const std::string& name);
    /// 获取本会话选择的模型名 (线程安全)
    std::string getModelName() const;

private:

    mutable std::mutex                            mutex_;
    std::shared_ptr<neograph::graph::CancelToken> cancelToken_ = nullptr;
    std::string                                   modelName_;
};

/// 会话存储: 按 thread_id 取/建 Session (线程安全)
class SessionStore {
public:

    /// 获取或创建指定 thread_id 的会话
    std::shared_ptr<Session> getOrCreate(const std::string& threadId);

    /// 获取指定 thread_id 的会话; 不存在时返回 nullptr
    std::shared_ptr<Session> get(const std::string& threadId);

    void remove(const std::string& threadId);

private:

    std::mutex                                      mutex_;
    std::map<std::string, std::shared_ptr<Session>> sessions_;
};

class AgentContext {
public:

    std::shared_ptr<agentxx::agent::AgentConfig>            agentConfig                   = nullptr;
    std::shared_ptr<agentxx::middleware::MiddlewareContext> middlewareHandleContext       = nullptr;
    std::shared_ptr<agentxx::middleware::PermissionMiddlewareHandle> permissionMiddleware = nullptr;
    agentxx::tools::SubAgentManagerTool* subagentManagerToolPtr                           = nullptr;
    /// 事件总线
    /// - 由 DeepAgent 在 init() 中创建并注入; 节点/middleware/tool 经
    ///   weak_ptr<AgentContext> 取用
    /// - 完整定义在使用点 (deepagent.h) 引入
    std::shared_ptr<agentxx::middleware::EventBus> bus = nullptr;

    /// 模型 Provider 注册表 (共享)
    /// - 由 DeepAgent 在 init() 中创建并注入
    /// - 含可用模型与默认模型; 各会话的当前选择记录在 Session 中
    std::shared_ptr<ModelProviderRegistry> modelRegistry = nullptr;

    /// 会话存储: 按 thread_id 取/建 Session
    std::shared_ptr<SessionStore> sessions = std::make_shared<SessionStore>();

    /// 便捷方法: 获取或创建指定 thread_id 的会话
    std::shared_ptr<Session> getSession(const std::string& threadId);
};

} // namespace agent
} // namespace agentxx
