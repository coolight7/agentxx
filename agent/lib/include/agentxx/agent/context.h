#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/conversation_types.h"
#include "agentxx/util/log.h"
#include "asio/awaitable.hpp"
#include "asio/thread_pool.hpp"
#include <atomic>
#include <cassert>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace agentxx::event {
class EventBus;
} // namespace agentxx::event

namespace neograph::graph {
class CancelToken;
class GraphRegistry;
} // namespace neograph::graph

namespace agentxx {
namespace middleware {
class MiddlewareContext;
class SubagentManagerMiddlewareHandle;
} // namespace middleware

namespace plugin {
class ToolRegistry;
class PluginManager;
} // namespace plugin

namespace tools {
class SubAgentManagerTool;
}

namespace agent {

class AgentIOBase;
class ModelProviderRegistry;
class SessionStore;
class AgentHost;
/// 会话资源应用器 (Skill/Memory/MCP 扩展; 完整定义见
/// [resource_applier.h](/agent/lib/include/agentxx/agent/resource_applier.h))
class AgentResourceApplier;

/// 会话绑定的 git worktree (worktree 模式; 由 agentxx_git_worktree 工具维护)
/// - 绑定后该会话的相对路径基准、权限隔离边界均切换到 worktree
/// - 底层 git 操作封装见 [worktree.h](/agent/lib/include/agentxx/util/worktree.h),
///   工具实现见 [git_worktree.h](/agent/lib/include/agentxx/tools/git_worktree.h)
struct WorktreeBinding {
    /// worktree 名称 (目录名与自动分支名来源)
    std::string name;
    /// worktree 绝对路径 ({repoRoot}/.agentxx/agent/worktrees/{name})
    std::string path;
    /// 关联分支名 (agentxx/wt-{name})
    std::string branch;
    /// 主检出仓库根绝对路径
    std::string repoRoot;
};

/// 会话持久化回调 (由 SessionsManager 创建 Session 时注入, 解耦 sqlite 依赖)
/// - 所有回调仅做"尽力而为"持久化, 内部已捕获异常并记录日志, 不中断主流程
struct SessionStoreHooks {
    /// 追加展示历史消息后调用 (msg 已含分配 id; msgIdCounter 为追加后计数,
    /// 供重启恢复时延续 id 分配)
    std::function<void(const ViewMessage&, uint64_t msgIdCounter)> onAppendViewMessage;

    /// 更新一条已持久化历史消息后调用 (msg 已含分配 id; 如 tool 结果回填)
    std::function<void(const ViewMessage&)> onUpdateViewMessage;

    /// 保存 LLM 上下文消息 (每轮对话结束时调用)
    std::function<void(const neograph::json&)> onSaveLlmMessages;
};

/// 上下文统计 (供 UI 显示上下文占用)
/// - 由 SummarizationMiddlewareHandle 在每次 modelcall 前更新
struct ContextStats {
    /// 当前上下文占用的 token 数
    size_t contextTokens{0};

    /// 模型支持的最大 token 数
    size_t maxContextTokens{0};

    /// 当前 ModelCall 的平均生成速度 (token/s, 估算值)
    /// - 由 EventBridge 在流式期间定时更新推送; 无流式时为 0
    double tps{0.0};
};

/// 会话当前活动状态
enum class SessionActivity : uint8_t {
    Idle,
    Streaming,     /// LLM 正在输出 token
    ExecutingTool, /// 工具正在执行
    WaitingInput,  /// 等待用户输入 (中断/权限)
};

/// 单个会话的独立状态 (按 sessionId 区分)
/// - 设计目标：单线程/多协程交错执行多会话，会话间状态彼此隔离
/// - io/bus/contextStats 在 agent 线程 (io_context) 上访问，无需额外同步
/// - viewMessages/llmMessages/chainHash/cancelToken/modelName 仅在 ioContext 线程读写,
///   通过 bindIoThread() 绑定 io 线程, assertIoThread() 强制校验。
///   client/UI 不直接读取, 需要时由 io 线程拷贝后经 Wire 消息 (Sync/WireDelta) 传输,
///   因此无需快照/锁同步
/// - deltaSeq              (普通 uint64_t, 仅 io 线程递增; EventBridge 分配)
/// - contextStats          (std::atomic 字段, 跨线程安全)
/// - UI 线程的取消/切模型操作通过 Wire 消息 (WireCancel/WireSelectModel) 发往 agent 线程处理
class Session {
public:

