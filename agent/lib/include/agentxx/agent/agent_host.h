#pragma once

#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/config.h"
#include "agentxx/event/event_host.h"
#include "agentxx/plugin/plugin_manager.h"
#include "asio/awaitable.hpp"
#include "asio/io_context.hpp"
#include "utilxx_base/json.h"
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx::events {
class EventBus;
} // namespace agentxx::events

namespace agentxx {
namespace protocol {
class A2aClient;
} // namespace protocol

namespace agent {

/// 一个已挂到容器上的根 (内置根或外部挂载根)
///
/// 与 `AgentNode` 的区别: `AgentNode` 是容器里的登记项 (含子代理), 本结构是
/// "这个根是怎么进来的"的记账 —— 归属者、是否内置根、子代理服务句柄, 用于
/// 按归属批量卸载与诊断。
struct MountedRoot {
    /// 根的 agentId (与 `AgentRegistry` 里的节点 id 相同)
    std::string agentId;
    /// 归属者 (插件名 / 主程序为空)
    std::string owner;
    /// 是否内置根 (进程自己那一个)
    bool primary = false;
    /// 本根全局总线上的子代理委派服务句柄 (卸载时注销)
    size_t subagentServerId = 0;
    /// 根 agent
    std::shared_ptr<BaseAgent> agent;
};

/// 挂载一个根的参数
///
/// "根的配置"是 `AgentConfig` 级的东西 (提示词/工具白名单/插件挂载策略),
/// 都在 agent 自己的上下文里 —— 本结构只管"怎么挂进容器"。
struct MountSpec {
    /// 根的 agentId
    /// - 空 = 生成 `ext:<owner>:<n>` (内置根固定用 `root`)
    std::string agentId;
    /// 归属者 (插件名; 按归属批量卸载); 空 = 主程序
    std::string owner;
    /// 是否内置根 (`attachRoot` 路径为 true)
    bool primary = false;
    /// 是否在本根总线上注册子代理委派服务
    /// - true (默认): 本根可用 `agentxx_subagent` 委派, 用量记到本根会话
    /// - false: 本根不参与子代理委派 (预算仍然共享)
    bool registerSubagentService = true;
};

/// agent 节点容器中一个 agent 节点 (主 agent 与子 agent 完全平等)
/// - 每个节点持有独立的 BaseAgent 实例 (独立 AgentContext / engine /
///   SessionStore / 中间件栈), 节点间互不共享可变状态
/// - 生命周期归 AgentHost 所有: spawn 结束 / 取消 / 超时后由 agent 节点容器回收
struct AgentNode {
    /// agent 节点容器内全局唯一 id
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

/// agent 节点容器内 agent 节点注册表
/// - 仅在 agent 节点容器 io 线程访问 (单线程无锁); 跨线程访问需 post 到 agent 节点容器 executor
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

/// AgentHost: 进程级 agent 节点容器
/// - 所有 agent (主 agent、挂载根与子代理) 平等: 统一构造/注册/回收, 经 HostBus 交互
/// - 支持**多个根**: 内置根 (`root`) + 外部挂载根 (如插件自建的"QQ 群助手")
///   各自持有独立 `AgentConfig` (提示词/工具白名单/插件挂载策略), 会话互不影响
/// - 持有共享 ioCtx / threadPool / HostBus (多 agent 共享基础设施, 避免
///   每个 agent 各自创建线程池)
/// - 子代理委派 (service.subagent / service.subagent.batch) 由本容器在各根全局总线
///   上 registerServer: 派生的是独立 agent (不再复用父 AgentContext/subgraph)
/// - 强制子代理嵌套深度与并发预算 (全局共享, 不按根拆分)
/// - 生命周期: spawn 结束/取消后立即回收 AgentNode (Session/中间件状态
///   随 AgentContext 析构整体释放, 无按 thread 累积泄漏)
class AgentHost : public std::enable_shared_from_this<AgentHost> {
public:

