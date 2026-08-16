#pragma once

#include "agentxx/agent/base_agent.h"
#include "agentxx/middlewares/host_events.h"
#include "asio/io_context.hpp"
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace agentxx {
namespace server {
class A2aClient;
} // namespace server

namespace agent {

/// 宿主中一个 agent 节点 (主 agent 与子代理完全平等)
/// - 每个节点持有独立的 BaseAgent 实例 (独立 AgentContext / engine /
///   SessionStore / 中间件栈), 节点间互不共享可变状态
/// - 生命周期归 AgentHost 所有: spawn 结束 / 取消 / 超时后由宿主回收
struct AgentNode {
    /// 宿主内全局唯一 id
    std::string agentId;
    /// agent 名称 (子代理为 spawn 模板名, 根为 agentName)
    std::string name;
    /// 父节点 id (空 = 根 agent)
    std::string parentAgentId;
    /// 嵌套深度 (根 = 0)
    size_t depth = 0;
    /// 独立 agent 实例
    std::shared_ptr<BaseAgent> agent;
};

/// 宿主内 agent 节点注册表
/// - 仅在宿主 io 线程访问 (单线程无锁); 跨线程访问需 post 到宿主 executor
class AgentRegistry {
public:

    std::shared_ptr<AgentNode> get(std::string_view agentId) const;
    bool                       contains(std::string_view agentId) const;
    void                       insert(std::shared_ptr<AgentNode> node);
    void                       remove(std::string_view agentId);
    size_t                     size() const;

    /// 列出以 parentAgentId 为父的全部节点
    std::vector<std::shared_ptr<AgentNode>> childrenOf(std::string_view parentAgentId) const;

    void clear();

private:

    std::map<std::string, std::shared_ptr<AgentNode>, std::less<>> nodes_;
};

/// AgentHost: 进程级 agent 宿主
/// - 所有 agent (主 agent 与子代理) 平等: 统一构造/注册/回收, 经 HostBus 交互
/// - 持有共享 ioCtx / blockingPool / HostBus (多 agent 共享基础设施, 避免
///   每个 agent 各自创建线程池)
/// - 子代理委派 (service.subagent / service.subagent.batch) 由宿主在根 agent
///   全局总线上 serve: 派生的是独立 agent (不再复用父 AgentContext/subgraph)
/// - 强制子代理嵌套深度与并发预算
/// - 生命周期: spawn 结束/取消后宿主立即回收 AgentNode (Session/中间件状态
///   随 AgentContext 析构整体释放, 无按 thread 累积泄漏)
class AgentHost : public std::enable_shared_from_this<AgentHost> {
public:

    struct Config {
        /// 宿主 io_context (空 = 自建, 调用方负责 run)
        std::shared_ptr<asio::io_context> ioCtx = nullptr;
        /// 共享 blockingPool 线程数 (0 = 默认 hardware_concurrency/2)
        size_t blockingPoolThreads = 0;
        /// 子代理最大嵌套深度 (根 = 0; 超出拒绝派生)
        size_t maxDepth = 3;
        /// 子代理最大并发数 (超出拒绝派生)
        size_t maxConcurrentSubagents = 8;
        /// agent 构造工厂 (默认 CodeAgent; 可注入自定义 BaseAgent 子类)
        std::function<std::shared_ptr<BaseAgent>(std::shared_ptr<AgentConfig>)> agentFactory;
    };

    /// 创建宿主 (须经 shared_ptr 持有: 总线 handler 以 weak_ptr 捕获)
    static std::shared_ptr<AgentHost> create(Config cfg = {});

    ~AgentHost();

    std::shared_ptr<asio::io_context>             ioCtx();
    std::shared_ptr<agentxx::middleware::EventBus> hostBus();
    std::shared_ptr<asio::thread_pool>             blockingPool();
    AgentRegistry&                                 registry();

    /// 注册根 agent (主 agent):
    /// - 注入共享 blockingPool 与宿主引用 (AgentContext::host)
    /// - 在根 agent 全局总线上 serve service.subagent / service.subagent.batch
    ///   (子代理委派的中断路径经根 agent 总线到达宿主, 由宿主派生独立 agent)
    /// - 仅支持单根 (进程级宿主)
    void attachRoot(std::shared_ptr<BaseAgent> rootAgent);

    /// 派生一个子代理并等待其完成 (任意 agent 可调用; 宿主强制深度/并发预算)
    /// - 子代理为独立 agent: 独立 AgentContext / engine / SessionStore
    /// - HIL (权限/中断) 冒泡: 子代理会话继承父会话的 io 与总线
    /// - 取消令牌透传: 父取消级联中止子代理
    asio::awaitable<events::RespSubagentResult> spawnSubagent(
        std::string_view                                   subagentName,
        std::string_view                                   systemPrompt,
        std::string_view                                   message,
        std::string_view                                   parentThreadId,
        std::shared_ptr<neograph::graph::CancelToken>      cancelToken = nullptr
    );

    /// 批量派生并等待全部完成 (结果顺序与输入一致)
    asio::awaitable<events::RespSubagentBatch> spawnBatch(const events::ReqSubagentBatch& req);

    /// 跨 agent 消息 (hostBus agent.message RR)
    /// - 目标 agent 需注册 mailbox 或注册为远程 agent (A2A 桥接);
    ///   否则返回明确的 not-implemented 错误
    asio::awaitable<events::RespHostMessage> sendMessage(events::ReqHostMessage req);

    /// 注册 agent 消息信箱 (扩展点: 持久会话 agent / 远程 A2A 桥接挂接)
    using Mailbox = std::function<asio::awaitable<events::RespHostMessage>(
        const events::ReqHostMessage&)>;
    void setMailbox(std::string_view agentId, Mailbox mailbox);

    /// 注册远程 agent (A2A 桥接): sendMessage 目标不在本地 mailbox 时,
    /// 经 A2A 协议 (SendMessage + GetTask 轮询) 转发并等待终态
    /// - 本地 agent 与远程 agent 在消息面完全同构 (agent.message RR 统一路由)
    void registerRemoteAgent(std::string_view agentId, std::shared_ptr<agentxx::server::A2aClient> client);

    /// 注销远程 agent
    void unregisterRemoteAgent(std::string_view agentId);

    /// 移除 agent 节点 (递归移除其全部子节点), 释放独立 AgentContext
    void destroyAgent(std::string_view agentId);

    /// 当前运行中的子代理数量
    size_t runningSubagents() const;

    /// 根 agent (attachRoot 设置; 未注册为空)
    std::shared_ptr<BaseAgent> rootAgent() const;

private:

    explicit AgentHost(Config cfg);

    std::shared_ptr<BaseAgent> createAgentInstance(std::shared_ptr<AgentConfig> config);
    /// 从父配置派生轻量子代理配置 (不重复建连/不持久化/独立模型)
    std::shared_ptr<AgentConfig> makeSubagentConfig(std::shared_ptr<AgentConfig> parentConfig) const;
    /// 经 A2A 协议向远程 agent 发送消息并等待终态 (轮询 GetTask)
    asio::awaitable<events::RespHostMessage>
        sendViaA2a(std::shared_ptr<agentxx::server::A2aClient> client, const events::ReqHostMessage& req);
    std::string nextAgentId();
    void        publishProgress(
        std::string_view agentId,
        std::string_view parentAgentId,
        std::string_view kind,
        std::string_view data
    );

    Config                                     cfg_;
    std::shared_ptr<asio::io_context>          ioCtx_;
    std::shared_ptr<asio::thread_pool>         blockingPool_;
    std::shared_ptr<agentxx::middleware::EventBus> hostBus_;
    AgentRegistry                              registry_;
    std::shared_ptr<BaseAgent>                 rootAgent_;
    std::map<std::string, Mailbox, std::less<>> mailboxes_;
    /// 远程 agent (A2A 桥接): agentId -> A2A 客户端
    std::map<std::string, std::shared_ptr<agentxx::server::A2aClient>, std::less<>> remoteAgents_;
    /// 根 agent 全局总线上的 subagent/batch server id (attachRoot 注册)
    size_t subagentServerId_ = 0;
    size_t batchServerId_    = 0;
    /// agent id 自增序号 (单线程协作式调度, 无需原子)
    uint64_t                  agentIdSeq_ = 0;
    /// 当前运行中的子代理数 (并发预算)
    size_t                    runningSubagentCount_ = 0;
    /// subagent threadId -> 嵌套深度 (根会话不在表内, 视为 0)
    std::map<std::string, size_t, std::less<>> threadDepth_;
};

} // namespace agent
} // namespace agentxx