    Session() = default;
    ~Session();

    /// 本会话的 IO
    std::shared_ptr<AgentIOBase> io = nullptr;

    /// 本会话的事件总线 (会话级事件: interrupt/permission/tool 等)
    std::shared_ptr<agentxx::event::EventBus> bus = nullptr;

    /// 本会话的上下文统计
    std::shared_ptr<ContextStats> contextStats = std::make_shared<ContextStats>();

    /// 当前活动状态 (io 通过此字段感知状态变化)
    SessionActivity activity = SessionActivity::Idle;

    /// 完整历史消息 (append-only, 永不压缩, 用于 client 同步与展示)
    /// - 仅 ioContext 线程可读写 (写: appendViewMessage), 通过 assertIoThread() 强制
    std::vector<ViewMessage> viewMessages;

    /// LLM 上下文消息 (可压缩/裁剪, 仅用于调用 LLM API)
    /// - 仅 ioContext 线程可读写
    neograph::json llmMessages = neograph::json::array();

    /// viewMessages 的链式哈希 (用于 client 校验一致性)
    /// - 仅 ioContext 线程可读写 (appendViewMessage 内部更新)
    ChainHash chainHash;

    /// WireDelta 流序号 (单调递增; 仅 io 线程读写)
    /// - 由 EventBridge / Session::nextDeltaSeq 统一分配, 服务端增量重放缓冲
    ///   依赖 seq 单调性; 除重放路径外, 新产出的 WireDelta 必须经 nextDeltaSeq 分配
    uint64_t deltaSeq = 0;

    // -------------------------------------------------------------------
    // 线程绑定: 强制 viewMessages/llmMessages/chainHash 只在 io 线程写入
    // -------------------------------------------------------------------

    /// 绑定 io 线程 (在 BaseAgent::runTurnAsync 首次使用时调用)
    /// - 仅首次调用生效, 后续调用为 no-op
    void bindIoThread() {
        auto expected = std::thread::id{};
        ioThreadId_.compare_exchange_strong(expected, std::this_thread::get_id());
    }

    /// 判断当前线程是否为已绑定的 io 线程
    /// - 未绑定时 (ioThreadId_ == default) 返回 true, 允许初始化阶段/单线程使用
    bool isIoThread() const noexcept {
        auto bound = ioThreadId_.load(std::memory_order_relaxed);
        return bound == std::thread::id{} || bound == std::this_thread::get_id();
    }

    /// 断言当前线程为已绑定的 io 线程
    /// - 未绑定时 (ioThreadId_ == default) 不触发, 允许初始化阶段使用
    /// - 已绑定后, 非 io 线程调用在 Debug 下 assert 失败, Release 下记录错误并返回
    void assertIoThread() const {
        auto bound = ioThreadId_.load(std::memory_order_relaxed);
        if (bound == std::thread::id{} || bound == std::this_thread::get_id()) {
            return;
        }
#ifndef NDEBUG
        assert(false && "Session: mutable state must only be accessed on the bound io thread");
#else
        XX_LOGE(
            "Session: mutable state accessed off io thread (bound={}, current={})",
            bound == std::thread::id{} ? std::string{"unbound"} : std::string{"bound"},
            std::this_thread::get_id() == std::thread::id{} ? std::string{"unknown"}
                                                            : std::string{"other"}
        );
#endif
    }