    struct Config {
        /// agent 节点容器 io_context (空 = 自建, 调用方负责 run)
        std::shared_ptr<asio::io_context> ioCtx = nullptr;
        /// 共享 threadPool 线程数 (0 = 默认 hardware_concurrency/2)
        size_t threadPoolWorkers = 0;
        /// 子代理最大嵌套深度 (根 = 0; 超出拒绝派生)
        size_t maxDepth = 3;
        /// 子代理最大并发数 (超出拒绝派生)
        size_t maxConcurrentSubagents = 8;
        /// 进程内根数上限 (含内置根; 超出时 [mountRoot] 返回空)
        size_t maxRoots = 8;
        /// agent 构造工厂 (默认 CodeAgent; 可注入自定义 BaseAgent 子类)
        std::function<std::shared_ptr<BaseAgent>(std::shared_ptr<AgentConfig>)> agentFactory
            = nullptr;
        /// 插件加载表 (进程一份: 主程序在 [loadPluginsAsync] 里 create/start 各一次)
        std::vector<agentxx::agent::PluginConfig> pluginList{};
        /// 主 agent 上下文 (主程序级配置来源: 总线 / 语言 / 模型 / 工作目录)
        /// - 加载阶段还没有挂载, 但插件 `start` 里的进程级注册 (订阅主程序总线等)
        ///   需要它能作答; 不传时这些注册会被拒 (记警告后继续)
        std::weak_ptr<agentxx::agent::AgentContext> primaryAgent{};
    };

    /// 创建 agent 节点容器 (须经 shared_ptr 持有: 总线 handler 以 weak_ptr 捕获)
    static std::shared_ptr<AgentHost> create(Config cfg);

    ~AgentHost();

    std::shared_ptr<asio::io_context>          ioCtx();
    std::shared_ptr<agentxx::events::EventBus> hostBus();
    std::shared_ptr<asio::thread_pool>         threadPool();
    AgentRegistry&                             registry();

    /// 本容器的插件管理器 (唯一实例; 未加载插件时为空)
    /// - 各 agent 在 `init` 前把它放进 `AgentContext::pluginManager`, 由 agent 自己
    ///   `mountAgentAsync` 挂载 (见 `BaseAgent::init` 的 `plugin_mount` 步骤)
    std::shared_ptr<agentxx::plugin::PluginManager> pluginHost() const;

    /// 加载插件加载表 (拓扑排序 + 依赖 + 内置/外置 + create + start)
    /// - 必须在 agent `init` 之前完成: 插件只加载一次, 各 agent 只挂载
    /// - 重复调用无副作用; 列表为空时直接返回
    asio::awaitable<void> loadPluginsAsync();

    /// 注册内置根 agent (主 agent)
    ///
    /// 等价于 `mountRoot(agent, {.agentId="root", .primary=true})`; 已有内置根时
    /// 只记警告并返回。
    void attachRoot(std::shared_ptr<BaseAgent> rootAgent);

    /// 把一个根挂到容器上, 返回它的 agentId (失败返回空)
    ///
    /// 步骤:
    /// 1. 预算检查 (`maxRoots`) 与 id 重复检查 (重复 id 拒绝, 不覆盖现有节点);
    /// 2. 登记会话存储根 (`(存储根) 已被别的 agent 占用 → 拒绝`);
    /// 3. 注入共享基础设施: threadPool / host / 插件管理器;
    ///    **插件管理器一律用容器这一份** (根不能自带一份, 否则又跑一份实例);
    /// 4. 登记节点 (`depth=0`, `parentAgentId` 为空);
    /// 5. 按 [MountSpec::registerSubagentService] 在根的全局总线上注册子代理委派。
    ///
    /// 注: 本函数只做"挂进容器", 不调用 `agent->init()` —— 调用方负责先 init
    /// (插件自建的根在挂载前就已 init 完成)。
    std::string mountRoot(std::shared_ptr<BaseAgent> agent, MountSpec spec);

