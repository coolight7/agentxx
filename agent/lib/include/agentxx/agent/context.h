#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/conversation_types.h"
#include "asio/thread_pool.hpp"
#include <atomic>
#include <cassert>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
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
/// - fullHistory/llmMessages/deltaSeq/chainHash/cancelToken/modelName 仅在 ioContext 线程访问
///   通过 bindIoThread() 绑定 io 线程, assertIoThread() 强制校验
///   UI 线程仅通过 getFullHistoryCopy() / getHashInfo() 等原子快照方法只读访问
/// - UI 线程的取消/切模型操作通过 Wire 消息 (WireCancel/WireSelectModel) 发往 agent 线程处理
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
    /// - 仅 ioContext 线程可写 (appendHistory), 通过 assertIoThread() 强制
    std::vector<HistoryMessage> fullHistory;
    /// LLM 上下文消息 (可压缩/裁剪, 仅用于调用 LLM API)
    /// - 仅 ioContext 线程可读写
    neograph::json llmMessages = neograph::json::array();
    /// fullHistory 的链式哈希 (用于 client 校验一致性)
    /// - 仅 ioContext 线程可写 (appendHistory 内部更新)
    ChainHash chainHash;
    /// Delta 流序号 (单调递增, 原子操作, 跨线程安全)
    std::atomic<uint64_t> deltaSeq{0};

    // -------------------------------------------------------------------
    // 线程绑定: 强制 fullHistory/llmMessages/chainHash 只在 io 线程写入
    // -------------------------------------------------------------------

    /// 绑定 io 线程 (在 BaseAgent::runConversationTurnAsync 首次使用时调用)
    /// - 仅首次调用生效, 后续调用为 no-op
    void bindIoThread() {
        auto expected = std::thread::id{};
        ioThreadId_.compare_exchange_strong(expected, std::this_thread::get_id());
    }

    /// 断言当前线程为已绑定的 io 线程
    /// - 未绑定时 (ioThreadId_ == default) 不触发, 允许初始化阶段使用
    /// - 已绑定后, 非 io 线程调用将触发 assert 失败 (Debug) / 未定义行为 (Release)
    void assertIoThread() const {
        [[maybe_unused]] auto bound = ioThreadId_.load(std::memory_order_relaxed);
        assert(
            (bound == std::thread::id{} || bound == std::this_thread::get_id())
            && "Session: mutable state (fullHistory/llmMessages/chainHash) must only be "
               "accessed on the bound io thread"
        );
    }

    // -------------------------------------------------------------------
    // 跨线程安全的只读快照接口 (UI / SessionServerAgentIO 线程可调用)
    // -------------------------------------------------------------------

    /// 获取完整历史消息副本（无锁，原子读取）
    /// - 返回的是不可变快照的拷贝, 任意线程安全
    std::vector<HistoryMessage> getFullHistoryCopy() const {
        auto snap = historySnapshot_.load(std::memory_order_acquire);
        return *snap;
    }

    /// 获取链式哈希信息（线程安全, 基于快照）
    struct HashInfo {
        size_t      count = 0;
        std::string tailHex;
    };

    HashInfo getHashInfo() const {
        auto snap = hashSnapshot_.load(std::memory_order_acquire);
        return *snap;
    }

    /// 获取 Delta 序列号（原子读取, 任意线程安全）
    uint64_t getDeltaSeq() const {
        return deltaSeq.load(std::memory_order_acquire);
    }

    // -------------------------------------------------------------------
    // io 线程专用写入接口
    // -------------------------------------------------------------------

    /// 向 fullHistory 追加一条消息并更新链式哈希，返回分配的 msgId
    /// - 必须在 ioContext 线程内调用 (assertIoThread 强制校验)
    std::string appendHistory(neograph::json msgData);

    /// 设置本会话当前轮次的取消令牌 (仅 io 线程)
    void setCancelToken(std::shared_ptr<neograph::graph::CancelToken> token);
    /// 获取本会话当前轮次的取消令牌 (仅 io 线程)
    std::shared_ptr<neograph::graph::CancelToken> getCancelToken();

    /// 设置本会话选择的模型名 (仅 io 线程)
    /// - 为空表示使用 ModelProviderRegistry 的默认模型
    void setModelName(std::string_view name);
    /// 获取本会话选择的模型名 (仅 io 线程)
    std::string getModelName() const;