    // -------------------------------------------------------------------
    // 只读访问接口 (仅 io 线程调用; client 同步由 io 线程拷贝后经 Wire 传输)
    // -------------------------------------------------------------------

    /// 获取完整历史消息副本
    /// - 仅 io 线程调用 (assertIoThread 强制校验); 返回拷贝供 Sync 传输
    std::vector<ViewMessage> getFullViewMessagesCopy() const {
        assertIoThread();
        return viewMessages;
    }

    /// 历史消息总数 (仅 io 线程调用; 历史分页同步用)
    size_t viewMessageCount() const {
        assertIoThread();
        return viewMessages.size();
    }

    /// 获取 [start, end) 区间的历史消息副本 (历史分页同步用)
    /// - 仅 io 线程调用 (assertIoThread 强制校验)
    /// - start/end 自动收敛到有效区间 (end <= start 时返回空), 避免调用方
    ///   重复做边界裁剪; 相比 getFullViewMessagesCopy 避免了每页请求的
    ///   全量拷贝 (长会话分页热路径)
    std::vector<ViewMessage> getViewMessagesRange(size_t start, size_t end) const {
        assertIoThread();
        if (end > viewMessages.size()) {
            end = viewMessages.size();
        }
        if (start >= end) {
            return {};
        }
        return std::vector<ViewMessage>(viewMessages.begin() + start, viewMessages.begin() + end);
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
    std::string appendViewMessage(ViewMessage msg);

    /// 更新一条已存在的历史消息 (按 msg.id 定位) 并同步持久化
    /// - 必须在 ioContext 线程内调用 (assertIoThread 强制校验)
    /// - 不更新链式哈希 (哈希基于消息内容, 历史内容本不应变化;
    ///   tool 结果回填属于"补齐信息", 与增量 WireDelta 语义一致)
    /// - 已绑定持久化回调时同步更新库内对应行 (失败仅记日志)
    void updateViewMessage(ViewMessage msg);

    /// 绑定持久化回调 (由 SessionsManager 创建 Session 时注入; 测试可不绑定)
    void setStoreHooks(SessionStoreHooks hooks);

    /// 从持久化状态恢复: 重建链式哈希 (对不含 id 的消息内容, 与
    /// appendViewMessage 语义一致) 并恢复 msgIdCounter
    /// - 不触发持久化回调 (恢复本身不产生新的写入)
    void restore(std::vector<ViewMessage> messages, uint64_t msgIdCounter);

    /// 持久化 LLM 上下文消息 (每轮对话结束时由 BaseAgent 调用)
    /// - 未绑定回调时为 no-op
    void saveLlmMessages();

    /// 追加一批已结算 (定稿) 的 LLM 上下文消息并请求节流持久化
    /// - 由 EventBridge 在收到节点写入 messages channel 的 CHANNEL_WRITE 事件时
    ///   调用 (assistant 回复完成 / tool 结果写回 = 一条消息结算, 非流式 token 粒度)
    /// - 目的: 进程在轮次中途被杀/崩溃时, 已结算的上下文最多丢失一个节流窗口
    ///   (kPersistThrottleMs), 而非整轮
    /// - 节流: 距上次落盘 >= kPersistThrottleMs 时立即保存; 窗口内仅更新内存,
    ///   待下次结算触发或轮末 saveLlmMessages() 统一落盘
    void appendSettledLlmMessages(const neograph::json& settledMsgs);

    /// 请求节流保存当前 llmMessages (首次触发立即落盘, 窗口内合并)
    void requestSaveLlmMessages();

    /// 立即补存节流窗口内未落盘的 viewMessages 操作 (轮末统一调用)
    /// - 保证正常结束的轮次其 view 消息全部落库, 与旧有逐条即时落盘语义收敛一致;
    ///   仅进程异常退出时才可能丢失窗口内 (<3s) 的尾部消息
    void flushViewMessages();

    /// 设置本会话当前轮次的取消令牌 (仅 io 线程)
    void setCancelToken(std::shared_ptr<neograph::graph::CancelToken> token);

    /// 获取本会话当前轮次的取消令牌 (仅 io 线程)
    std::shared_ptr<neograph::graph::CancelToken> getCancelToken();

    /// 设置本会话选择的模型名 (仅 io 线程)
    /// - 为空表示使用 ModelProviderRegistry 的默认模型
    void setModelName(std::string_view name);

    /// 获取本会话选择的模型名 (仅 io 线程)
    std::string getModelName() const;

    /// 分配下一个 WireDelta 流序号 (仅 io 线程调用)
    /// - 会话级单调递增; EventBridge 与 SessionServerAgentIO 共用此入口,
    ///   保证所有新产出的 WireDelta 都分配 seq (重放缓冲依赖 seq 单调性,
    ///   未分配 seq (=0) 的 WireDelta 不会入缓冲, 断线重连增量重放会丢失)
    uint64_t nextDeltaSeq() {
        assertIoThread();
        return ++deltaSeq;
    }

    // -------------------------------------------------------------------
    // worktree 绑定 (worktree 模式; 仅 io 线程读写)
    // - path 非空表示已绑定: 该会话的相对路径解析基准/权限隔离边界切换到 worktree
    // -------------------------------------------------------------------

    /// 设置会话绑定的 worktree (仅 io 线程)
    void setWorktreeBinding(WorktreeBinding binding) {
        assertIoThread();
        worktreeBinding_ = std::move(binding);
    }

    /// 清除绑定 (如删除当前 worktree 后; 仅 io 线程)
    void clearWorktreeBinding() {
        assertIoThread();
        worktreeBinding_ = {};
    }

    /// 获取当前绑定 (仅 io 线程; path 为空 = 未绑定)
    const WorktreeBinding& getWorktreeBinding() const {
        assertIoThread();
        return worktreeBinding_;
    }

    /// 设置本会话使用的语言 (仅 io 线程; 不支持 auto, 空或 auto 回退 "en")
    void setLanguage(std::string_view lang) {
        assertIoThread();
        language_ = normalizeLanguage(lang);
    }

    /// 获取本会话使用的语言 (仅 io 线程, 为空表示未单独设置)
    std::string getLanguage() const {
        assertIoThread();
        return language_;
    }

private:

    std::shared_ptr<neograph::graph::CancelToken> cancelToken_ = nullptr;
    std::string                                   modelName_;
    std::string                                   language_;
    uint64_t                                      msgIdCounter_ = 0;

    /// worktree 绑定 (仅 io 线程读写; path 为空 = 未绑定)
    WorktreeBinding worktreeBinding_;

    /// 绑定的 io 线程 id (std::thread::id{} 表示未绑定)
    std::atomic<std::thread::id> ioThreadId_{std::thread::id{}};

    /// 持久化回调 (可选; 为空时不落库)
    SessionStoreHooks hooks_;

    // -------------------------------------------------------------------
    // 持久化节流 (仅 io 线程访问)
    // - 规则: 首次触发立即落盘; 距上次落盘 < kPersistThrottleMs 的后续触发
    //   合并 (view 压入待落盘队列 / llm 仅更新内存), 待下次触发或轮末强制
    //   补存。进程异常退出最多丢失一个窗口内的增量
    // -------------------------------------------------------------------

    /// 节流窗口 (毫秒)
    static constexpr int64_t kPersistThrottleMs = 3000;

    /// 待落盘的 viewMessages 操作 (保持 append/update 混合顺序, 回放即重放写序列)
    struct PendingViewOp {
        bool        isAppend = false;
        ViewMessage msg;
        uint64_t    counter = 0; ///< isAppend 时的 msgIdCounter (与消息同事务提交)
    };

    std::vector<PendingViewOp> pendingViewOps_;
    /// 上次 viewMessages 实际落盘时刻 (steady ms; 0 = 本进程内尚未落过)
    int64_t viewLastPersistMs_ = 0;
    /// 上次 llm 上下文实际落盘时刻 (steady ms; 0 = 本进程内尚未落过)
    int64_t llmLastSaveMs_ = 0;

    /// 压入一条待落盘 view 操作并按节流规则决定是否立即刷出
    void enqueueViewPersist(PendingViewOp op);
    /// 回放并清空待落盘队列 (更新节流时间戳)
    void flushPendingViewOps();
};

/// 会话存储: 按 sessionId 取/建 Session
/// - 仅在 agent io_context 线程访问, 无需锁保护
/// - UI 线程通过 Wire 消息间接操作, 不直接访问此存储
class SessionsManager {
public:

    /// 获取或创建指定 sessionId 的会话 (同步版本: 若未加载且有 sessionStore 则同步加载,
    /// 供非协程上下文兜底)
    std::shared_ptr<Session> getOrCreate(std::string_view sessionId);

    /// 获取或创建指定 sessionId 的会话 (异步版本: 若未加载且有 pool, 卸载 loadSession 到线程池,
    /// 避免卡住 io 线程)
    asio::awaitable<std::shared_ptr<Session>>
        getOrCreateAsync(std::string_view sessionId, asio::thread_pool* pool = nullptr);

    /// 获取指定 sessionId 的会话; 不存在时返回 nullptr
    std::shared_ptr<Session> get(std::string_view sessionId);

    void remove(std::string_view sessionId);

    /// - 会话 SQLite 持久化 (AgentConfig::enableSessionStore 开启时由
    /// BaseAgent 创建并注入; 为空表示不持久化)
    /// - 仅 io 线程读写, 无需锁
    /// - 数据目录: {dataDir}/sqlite/sessions/{sessionId}/
    std::shared_ptr<SessionStore> sessionStore = nullptr;

private:

    std::map<std::string, std::shared_ptr<Session>, std::less<>> sessions_;
};

/// 加载组件信息容器
struct AgentAppendComponentInfo {
    /// 已成功加载的 MCP 工具命名空间列表
    std::vector<std::string> mcpTools;