    /// 按 agentId 卸载一个根
    /// - 先注销子代理服务与节点, 再释放 agent;
    /// - 内置根 (`primary=true`) 与不存在的 id 都返回 false (内置根不随插件卸载);
    /// - 若该根有会话存储根占用则一并释放;
    /// - 插件禁用/卸载时用 [unmountRootsByOwner] 批量卸载
    bool unmountRoot(std::string_view agentId);

    /// 按归属者批量卸载根, 返回卸载个数 (内置根与其它归属者的根不动)
    size_t unmountRootsByOwner(std::string_view owner);

    /// 按 agentId 找一个根 (不存在或不是根时返回空)
    std::shared_ptr<BaseAgent> findRoot(std::string_view agentId) const;

    /// 全部根的快照 (含内置根; 按挂载顺序)
    std::vector<MountedRoot> roots() const;

    /// 批量派生子代理并等待全部完成 (结果顺序与输入一致; 任意 agent 可调用)
    /// - 单个任务 (SubagentBatchItem) 派生一个独立 agent:
    ///   独立 AgentContext / engine / SessionStore
    /// - HIL (权限/中断) 冒泡: 子代理会话继承父会话的 io 与总线
    ///   (parentAgentCtx 指定父 agent 上下文, 从该上下文的 SessionStore 查找
    ///   父会话; 为空回退根 agent —— 根派生子代理 / agent 节点容器外部调用的默认路径)
    /// - 取消令牌原样转发: 父取消级联中止子代理
    /// - 同上下文模式 (messages 或 sessionId 非空):
    ///   - messages: 结构化消息原样转发作为子代理初始上下文 (可含 system,
    ///     优先于 message 文本)
    ///   - sessionId: 子代理运行在指定 thread (而非独立 subagent 线程),
    ///     并强制使用该 thread 父会话的当前模型 (忽略 subagentModel);
    ///     两者配合保证"相同上下文前缀 + 相同 threadid + 相同模型",
    ///     以命中 provider KV/prefix cache (如上下文压缩场景)
    /// - tools (可选, 子代理工具策略): 缺省 = 默认全部工具;
    ///   `[]` = 无工具; `["*"]` = 全部继承父 agent 工具 (解析为父工具名白名单);
    ///   `["name", ...]` = 自定义白名单
    /// - enableSummarization (可选): 子代理上下文压缩中间件开关;
    ///   缺省继承 config (父拷贝); 压缩发起的子代理必须显式 false
    asio::awaitable<events::RespSubagentBatch> spawnBatch(
        const events::ReqSubagentBatch& req,
        std::shared_ptr<AgentContext>   parentAgentCtx = nullptr
    );

    /// 跨 agent 消息 (hostBus agent.message RR)
    /// - 目标 agent 需注册 mailbox 或注册为远程 agent (A2A 桥接);
    ///   否则返回明确的 not-implemented 错误
    asio::awaitable<events::RespHostMessage> sendMessage(events::ReqHostMessage req);

    /// 注册 agent 消息信箱 (扩展点: 持久会话 agent / 远程 A2A 桥接挂接)
    using Mailbox
        = std::function<asio::awaitable<events::RespHostMessage>(const events::ReqHostMessage&)>;
    void setMailbox(std::string_view agentId, Mailbox mailbox);

    /// 注册远程 agent (A2A 桥接): sendMessage 目标不在本地 mailbox 时,
    /// 经 A2A 协议 (SendMessage + GetTask 轮询) 转发并等待终态
    /// - 本地 agent 与远程 agent 在消息面完全同构 (agent.message RR 统一路由)
    void registerRemoteAgent(
        std::string_view                              agentId,
        std::shared_ptr<agentxx::protocol::A2aClient> client
    );

    /// 注销远程 agent
    void unregisterRemoteAgent(std::string_view agentId);

    /// 移除 agent 节点 (递归移除其全部子节点), 释放独立 AgentContext
    void destroyAgent(std::string_view agentId);

