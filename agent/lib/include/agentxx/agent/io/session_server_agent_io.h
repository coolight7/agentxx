#pragma once

#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "asio/steady_timer.hpp"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neograph::graph {
class CancelToken;
}

namespace agentxx {

namespace events {
class EventBus;
}

namespace middleware {
class PermissionMiddlewareHandle;
}

namespace agent {

class BaseAgent;
class Session;

/// 按 sessionId 持久的会话控制器 (服务端 AgentIOBase 端点)
///
/// 数据流: BaseAgent → SessionServerAgentIO → 1:N transports → 客户端 AgentIOBase
///         客户端 AgentIOBase → transport → SessionServerAgentIO → BaseAgent
///
/// - 作为 AgentIOBase 被 BaseAgent 驱动; 驱动循环独立于连接存在
/// - 一对多支持: 支持多个客户端 (多个 UI / TUI / CLI) 同时连接同一个会话控制器
///   - 广播: 会话实时增量 (WireDelta)、轮次结果 (WireTurnResult)、上下文统计 (WireContextStats)、
///     消息队列更新 (WireMessageQueueUpdate)、中断询问 (WireInterruptRequest)、
///     中断结束 (WireInterruptExpired)、插件数据 (WirePluginData) 面向所有客户端广播
///   - 单播: 客户端握手重放/初始化 (HelloAck/Replay)、历史分页请求 (WireGetViewMessages)、
///     会话列表 (WireListSessions)、模型查询等仅发回给发起请求的具体客户端 transport
/// - 持有 delta 环形缓冲, 供重连时增量重放 (seq 连续则重放, 否则回退全量 sync)
/// - 多客户端断开与宽限期: 只要有至少一个客户端在线，就不会触发宽限期;
///   仅当所有客户端均断开且轮次活动时才进入 gracePeriod 宽限期; 期间任意客户端接入即取消宽限期
/// - 线程模型: 所有成员状态 (clients_/deltaBuffer_/pending_/graceTimer_) 仅在 ex_ 线程访问，
///   无需锁保护; stop() 通过 asio::dispatch(ex_) 保证在 ex_ 线程执行清理
class SessionServerAgentIO : public AgentIOBase,
                             public std::enable_shared_from_this<SessionServerAgentIO> {
public:

    struct Config {
        std::string sessionId = "session";
        /// 中断/权限等待客户端响应的超时; <=0 表示不限制 (无限等待用户响应)
        std::chrono::milliseconds interruptTimeout = std::chrono::milliseconds{0};
        /// 断线后保持运行中轮次的宽限期; <=0 表示断线立即取消轮次
        std::chrono::milliseconds gracePeriod = std::chrono::seconds{30};
        /// delta 环形缓冲容量 (按消息数)
        size_t deltaBufferCap = 4096;
        /// 首次接入/切换会话时同步的历史消息窗口大小 (历史分页)
        /// - 0 = 全量同步 (旧行为)
        /// - N > 0 = 仅同步末尾 N 条 (fromIndex=窗口起始下标), 客户端
        ///   (TUI) 向上滚动到窗口顶部时经 WireGetViewMessages 分页拉取
        ///   更早历史, 避免长会话恢复时全量传输
        size_t initialSyncTailCount = 0;
        /// `collect` 投递模式的静默合并窗口 (计划 LOOP-11)
        /// - 窗口内同一客户端的连续输入合并为一条, 窗口结束后按 next-turn 提交
        /// - <=0 表示不合并 (collect 等同 next-turn)
        std::chrono::milliseconds collectWindow = std::chrono::milliseconds{400};
    };

    SessionServerAgentIO(asio::any_io_executor ex, std::weak_ptr<BaseAgent> agent, Config config);

    ~SessionServerAgentIO() override;

    // ----- 多客户端 Transport 管理 (一对多 1:N) -----

    /// 设置默认传输层 (覆写 AgentIOBase: 添加到客户端列表并设为当前 transport)
    void setTransport(std::shared_ptr<AgentIOTransportBase> transport) override;

    /// 接入一个新的客户端传输层
    void attachClient(std::shared_ptr<AgentIOTransportBase> transport);

    /// 移除一个已断开的客户端传输层
    void detachClient(const std::shared_ptr<AgentIOTransportBase>& transport);

    /// 获取当前已连接客户端数量
    size_t clientCount() const noexcept;

    /// 是否存在存活 (alive) 的客户端传输
    bool hasAliveClient() const noexcept;

    /// 获取当前所有客户端传输列表快照
    std::vector<std::shared_ptr<AgentIOTransportBase>> clients() const;

    /// 广播消息到所有活跃的客户端
    void broadcastToClients(const WireMessage& msg);

    /// 定向发送消息给特定客户端
    void sendToClient(const std::shared_ptr<AgentIOTransportBase>& transport, WireMessage msg);

    // ----- AgentIOBase: 主动发送 (覆写: 新产出的 WireDelta 先写入重放缓冲再广播给所有客户端) -----
    void sendToPeer(WireMessage msg) override;

    // ----- AgentIOBase: 传输接收循环 -----
    /// 针对基类 transport 运行接收循环 (兼容原有单连接用法)
    asio::awaitable<void> runTransportLoop() override;

    /// 针对特定客户端传输层运行接收循环 (多连接并发使用)
    asio::awaitable<void> runTransportLoop(std::shared_ptr<AgentIOTransportBase> transport);

    // ----- AgentIOBase: 对端从我这拉取的 (BaseAgent 调用) -----
    asio::awaitable<std::optional<std::string>> getInput() override;
    asio::awaitable<utilxx_base::Json>          handleInterrupt(
                 std::string_view sessionId,
                 std::string_view interruptNode,
                 std::string_view interruptValue,
                 std::string_view interruptArgJson
             ) override;

    // ----- AgentIOBase: 对端发来的消息分发 -----
    void onPeerMessage(WireMessage msg) override;
    void onPeerMessage(WireMessage msg, const std::shared_ptr<AgentIOTransportBase>& sender);

    // ----- 生命周期 -----

    /// 驱动循环: 取输入 -> 执行对话轮次 -> 推送结果; 由 AgentServer 创建控制器时 co_spawn 一次
    asio::awaitable<void> run();

    /// 停止驱动循环 (关闭输入 channel/取消轮次/失败 pending)
    void stop();

    // ----- 连接管理 -----

    /// 处理客户端 hello: 按需重放 delta 或全量 sync, 发送 helloAck (仅定向回复给 sender 客户端)
    void handleHello(
        const WireHello&                             hello,
        std::vector<std::string>                     models = {},
        const std::shared_ptr<AgentIOTransportBase>& sender = nullptr
    );

    /// 传输断开时调用: 若指定 transport 则仅移除该客户端; 仅当无存活客户端且轮次进行中才启动 grace
    /// 定时器
    void onDisconnect(const std::shared_ptr<AgentIOTransportBase>& transport = nullptr);

    // ----- 查询 -----

    std::string_view sessionId() const noexcept {
        return config_.sessionId;
    }

    /// 中断等待超时 (供 BaseAgent 中断请求显式传递, 避免被总线默认超时截断)
    std::chrono::milliseconds interruptTimeout() const noexcept {
        return config_.interruptTimeout;
    }

    bool turnActive() const noexcept {
        return turnActive_.load(std::memory_order_acquire);
    }

    /// 驱动循环 run() 是否仍在运行 (用于停止时等待其退出)
    bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    /// 测试辅助: 强制设置轮次活动状态
    void setTurnActiveForTest(bool v) noexcept {
        turnActive_.store(v, std::memory_order_release);
    }

    /// 测试辅助: 获取消息队列是否处于暂停态
    bool isQueuePausedForTest() const noexcept {
        return queueState_ == SessionQueueState::Paused;
    }

    /// 当前消息队列状态 (计划 LOOP-4)
    SessionQueueState queueState() const noexcept {
        return queueState_;
    }

    /// 测试辅助: 获取当前消息队列大小
    size_t queueSizeForTest() const noexcept {
        return messageQueue_.size();
    }

    /// 当前 viewMessages 的链式哈希尾 (供 hello_ack/sync)
    std::string currentTailHash();

    /// 切换本端点绑定的会话 (会话选择弹窗确认后由客户端经 WireSwitchSession 请求)
    /// - 重新绑定 config_.sessionId 到目标会话 (不存在时由 SessionStore 从持久化恢复创建)
    /// - 清空 delta 重放缓冲 (新会话 delta seq 独立编号)
    /// - 回推新会话的全量 Sync + 模型信息 + 上下文统计, 客户端据此恢复界面
    /// - 仅当无进行中轮次时生效 (客户端已做前置拦截, 此处双重保护)
    void switchSession(std::string newSessionId);

protected:

    // ----- AgentIOBase: 被动接收回调 (server 端点不会从 client 收到这些消息,
    //       空实现仅用于满足纯虚契约) -----
    void onDelta(const WireDelta& delta) override;
    void onSync(const WireSyncPayload& payload) override;

private:

    using ErrorCode = utilxx_base::AsioErrorCode;

    struct PendingInterrupt {
        std::shared_ptr<asio::experimental::concurrent_channel<void(ErrorCode, utilxx_base::Json)>>
                    ch;
        std::string node;
        std::string value;
        std::string argJson;
    };

    using RespChannel = asio::experimental::concurrent_channel<void(ErrorCode, utilxx_base::Json)>;
    using WakeChannel = asio::experimental::concurrent_channel<void(ErrorCode, int)>;

    /// 取 seq 之后的 delta; nullopt 表示需全量 sync
    std::optional<std::vector<WireDelta>> deltasSince(uint64_t seq);

    WireSyncPayload buildFullSync();
    /// 构建同步载荷; tailCount>0 时仅取末尾 tailCount 条 (历史分页尾窗)
    /// - fromIndex = 窗口起始绝对下标, totalMessages = 会话总消息数;
    ///   客户端据此展示"上方还有更早消息"并按 WireGetViewMessages 分页拉取
    /// - tailCount==0 时等价 buildFullSync() (全量, fromIndex=0)
    WireSyncPayload          buildTailSync(size_t tailCount);
    /// 构建增量补拉载荷 (计划 STO-4): 仅含 [afterSeq] 之后的展示消息
    /// - `incremental = true`: 客户端按 msg.id 去重后追加到本地历史尾部
    /// - 消息 id 为空的老数据不可去重: 此时返回全量同步 (调用方回退)
    WireSyncPayload buildIncrementalSync(
        std::vector<SequencedViewMessage> rows,
        uint64_t                          afterSeq
    );
    std::shared_ptr<Session> session();

    /// 构建模型信息响应 (WireModelInfo): 当前会话模型名 + 可用模型列表 +
    /// 各模型多模态能力
    /// - 能力 (image/audio/video 输入) 取自 agent 配置的 availableModels,
    ///   与具体会话无关; 客户端据此判断输入栏是否展示附件按钮
    /// - 客户端接入 (WireGetModel) 与会话切换 (switchSession) 都必须带上
    ///   该字段, 否则切换会话后客户端的能力表为空, 附件按钮会消失
    ///
    /// - `args`:
    ///     - [sessionId] 目标会话 id (取该会话的当前模型名)
    ///
    /// - `return` agent 已释放时返回除空模型名外全空的响应
    WireModelInfo buildModelInfo(std::string_view sessionId);

    /// 处理客户端新增模型配置请求 (WireAddModel)
    ///
    /// 顺序: 校验 (与客户端表单同一套规则, 见
    /// agentxx::agent::validateNewModelConfig) → 写入 {dataDir}/agentxx-config.yaml
    /// (文件不存在则创建) → 注册到 ModelProviderRegistry 与
    /// AgentConfig::availableModels → 当前会话切换为新模型 → 回执
    /// (WireAddModelResult) + 回推模型信息 (WireModelInfo)。
    ///
    /// 先落盘再注册: 写盘失败时不注册, 避免出现"本次能用、重启后模型消失"的
    /// 半生效状态; 任何失败都经回执的 error 文本告知客户端。
    ///
    /// - `args`:
    ///     - [req] 客户端提交的模型配置
    ///     - [sender] 发起请求的客户端 transport (回执与模型信息只发回给它)
    void handleAddModel(
        const WireAddModel&                          req,
        const std::shared_ptr<AgentIOTransportBase>& sender
    );

    /// 向客户端推送当前上下文统计 (target 指定时仅发向该客户端; 为空时向所有客户端广播)
    void sendContextStats(const std::shared_ptr<AgentIOTransportBase>& target = nullptr);

    /// 向客户端推送当前消息队列更新
    void sendMessageQueueUpdate();

    /// 追加一条排队消息并同步客户端 (空闲时解除暂停并唤醒驱动循环)
    /// - `return` 分配的条目 id (内容为空时返回空串, 不追加)
    std::string pushMessageQueueItem(
        std::string                  text,
        std::string                  model       = "",
        std::vector<MediaAttachment> attachments = {},
        std::string                  delivery    = std::string{InputDelivery::NextTurn},
        bool                         recovered   = false
    );
    void interruptAndRunNext();
    void clearMessageQueue();
    /// 删除消息队列条目; `return` false = 条目不存在 (调用方回 MessageNotFound)
    bool removeQueueItem(std::string_view itemId);

    // ----- 输入投递 (计划 LOOP-1/2/3/4/11; 仅 ex_ 线程) -----

    /// 受理一条客户端输入 (投递模式分发 + 回执 + 收件箱落库)
    ///
    /// 顺序: 会话校验 → 参数校验 (空内容/投递模式) → 按投递模式分发:
    /// - `next-turn`: 进入消息队列 (空闲时立即执行)
    /// - `next-step`: 轮次进行中登记为待注入 (下一个 modelcall 边界写入上下文),
    ///   空闲时按 next-turn 处理
    /// - `inject`:    登记为待注入 (只进入下一次请求; 不改写上下文/不唤醒会话)
    /// - `collect`:   进入静默合并窗口, 窗口结束后按 next-turn 提交
    ///
    /// 受理结果经 [WireInputAck] 回给请求方 (仅 requestId > 0 时);
    /// 被拒绝的输入不写收件箱。
    ///
    /// - `args`:
    ///     - [input]  客户端输入 (携带 sessionId/delivery/requestId)
    ///     - [sender] 来源 transport (回执只发给它; 为空时广播)
    void handleUserInput(
        WireUserInput                                input,
        const std::shared_ptr<AgentIOTransportBase>& sender
    );

    /// 发送输入受理回执 (requestId == 0 时跳过: 旧客户端不认识该消息类型)
    void sendInputAck(
        const std::shared_ptr<AgentIOTransportBase>& sender,
        uint64_t                                     requestId,
        std::string_view                             delivery,
        std::string_view                             status,
        std::string_view                             reason  = {},
        std::string                              detail  = {},
        std::string_view                             itemId  = {}
    );

    /// 队列状态转移 (记 Debug 日志: 旧状态 → 新状态 + 原因)
    void setQueueState(SessionQueueState state, std::string_view reason);

    /// 队列是否处于"不自动继续"状态
    bool isQueueHalted() const noexcept {
        return queueState_ == SessionQueueState::Paused;
    }

    /// 收件箱落库 (status=admitted): 进程重启后可按"待确认"恢复
    /// - 无持久化 / 条目无 id / payload 为空时 no-op
    void persistInputAdmitted(const MessageQueueItem& item, uint64_t requestId);

    /// 标记收件箱条目终态 (promoted/dropped)
    void persistInputStatus(std::string_view id, std::string_view status, uint64_t promotedSeq = 0);

    /// 从持久化收件箱恢复未投递输入 (run() 预热会话后调用一次)
    /// - 只恢复 status=admitted 的条目, 作为"待确认"进入队列并置 Paused
    ///   (不自动执行, 避免重启后重放副作用)
    void recoverPendingInputs();

    /// `collect` 静默窗口到期: 合并暂存输入为一条并提交
    void flushCollectWindow();

    void startGraceTimer();
    void cancelGraceTimer();
    void failAllPending();

    /// 处理客户端历史分页请求 (WireGetViewMessages → WireViewMessagesPage, target
    /// 指定时仅回复给请求方)
    void handleGetViewMessages(
        const WireGetViewMessages&                   req,
        const std::shared_ptr<AgentIOTransportBase>& target = nullptr
    );

    /// 会话 ID 校验 (端点绑定单一会话; 统一入口校验, 见计划 PRO-7)
    /// - `sessionId` 为空: 视为"未指定", 按当前绑定会话处理 (旧客户端兼容)
    /// - 与当前绑定会话不一致: 拒绝该请求, 回 WireError(SessionMismatch) 给来源
    ///   (来源为空时广播), 并记警告日志 —— 避免切换会话后迟到的旧请求写到新会话
    ///
    /// - `args`:
    ///     - [sessionId] 请求携带的会话 ID
    ///     - [sender]    请求来源 transport (错误回执只发回给它)
    ///     - [what]      请求名称 (日志与错误文本用, 如 "user_input")
    /// - `return` 是否接受该请求
    bool acceptSessionScope(
        std::string_view                             sessionId,
        const std::shared_ptr<AgentIOTransportBase>& sender,
        std::string_view                             what
    );

    /// 实际清理逻辑 (须在 ex_ 线程执行)
    void stopImpl();

    void resolveInterrupt(int64_t id, utilxx_base::Json result);
    void onCancel();

    // ----- 插件事件转发 (仅 ex_ 线程访问) -----

    /// 订阅事件总线 `plugin.` 前缀 (run 开始时调用一次):
    /// - 插件 publish 的事件 (topic 约定 `{插件名}.{事件名}`) 原样转发为
    ///   WirePluginData (plugin/event/data), 宿主不解析载荷语义
    /// - 频率由插件自身控制; 客户端据此判断插件可用性并展示
    /// - 注册成功后向总线发布宿主约定事件 `agentxx_host.client_attached`
    ///   (见 kHostPluginName 注释), 双端插件可据此重发当前状态快照
    ///   (修复"一次性 status 事件先于订阅发布而丢失"的滞留显示问题)
    /// - 同时订阅权限状态变更事件 (service.permission.full_auth): 状态变化时
    ///   向所有客户端广播 WirePermissionState (见 [subscribePermissionEvents])
    void subscribePluginEvents();

    /// 订阅"完全授权所有权限"状态变更事件 (run 开始时调用一次, 与插件事件订阅同处)
    /// - 权限状态由 agent 侧权限中间件持有, 客户端 (TUI Info 侧边栏按钮) 只能
    ///   经服务端读写; 状态变化 (询问卡片勾选 / 任一客户端切换) 经本订阅广播
    ///   给所有已连接客户端, 多端界面保持一致
    void subscribePermissionEvents(const std::shared_ptr<agentxx::events::EventBus>& bus);

    /// 查询 agent 侧权限中间件 (未装配时返回 nullptr)
    agentxx::middleware::PermissionMiddlewareHandle* permissionMiddleware() const;

    /// 当前是否已"完全授权所有权限" (无权限中间件时按 false)
    bool fullAuthorized() const;

    /// 设置"完全授权所有权限"状态 (无权限中间件时忽略并返回 false; 状态变化由
    /// 中间件发布事件, 再经 [subscribePermissionEvents] 广播回各客户端)
    /// - `return` 是否已应用到权限中间件
    bool setFullAuthorized(bool authorized);

    asio::any_io_executor    ex_;
    std::weak_ptr<BaseAgent> agent_;
    Config                   config_;

    // 多客户端列表 (1:N 支持, 仅 ex_ 线程访问: attach/detach/broadcast)
    std::vector<std::shared_ptr<AgentIOTransportBase>> clients_;

    // delta 环形缓冲 (仅 ex_ 线程访问: sendToPeer 写, handleHello 读)
    std::deque<WireDelta> deltaBuffer_;

    // 服务端消息队列 (仅 ex_ 线程访问)
    std::deque<MessageQueueItem> messageQueue_;
    uint64_t                     nextQueueItemId_ = 1;
    /// 队列状态 (计划 LOOP-4: 取代 queuePaused_/pendingInsert_ 两个 bool)
    SessionQueueState queueState_ = SessionQueueState::Idle;
    /// 轮次进行中收到"打断并运行下一条"时置位: 本轮结束后不暂停队列, 直接继续
    bool pendingInsert_ = false;

    /// `collect` 静默合并窗口暂存 (计划 LOOP-11; 仅 ex_ 线程)
    struct CollectEntry {
        uint64_t                            requestId = 0;
        std::string                         text;
        std::string                         model;
        std::vector<MediaAttachment>        attachments;
        std::shared_ptr<AgentIOTransportBase> sender;
    };
    std::vector<CollectEntry>            collectPending_;
    std::shared_ptr<asio::steady_timer>  collectTimer_;
    uint64_t                             nextCollectId_ = 1;
    /// 收件箱条目自增序号 (与队列条目 id 无关, 仅用于恢复时的稳定 id)
    uint64_t nextInboxId_ = 1;
    /// 本端点实例受理过的收件箱条目 id (启动恢复时用于跳过"本进程刚受理、还没轮到
    /// 执行"的条目: 它们不是上次进程的遗留, 不该被当成待确认项)
    std::set<std::string, std::less<>> locallyAdmittedInputs_;

    // 唤醒 channel (驱动循环等待新输入/事件)
    std::shared_ptr<WakeChannel> wakeChannel_;

    // pending interrupt (仅 ex_ 线程访问: handleInterrupt 写, resolveInterrupt 读)
    std::map<int64_t, PendingInterrupt> pending_;
    int64_t                             nextReqId_ = 1;

    // grace 定时器 (仅 ex_ 线程访问: startGraceTimer/cancelGraceTimer)
    std::shared_ptr<asio::steady_timer> graceTimer_;

    // ----- 插件事件转发状态 (仅 ex_ 线程访问) -----

    /// 是否已注册插件事件订阅 (防止 run() 重复注册覆盖回调)
    bool pluginSubscribed_ = false;
    /// 事件总线前缀订阅 id (0 = 未订阅)
    size_t pluginSubId_ = 0;
    /// 权限状态变更事件订阅 id (0 = 未订阅)
    size_t fullAuthSubId_ = 0;
    /// 上行 WirePluginDataUp 对端缺失警告冷却表 (仅 ex_ 线程访问):
    /// client 插件上行数据但 agent 侧未加载同名插件时, 每插件名最多每
    /// kUplinkWarnCooldown 一次 XX_LOGW, 防御高频上行刷屏
    std::map<std::string, std::chrono::steady_clock::time_point> uplinkWarnAt_;
    // 注: 同一 controller 可被多个 client 连接/重连 (1:N), 因此不在此存储
    // 各 client 的接口集上报 —— 收到 client_interfaces 约定事件时仅向 agent
    // 总线转发 (订阅方按事件到达感知各 client 快照), 不做单值缓存避免覆盖

    std::atomic<bool> running_{false};
    std::atomic<bool> turnActive_{false};
    std::atomic<bool> stopped_{false};
};

} // namespace agent
} // namespace agentxx