    /// 成功加载的 Skill 名称列表
    std::vector<std::string> skills;

    /// 成功加载的 Memory 文件路径列表
    std::vector<std::string> memoryFiles;

    /// 加载失败的组件记录 (success=false + errorMessage; 供客户端 "Failed"
    /// 组统计与弹窗详情展示)
    /// - 与上方成功列表分离: 失败项不参与运行期资源增删 (resource_applier) 管理,
    ///   仅在启动加载阶段由各加载点写入 (MCP 连接失败 / Skill 目录不存在 /
    ///   Memory 文件不存在等)
    std::vector<AppendComponentNotification> failedComponents;
};

class AgentContext {
public:

    AgentContext();
    ~AgentContext();

    std::shared_ptr<agentxx::agent::AgentConfig>            agentConfig             = nullptr;
    std::shared_ptr<agentxx::middleware::MiddlewareContext> middlewareHandleContext = nullptr;

    /// 事件总线
    /// - 由 BaseAgent 在 init() 中创建并注入; 节点/middleware/tool 经
    ///   weak_ptr<AgentContext> 取用
    /// - 完整定义在使用点 ([base_agent.h](/agent/lib/include/agentxx/agent/base_agent.h)) 引入
    std::shared_ptr<agentxx::event::EventBus> bus = nullptr;

    /// 模型 Provider 注册表 (共享)
    /// - 由 BaseAgent 在 init() 中创建并注入
    /// - 含可用模型与默认模型; 各会话的当前选择记录在 Session 中
    std::shared_ptr<ModelProviderRegistry> modelRegistry = nullptr;

    /// 会话存储：按 sessionId 取/建 Session
    std::shared_ptr<SessionsManager> sessions = std::make_shared<SessionsManager>();

    /// 组件加载信息
    AgentAppendComponentInfo appendComponentInfo;