    /// 等待 agent 所属 IO executor 完成插件关闭后再移除节点。
    asio::awaitable<bool> destroyAgentAsync(
        std::string_view          agentId,
        std::chrono::milliseconds timeout = std::chrono::seconds{30}
    );

    /// 当前运行中的子代理数量
    size_t runningSubagents() const;

    // =====================================================================
    // 会话存储根占用 (同一份磁盘会话只能由一个 agent 写)
    // =====================================================================
    //
    // 会话以 (存储根, sessionId) 定位: 不同存储根可以同名, 同一存储根必须独占
    // (进程内的会话写租约是可重入的, 撞了不会报错, 只会静默串数据)。因此 agent
    // 开始用某个存储根前先登记, 撞上就拒绝, 早失败而不是等写库时才发现。

    /// 登记一个 agent 对规范化会话存储根独占
    /// - 路径先规范化 (绝对路径 + 消解符号链接; Windows 统一小写) 再比较;
    /// - agent 没有会话存储 (子代理默认) 或路径为空: 直接通过 (不占用);
    /// - 同一存储根已属于别的 agent: 记 ERROR 并返回 false (调用方应拒绝该 agent)
    bool claimSessionStoreRoot(agentxx::agent::AgentContext& ctx, std::string_view agentId);

    /// 释放某个 agent 的会话存储根占用 (agent 关闭时)
    void releaseSessionStoreRoot(std::string_view agentId);

    /// 已被占用的会话存储根 (规范化路径 -> agentId; 诊断用)
    utilxx_base::Json sessionStoreOwnersJson() const;

    // =====================================================================
    // 会话占用 (同一份磁盘会话只能有一个驱动者)
    // =====================================================================
    //
    // 会话以 (存储根, sessionId) 定位。上面那个登记表按**存储根**独占, 防的是
    // "两个 agent 开同一份目录"; 这里再按 **(存储根, sessionId)** 独占, 防的是
    // "两个通道同时驱动同一个会话" —— 进程内的会话写租约是可重入的, 両者会静默互盖。
    //
    // 占用表挂在进程唯一的 AgentHost 上 (不能按插件实例建): 不同插件实例之间同样要互斥。

    /// 占用一个会话, 成功返回 true
    /// - `owner` 只用于日志/诊断 (写清是哪个调用方占的)
    /// - 同一个 owner 重复占同一会话视为成功 (幂等)
    /// - 任意线程可调用 (内部加锁)
    bool claimSession(std::string_view storeRoot, std::string_view sessionId, std::string_view owner);

    /// 释放一个会话
    /// - 只有占用者本人能释放 (不一致时拒绝并记警告)
    void
        releaseSession(std::string_view storeRoot, std::string_view sessionId, std::string_view owner);

    /// 当前被占用的会话 (规范化 `存储根|sessionId` -> owner; 诊断用)
    utilxx_base::Json sessionOccupancyJson() const;

    /// 根 agent (内置根; 未注册为空)
    std::shared_ptr<BaseAgent> rootAgent() const;

    /// 当前根数 (含内置根; 诊断用)
    size_t rootCount() const {
        return roots_.size();
    }

private:

    explicit AgentHost(Config cfg);

    std::shared_ptr<BaseAgent> createAgentInstance(std::shared_ptr<AgentConfig> config);
    /// 从父配置派生轻量子代理配置 (不重复建连/不持久化/独立模型)
    std::shared_ptr<AgentConfig> makeSubagentConfig(std::shared_ptr<AgentConfig> parentConfig
    ) const;

    /// 注入共享基础设施与插件管理器 (根与子代理共用)
    /// - threadPool / host 引用 / 插件管理器 (进程唯一那一份)
    /// - 插件挂载策略为 None 时不注入管理器
    void injectShared(std::shared_ptr<agentxx::agent::AgentContext> ctx);

    /// 在指定根的全局总线上注册子代理委派服务
    /// - 返回服务句柄 (0 = 注册失败/根的全局总线不存在)
    /// - 闭包捕获该根的上下文: 额度记账/工具继承/HIL 派发都落到那一个根
    size_t registerSubagentService(std::shared_ptr<BaseAgent> rootAgent);