private:

    std::shared_ptr<neograph::graph::CancelToken> cancelToken_ = nullptr;
    std::string                                   modelName_;
    uint64_t                                      msgIdCounter_ = 0;

    /// 绑定的 io 线程 id (std::thread::id{} 表示未绑定)
    std::atomic<std::thread::id> ioThreadId_{std::thread::id{}};

    /// 无锁快照: fullHistory (供 UI 线程只读)
    std::atomic<std::shared_ptr<const std::vector<HistoryMessage>>> historySnapshot_{
        std::make_shared<const std::vector<HistoryMessage>>()
    };

    /// 无锁快照: chainHash 信息 (供 UI 线程只读)
    std::atomic<std::shared_ptr<const HashInfo>> hashSnapshot_{std::make_shared<const HashInfo>()};
};

/// 会话存储: 按 thread_id 取/建 Session
/// - 仅在 agent io_context 线程访问, 无需锁保护
/// - UI 线程通过 Wire 消息间接操作, 不直接访问此存储
class SessionStore {
public:

    /// 获取或创建指定 thread_id 的会话
    std::shared_ptr<Session> getOrCreate(std::string_view threadId);

    /// 获取指定 thread_id 的会话; 不存在时返回 nullptr
    std::shared_ptr<Session> get(std::string_view threadId);

    void remove(std::string_view threadId);

private:

    std::map<std::string, std::shared_ptr<Session>, std::less<>> sessions_;
};

/// 加载组件信息容器
struct AgentAppendComponentInfo {
    // 已成功加载的 MCP 工具命名空间列表
    std::vector<std::string> mcpTools;
    // 成功加载的 Skill 名称列表
    std::vector<std::string> skills;
    // 成功加载的 Memory 文件路径列表
    std::vector<std::string> memoryFiles;
};

class AgentContext {
public:

    std::shared_ptr<agentxx::agent::AgentConfig>            agentConfig                   = nullptr;
    std::shared_ptr<agentxx::middleware::MiddlewareContext> middlewareHandleContext       = nullptr;
    std::shared_ptr<agentxx::middleware::PermissionMiddlewareHandle> permissionMiddleware = nullptr;
    agentxx::tools::SubAgentManagerTool* subagentManagerToolPtr                           = nullptr;
    /// 事件总线
    /// - 由 BaseAgent 在 init() 中创建并注入; 节点/middleware/tool 经
    ///   weak_ptr<AgentContext> 取用
    /// - 完整定义在使用点 (base_agent.h) 引入
    std::shared_ptr<agentxx::middleware::EventBus> bus = nullptr;

    /// 模型 Provider 注册表 (共享)
    /// - 由 BaseAgent 在 init() 中创建并注入
    /// - 含可用模型与默认模型; 各会话的当前选择记录在 Session 中
    std::shared_ptr<ModelProviderRegistry> modelRegistry = nullptr;

    /// 会话存储：按 thread_id 取/建 Session
    std::shared_ptr<SessionStore> sessions = std::make_shared<SessionStore>();

    /// 组件加载信息
    AgentAppendComponentInfo appendComponentInfo;

    /// 阻塞操作执行线程池 (文件系统遍历、glob、DNS 解析等同步阻塞操作)
    /// - 通过 agentxx::util::offloadAsync / offloadCancellableAsync 使用
    /// - 避免阻塞操作卡住 io_context 事件循环
    std::shared_ptr<asio::thread_pool> blockingPool
        = std::make_shared<asio::thread_pool>(std::max(2u, std::thread::hardware_concurrency() / 2)
        );

    /// 便捷方法：获取或创建指定 thread_id 的会话
    std::shared_ptr<Session> getSession(std::string_view threadId);

    std::string getSessionCurrentModelName(std::string_view threadId) const;
    // 可能会变，建议仅在同步代码中使用
    const ModelConfig& getSessionCurrentModelConfig(std::string_view threadId) const;
};

} // namespace agent
} // namespace agentxx