    /// 插件工具注册表 (动态热插拔工具; ToolcallWrapNode 查找优先,
    /// ModelCallWrapNode 组装 LLM 侧工具 schema)
    /// - 由 BaseAgent::init 创建并注入
    std::shared_ptr<plugin::ToolRegistry> toolRegistry = nullptr;

    /// 本 agent 实际装配的工具名列表 (BaseAgent::init 构建执行图时填充)
    /// - 供子代理"全量继承父 agent 工具" (tools=["*"]) 使用:
    ///   AgentHost 据此填充子代理的 toolWhitelist
    std::vector<std::string> toolNames;

    /// 插件管理器 (生命周期/热插拔; 全局唯一)
    /// - 由 BaseAgent::init 创建并注入
    std::shared_ptr<plugin::PluginManager> pluginManager = nullptr;

    /// per-agent 节点注册表 (支持多 Agent 实例, 不依赖全局 NodeFactory)
    /// - 由 BaseAgent::init 创建并注入; 插件经 graph 接口表注册自定义节点类型
    /// - 非 const: 插件注册需修改; 图编译后引擎持有内部快照, 运行期不再读取
    std::shared_ptr<neograph::graph::GraphRegistry> graphRegistry = nullptr;

    /// 当前执行图 JSON 定义 (插件可经 graph 接口表查看/修改)
    /// - 初始为 BaseAgent::initGraphDefinition() 的默认图 (名称 "agentxx.default");
    ///   插件加载后可 set_graph_json 覆盖; BaseAgent 在构建 engine 前处理此值,
    ///   插件修改非法时回退默认图并记日志
    neograph::json graphDefinitionJson = neograph::json::object();

    /// 会话资源应用器 (插件向宿主贡献 Skill/Memory/MCP 的落地接口)
    /// - 由 CodeAgent::initMiddleware 构造注入 (单一具体实现,
    ///   见 [resource_applier.h](/agent/lib/include/agentxx/agent/resource_applier.h));
    ///   BaseAgent 场景无中间件 → 保持 nullptr,
    ///   此时插件的资源注册 vtable API 返回非 0 (不支持)
    /// - 插件声明式资源在 entry 成功后经 PluginManager 调 applyDecls 应用,
    ///   卸载/禁用时摘除 (所有权语义见
    ///   [resource_applier.h](/agent/lib/include/agentxx/agent/resource_applier.h))
    std::shared_ptr<AgentResourceApplier> resourceApplier = nullptr;

    /// 宿主引用 (由 AgentHost attachRoot/派生时注入; 无宿主时为空)
    /// - 节点/工具可经此感知宿主 (如查询子代理模板、发起跨 agent 消息)
    std::weak_ptr<AgentHost> host;

    /// agent 启动进度通知回调 (由客户端端点 (TUI) 注册; 无注册则为空, no-op)
    /// - 调用方: BaseAgent::init() / CodeAgent::initTools() 各启动阶段
    ///   (agent 线程, 同步调用, 不阻塞启动流程)
    /// - 语义: 报告当前正在执行的启动操作 (如 "加载 MCP server: xxx"),
    ///   供 TUI 在"启动中"banner 中逐步展示
    /// - 线程安全: 回调实现 (TUI onServerProgress) 内部自行加锁同步
    std::function<void(std::string_view)> initNotifier;

    /// 阻塞操作执行线程池 (文件系统遍历、glob、DNS 解析等同步阻塞操作)
    /// - 通过 agentxx::util::offloadAsync / offloadCancellableAsync 使用
    /// - 避免阻塞操作卡住 io_context 事件循环
    std::shared_ptr<asio::thread_pool> threadPool
        = std::make_shared<asio::thread_pool>(std::max(2u, std::thread::hardware_concurrency() / 2)
        );

    /// 便捷方法：获取或创建指定 sessionId 的会话 (同步)
    std::shared_ptr<Session> getSession(std::string_view sessionId);