    /// 注销某个根的子代理委派服务 (根卸载/销毁时; 幂等)
    void unregisterSubagentService(std::string_view agentId);

    /// 派生单个子代理并等待其完成 (spawnBatch 内部按任务逐个调用)
    /// - 深度/并发预算检查, 独立 agent 构造, 运行边界 RAII 回收
    /// - 派生时在该子代理全局总线上 registerServer service.subagent (agent 节点容器应答,
    ///   与 attachRoot 对称): 子代理作用域内再遇 subagent 委派中断,
    ///   经本总线请求递归处理, 与根委派完全同路径 (扁平化)
    /// - HIL 类中断 (权限询问等) 经子代理会话总线冒泡到父 IO:
    ///   parentAgentCtx 指定父 agent 上下文 (从该上下文的 SessionStore
    ///   查找父会话), 嵌套时是上一级子代理而非根
    asio::awaitable<events::RespSubagentBatchItem> spawnOneTask(
        const events::SubagentBatchItem&              task,
        std::string_view                              parentSessionId,
        std::shared_ptr<neograph::graph::CancelToken> cancelToken,
        std::shared_ptr<AgentContext>                 parentAgentCtx = nullptr
    );
    /// 经 A2A 协议向远程 agent 发送消息并等待终态 (轮询 GetTask)
    asio::awaitable<events::RespHostMessage> sendViaA2a(
        std::shared_ptr<agentxx::protocol::A2aClient> client,
        const events::ReqHostMessage&                 req
    );
    std::string nextAgentId();
    void        publishProgress(
               std::string_view agentId,
               std::string_view parentAgentId,
               std::string_view kind,
               std::string_view data
           );

    /// 按 agentId 找已挂载的根 (内部使用; 不加锁)
    MountedRoot* findMountedRoot(std::string_view agentId);

    Config                                      cfg_;
    std::shared_ptr<asio::io_context>           ioCtx_;
    std::shared_ptr<asio::thread_pool>          threadPool_;
    std::shared_ptr<agentxx::events::EventBus>  hostBus_;
    AgentRegistry                               registry_;
    /// 插件管理器 (进程一份; 由 [loadPluginsAsync] 建立)
    /// - 声明在 roots_ 之前: 成员逆序析构, 这样 agent 先释放 (拆挂载) 再关管理器
    std::shared_ptr<agentxx::plugin::PluginManager> pluginHost_;
    /// 已挂载的根 (含内置根; 按挂载顺序)
    std::vector<MountedRoot> roots_;
    /// 内置根 (便于频繁读取不搜表)
    std::shared_ptr<BaseAgent> rootAgent_;
    std::map<std::string, Mailbox, std::less<>> mailboxes_;
    /// 远程 agent (A2A 桥接): agentId -> A2A 客户端
    std::map<std::string, std::shared_ptr<agentxx::protocol::A2aClient>, std::less<>> remoteAgents_;
    /// agent id 自增序号 (单线程协作式调度, 无需原子)
    uint64_t agentIdSeq_ = 0;
    /// 挂载根自增序号 (生成 `ext:<owner>:<n>`)
    uint64_t rootSeq_ = 0;
    /// 当前运行中的子代理数 (并发预算)
    size_t runningSubagentCount_ = 0;
    /// subagent sessionId -> 嵌套深度 (根会话不在表内, 视为 0)
    std::map<std::string, size_t, std::less<>> sessionDepth_;
    /// 已占用的会话存储根 (规范化路径 -> agentId)
    std::map<std::string, std::string, std::less<>> storeRoots_;
    /// 已占用的会话 (规范化 `存储根|sessionId` -> owner; 跨插件实例, 需锁)
    mutable std::mutex                                  sessionOccupancyMutex_;
    std::map<std::string, std::string, std::less<>> sessionOccupancy_;
};

} // namespace agent
} // namespace agentxx
