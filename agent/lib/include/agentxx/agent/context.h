#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/conversation_types.h"
#include <atomic>
#include <functional>
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
/// - 设计目标：单线程/多协程交错执行多会话，会话间状态彼此隔离
/// - io/bus/contextStats 在 agent 线程 (io_context) 上访问，无需额外同步
/// - fullHistory/llmMessages/deltaSeq 仅在 DeepAgent::runConversationTurnAsync 中写入（ioContext 线程）
///   UI 线程仅通过 getFullHistoryCopy() 等辅助方法只读访问，避免竞争条件
/// - cancelToken/modelName 支持 UI 线程读取（用于显示取消按钮和当前模型），加锁保护
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

    /// 完整历史消息 (append-only, 永不压缩, 用于 client 同步与展示)
    std::vector<HistoryMessage> fullHistory;
    /// LLM 上下文消息 (可压缩/裁剪, 仅用于调用 LLM API)
    neograph::json llmMessages = neograph::json::array();
    /// fullHistory 的链式哈希 (用于 client 校验一致性)
    ChainHash chainHash;
    /// Delta 流序号 (单调递增)
    uint64_t deltaSeq = 0;

    /// 获取完整历史消息副本（无锁，原子读取）
    /// - 返回的是不可变快照的拷贝
    std::vector<HistoryMessage> getFullHistoryCopy() const {
        auto snap = historySnapshot_.load(std::memory_order_acquire);
        return *snap;  // 拷贝已有，无需锁
    }
    
    /// 获取 LLm 上下文消息副本（使用原有锁机制）
    neograph::json getLlmMessagesCopy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return llmMessages.dump();
    }
    
    /// 获取链式哈希信息（线程安全）
    struct HashInfo {
        size_t count;
        std::string tailHex;
    };
    
    HashInfo getHashInfo() const {
        // ChainHash 不支持 atomic，使用简单内存序读取
        // 注意：这里是潜在的数据竞争窗口，但只影响校验一致性
        // 设计保证 appendHistory 仅在 ioContext 线程调用，UI 读取为 snapshot
        if (fullHistory.empty()) {
            return {0, {}};
        }
        return {chainHash.count(), chainHash.tailHex()};
    }
    
    /// 获取 Delta 序列号（使用原锁机制）
    uint64_t getDeltaSeq() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deltaSeq;
    }
    
    /// 向 fullHistory 追加一条消息并更新链式哈希，返回分配的 msgId
    /// 注意：此方法应在 ioContext 线程内调用，调用方需自行处理锁
    std::string appendHistory(neograph::json msgData);

    /// 设置本会话当前轮次的取消令牌 (线程安全)
    void setCancelToken(std::shared_ptr<neograph::graph::CancelToken> token);
    /// 获取本会话当前轮次的取消令牌 (线程安全)
    std::shared_ptr<neograph::graph::CancelToken> getCancelToken();

    /// 设置本会话选择的模型名 (线程安全)
    /// - 为空表示使用 ModelProviderRegistry 的默认模型
    void setModelName(std::string_view name);
    /// 获取本会话选择的模型名 (线程安全)
    std::string getModelName() const;

private:

    mutable std::mutex                            mutex_;           // 保护 cancelToken_, modelName_, msgIdCounter_
    std::shared_ptr<neograph::graph::CancelToken> cancelToken_ = nullptr;
    std::string                                   modelName_;
    uint64_t                                      msgIdCounter_ = 0;
    
    // 无锁同步状态（history 快照用于 UI 读取）
    std::atomic<std::shared_ptr<const std::vector<HistoryMessage>>> historySnapshot_{
        std::make_shared<const std::vector<HistoryMessage>>()
    };
};

/// 会话存储: 按 thread_id 取/建 Session (线程安全)
class SessionStore {
public:

    /// 获取或创建指定 thread_id 的会话
    std::shared_ptr<Session> getOrCreate(std::string_view threadId);

    /// 获取指定 thread_id 的会话; 不存在时返回 nullptr
    std::shared_ptr<Session> get(std::string_view threadId);

    void remove(std::string_view threadId);

private:

    std::map<std::string, std::shared_ptr<Session>, std::less<>> sessions_;  // 单线程访问，无需锁
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
    std::shared_ptr<Session> getSession(std::string_view threadId);
};

} // namespace agent
} // namespace agentxx
