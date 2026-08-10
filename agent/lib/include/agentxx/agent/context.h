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
class SessionPersistence;

/// 会话持久化回调 (由 SessionStore 创建 Session 时注入, 解耦 sqlite 依赖)
/// - 所有回调仅做"尽力而为"持久化, 内部已捕获异常并记录日志, 不中断主流程
struct SessionPersistenceHooks {
    /// 追加展示历史消息后调用 (msg 已含分配 id; msgIdCounter 为追加后计数,
    /// 供重启恢复时延续 id 分配)
    std::function<void(const ViewMessage&, uint64_t msgIdCounter)> onAppendMessage;
    /// 保存 LLM 上下文消息 (每轮对话结束时调用)
    std::function<void(const neograph::json&)> onSaveLlmMessages;
    /// 保存会话选择的模型名
    std::function<void(std::string_view)> onSaveModelName;
};

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
/// - viewMessages/llmMessages/chainHash/cancelToken/modelName 仅在 ioContext 线程读写,
///   通过 bindIoThread() 绑定 io 线程, assertIoThread() 强制校验。
///   client/UI 不直接读取, 需要时由 io 线程拷贝后经 Wire 消息 (Sync/Delta) 传输,
///   因此无需快照/锁同步
/// - deltaSeq/contextStats 为原子, 跨线程安全
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
    Activity activity = Activity::Idle;

    /// 完整历史消息 (append-only, 永不压缩, 用于 client 同步与展示)
    /// - 仅 ioContext 线程可读写 (写: appendHistory), 通过 assertIoThread() 强制
    std::vector<ViewMessage> viewMessages;
    /// LLM 上下文消息 (可压缩/裁剪, 仅用于调用 LLM API)
    /// - 仅 ioContext 线程可读写
    neograph::json llmMessages = neograph::json::array();
    /// viewMessages 的链式哈希 (用于 client 校验一致性)
    /// - 仅 ioContext 线程可读写 (appendHistory 内部更新)
    ChainHash chainHash;
    /// Delta 流序号 (单调递增, 原子操作, 跨线程安全)
    uint64_t deltaSeq = 0;

    // -------------------------------------------------------------------
    // 线程绑定: 强制 viewMessages/llmMessages/chainHash 只在 io 线程写入
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
            && "Session: mutable state (viewMessages/llmMessages/chainHash) must only be "
               "accessed on the bound io thread"
        );
    }

    // -------------------------------------------------------------------
    // 只读访问接口 (仅 io 线程调用; client 同步由 io 线程拷贝后经 Wire 传输)
    // -------------------------------------------------------------------

    /// 获取完整历史消息副本
    /// - 仅 io 线程调用 (assertIoThread 强制校验); 返回拷贝供 Sync 传输
    std::vector<ViewMessage> getFullHistoryCopy() const {
        assertIoThread();
        return viewMessages;
    }

    /// 获取链式哈希信息
    /// - 仅 io 线程调用 (assertIoThread 强制校验)
    struct HashInfo {
        size_t      count = 0;
        std::string tailHex;
    };

    HashInfo getHashInfo() const {
        assertIoThread();
        return HashInfo{chainHash.count(), chainHash.tailHex()};
    }

    // -------------------------------------------------------------------
    // io 线程专用写入接口
    // -------------------------------------------------------------------

    /// 向 viewMessages 追加一条消息并更新链式哈希，返回分配的 msgId
    /// - 必须在 ioContext 线程内调用 (assertIoThread 强制校验)
    /// - 已绑定持久化回调时同步落库 (失败仅记日志)
    std::string appendHistory(ViewMessage msg);

    /// 绑定持久化回调 (由 SessionStore 创建 Session 时注入; 测试可不绑定)
    void setPersistenceHooks(SessionPersistenceHooks hooks);

    /// 从持久化状态恢复: 重建链式哈希 (对不含 id 的消息内容, 与
    /// appendHistory 语义一致)、恢复 msgIdCounter 与模型名
    /// - 不触发持久化回调 (恢复本身不产生新的写入)
    void restore(std::vector<ViewMessage> messages, uint64_t msgIdCounter, std::string modelName);

    /// 持久化 LLM 上下文消息 (每轮对话结束时由 BaseAgent 调用)
    /// - 未绑定回调时为 no-op
    void saveLlmMessages();

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

    /// 持久化回调 (可选; 为空时不落库)
    SessionPersistenceHooks hooks_;
};

/// 会话存储: 按 thread_id 取/建 Session
/// - 仅在 agent io_context 线程访问, 无需锁保护
/// - UI 线程通过 Wire 消息间接操作, 不直接访问此存储
class SessionStore {
public:

    /// 获取或创建指定 thread_id 的会话
    /// - 创建时若已注入持久化 (persistence), 从 SQLite 恢复该 thread 的
    ///   历史消息/LLM 上下文/模型名, 并绑定持久化回调
    std::shared_ptr<Session> getOrCreate(std::string_view threadId);

    /// 获取指定 thread_id 的会话; 不存在时返回 nullptr
    std::shared_ptr<Session> get(std::string_view threadId);

    void remove(std::string_view threadId);

    /// 会话 SQLite 持久化 (由 BaseAgent 注入; 为空时不持久化)
    /// - 仅 io 线程读写, 无需锁
    std::shared_ptr<SessionPersistence> persistence = nullptr;

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

    /// 会话 SQLite 持久化 (AgentConfig::enableSessionPersistence 开启时由
    /// BaseAgent 创建并注入; 为空表示不持久化)
    /// - 数据目录: ~/.agentxx/sqlite/{threadId}/
    std::shared_ptr<SessionPersistence> sessionPersistence = nullptr;

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