    /// 便捷方法：异步获取或创建指定 sessionId 的会话 (自动利用 threadPool 卸载 SQLite 阻塞 IO)
    asio::awaitable<std::shared_ptr<Session>> getSessionAsync(std::string_view sessionId);

    /// 统一的会话工作目录取值入口 (getSessionWorkDir; 全部使用方经此取值,
    /// 不直接读进程 cwd / AgentConfig::workDir):
    /// - 会话已绑定 worktree 时返回 worktree 路径 (worktree 模式)
    /// - 否则返回会话工作目录覆写 (setSessionWorkDir 注入, 如 ACP session/new
    ///   携带的客户端 cwd) —— 各会话可绑定不同项目目录, 彼此独立
    /// - 均未设置时回退 AgentConfig::resolvedWorkDir() (yaml work_dir / 进程
    ///   cwd); 兜底语义: 未做任何配置时与旧版行为完全一致
    /// - 均不可用时返回空串 (调用方自行兜底)
    /// - 线程约束: worktree 绑定读取沿用 Session 的 io 线程约定 (插件宿主侧
    ///   经 ioCallSync 投递); 覆写表经 mutex 保护, 任意线程可读写
    std::string getSessionWorkDir(std::string_view sessionId);

    /// 设置会话工作目录覆写 (会话级独立工作目录):
    /// - absWorkDir 应为绝对路径 (相对路径/`~` 由调用方按需归一, 如 ACP 侧
    ///   经 toCurrentSystemAbsolutePath); 空串等价 clearSessionWorkDir
    /// - 供端点在会话运行前/外注入 (如 ACP session/new), 与 Session 可变状态
    ///   的 io 线程约束解耦 —— mutex 保护, 任意线程可调用
    void setSessionWorkDir(std::string_view sessionId, std::string_view absWorkDir);

    /// 清除会话工作目录覆写 (该会话回退 AgentConfig::resolvedWorkDir)
    void clearSessionWorkDir(std::string_view sessionId);

    /// 获取语言: 优先返回指定 session 的语言, 若为空回退到 agentConfig->language, 最终兜底 "en"
    std::string getLanguage(std::string_view sessionId = "") const {
        if (!sessionId.empty() && sessions) {
            auto sess = sessions->get(sessionId);
            if (sess) {
                auto l = sess->getLanguage();
                if (!l.empty()) {
                    return l;
                }
            }
        }
        if (agentConfig && !agentConfig->language.empty()) {
            return agentConfig->language;
        }
        return "en";
    }

    /// 设置语言: 不支持 auto, 空或 auto 归一化为 "en"
    /// 若 sessionId 非空则设置会话独立语言, 同时同步更新 agentConfig->language; 若 sessionId
    /// 为空则仅更新 agentConfig->language
    void setLanguage(std::string_view lang, std::string_view sessionId = "") {
        auto norm = normalizeLanguage(lang);
        if (!sessionId.empty() && sessions) {
            auto sess = sessions->get(sessionId);
            if (sess) {
                sess->setLanguage(norm);
            }
        }
        if (agentConfig) {
            agentConfig->language = norm;
        }
    }

    std::string getSessionCurrentModelName(std::string_view sessionId) const;
    // 可能会变，建议仅在同步代码中使用
    const ModelConfig& getSessionCurrentModelConfig(std::string_view sessionId) const;

private:

    /// 会话级工作目录覆写 ({sessionId → 绝对路径}; 见 getSessionWorkDir)
    /// - 与 Session 可变状态的 io 线程约束解耦: 端点线程 (如 ACP HTTP handler)
    ///   在会话运行前注入, mutex 保护跨线程访问; 不随 Session 销毁自动清理
    ///   (与 sessions_ 的常驻语义一致, 量级为活跃会话数)
    std::mutex                                      sessionWorkDirMu_;
    std::map<std::string, std::string, std::less<>> sessionWorkDirs_;
};

} // namespace agent
} // namespace agentxx
