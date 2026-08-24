#include "agentxx/agent/io/session_server_agent_io.h"

#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/middlewares/permission.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/async_offload.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "asio/cancel_after.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/dispatch.hpp"
#include "asio/redirect_error.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "neograph/graph/cancel.h"
#include <algorithm>

namespace agentxx {
namespace agent {

// ---------------------------------------------------------------------------
// 宿主约定事件 (host convention events)
//
// 宿主以伪插件名 kHostPluginName 向总线发布约定事件, 经 subscribePluginEvents
// 的前缀订阅原样转发为 WirePluginData 到客户端, 同时可被 agent 侧插件直接订阅
// (subscribe 时宿主自动加 "plugin." 前缀)。用于解决跨进程/分端部署时
// "一次性状态事件先于订阅发布而丢失" 与 "对端插件可用性不可知" 问题:
//
// - `agentxx_host.client_attached`: 载荷 {"sessionId":"..."}。端点就绪后发布
//   (subscribePluginEvents 注册成功时 + handleHello 握手完成时各一次; 重复
//   发布无害 —— 状态快照重发是幂等的)。双端插件约定: 收到后重发当前完整
//   状态快照 (如 codegraph 的 status/progress), 使晚接入/晚订阅的客户端
//   立即得到正确显示。
// - `agentxx_host.server_plugins`: 载荷 {"plugins":["名",...]} (服务端已加载的
//   agent 侧插件名列表), 随 handleHello 发布。client 插件经 EVT_PLUGIN_DATA
//   或 get_client_state("agentPlugins") 查询对端可用性, 缺失时可降级提示,
//   避免静默丢弃造成"操作成功"假象。
//
// 注意: 这两个事件会转发到客户端 (不以 "client." 开头, 不触环回跳过),
// client 插件同样可订阅消费 (如据 server_plugins 自适应降级)。
static constexpr std::string_view kHostPluginName    = "agentxx_host";
static constexpr std::string_view kEvtClientAttached = "client_attached";
static constexpr std::string_view kEvtServerPlugins  = "server_plugins";
/// 上行 WirePluginDataUp 对端缺失警告冷却 (同一插件名两次警告最小间隔)
static constexpr auto kUplinkWarnCooldown = std::chrono::seconds{30};

/// 向总线发布宿主约定事件 (异步投递到总线 executor, 不阻塞调用方;
/// 总线为空时跳过)。topic 组装为 "plugin.{kHostPluginName}.{event}"。
static void publishHostEvent(
    const std::shared_ptr<agentxx::event::EventBus>& bus,
    std::string_view                                 event,
    std::string                                      dataJson
) {
    if (!bus) {
        return;
    }
    auto topic = fmt::format("plugin.{}.{}", kHostPluginName, event);
    asio::co_spawn(
        bus->executor(),
        [bus, topic = std::move(topic), data = std::move(dataJson)](
            ) -> asio::awaitable<void> {
            co_await bus->publish(topic, data);
            co_return;
        },
        asio::detached
    );
}

SessionServerAgentIO::SessionServerAgentIO(
    asio::any_io_executor    ex,
    std::weak_ptr<BaseAgent> agent,
    Config                   config
) :
    ex_(std::move(ex)),
    agent_(std::move(agent)),
    config_(std::move(config)),
    wakeChannel_(std::make_shared<WakeChannel>(ex_, 64)) {}

SessionServerAgentIO::~SessionServerAgentIO() {
    stopImpl();
}

// ---------------------------------------------------------------------------
// AgentIOBase: 主动发送 (BaseAgent 产出的事件经此转发给客户端)
// ---------------------------------------------------------------------------

void SessionServerAgentIO::sendToPeer(WireMessage msg) {
    // 新产出的 delta 写入重放缓冲, 供断线重连 hello 时按 seq 增量重放。
    // 以 seq 单调性区分两类 delta:
    // - 新 delta: seq 由 BaseAgent deltaSeq 单调递增分配, 严格大于缓冲尾 seq → 入缓冲
    // - 重放 delta: 来自缓冲内部 (handleHello 重放), seq <= 缓冲尾 seq → 不重复入缓冲
    // 由此重放路径无需特殊发送通道, 也不会污染缓冲
    if (const auto* d = std::get_if<Delta>(&msg)) {
        if (deltaBuffer_.empty() || d->seq > deltaBuffer_.back().seq) {
            deltaBuffer_.push_back(*d);
            while (deltaBuffer_.size() > config_.deltaBufferCap) {
                deltaBuffer_.pop_front();
            }
        }
    }
    AgentIOBase::sendToPeer(std::move(msg));
}

// ---------------------------------------------------------------------------
// AgentIOBase: 被动接收回调 (server 端点不会从 client 收到这些消息)
// ---------------------------------------------------------------------------

void SessionServerAgentIO::onDelta(const Delta& /*delta*/) {
    // 空实现仅满足纯虚契约; client→server 协议不包含 Delta
}

void SessionServerAgentIO::onSync(const SyncPayload& /*payload*/) {
    // 空实现仅满足纯虚契约; client→server 协议不包含 SyncPayload
}

asio::awaitable<std::optional<std::string>> SessionServerAgentIO::getInput() {
    co_return std::nullopt;
}

asio::awaitable<neograph::json> SessionServerAgentIO::handleInterrupt(
    std::string_view /*sessionId*/,
    std::string_view interruptNode,
    std::string_view interruptValue,
    std::string_view interruptArgJson
) {
    auto timeout = config_.interruptTimeout;

    auto    ch   = std::make_shared<RespChannel>(ex_, 1);
    int64_t id   = nextReqId_++;
    pending_[id] = PendingInterrupt{
        ch,
        std::string{interruptNode},
        std::string{interruptValue},
        std::string{interruptArgJson}
    };

    sendToPeer(WireInterruptRequest{
        .id        = id,
        .sessionId = config_.sessionId,
        .node      = std::string{interruptNode},
        .value     = std::string{interruptValue},
        .argJson   = std::string{interruptArgJson},
    });

    neograph::json result      = neograph::json::array();
    bool           gotResponse = false;
    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
            // timeout <= 0 表示不限制: 不启用 cancel_after, 无限等待客户端响应
            // (由 resolveInterrupt / failAllPending 正常结束等待)
            if (timeout.count() > 0) {
                result
                    = co_await ch->async_receive(asio::cancel_after(timeout, asio::use_awaitable));
            } else {
                result = co_await ch->async_receive(asio::use_awaitable);
            }
            gotResponse = true;
            co_return true;
        },
        [&](std::string errmsg) -> asio::awaitable<bool> {
            XX_LOGW("[session_ctrl] interrupt #{} ended early: {}", id, errmsg);
            co_return false;
        }
    );
    pending_.erase(id);
    if (!gotResponse) {
        // 超时/异常结束 (用户未响应): 通知客户端该中断已过期,
        // 使客户端将对应未操作的中断消息标记为过期并结束等待
        if (transport_ && transport_->alive()) {
            sendToPeer(WireInterruptExpired{id, config_.sessionId});
        }
    }
    co_return result;
}

// ---------------------------------------------------------------------------
// 消息队列管理 (ex_ 线程)
// ---------------------------------------------------------------------------

void SessionServerAgentIO::sendMessageQueueUpdate() {
    sendToPeer(WireMessageQueueUpdate{
        .sessionId = config_.sessionId,
        .items     = std::vector<MessageQueueItem>(messageQueue_.begin(), messageQueue_.end()),
    });
}

void SessionServerAgentIO::pushMessageQueueItem(std::string text, std::string model) {
    if (text.empty()) {
        return;
    }
    const auto nowMs = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::system_clock::now().time_since_epoch()
    )
                                                .count());
    MessageQueueItem item;
    item.id          = fmt::format("q-{}", nextQueueItemId_++);
    item.text        = std::move(text);
    item.model       = std::move(model);
    item.createdAtMs = nowMs;

    // 注意: 空闲 + 队列为空时即使处于暂停态也立即执行并解除暂停。
    // - 暂停态来自上一轮的异常/取消/中断 (run() 轮末按结果置位), 用于阻止
    //   "积压消息"自动继续执行; 但此刻队列已空, 本条是用户主动发送的新输入,
    //   视为明确的新轮次指令 —— 若不解除暂停, 驱动循环不会消费队列, 该消息
    //   将永远滞留等待 (异常中断后 TUI 发送消息卡在队列的根因)
    const bool willExecuteImmediately
        = !turnActive_.load(std::memory_order_acquire) && messageQueue_.empty();

    messageQueue_.push_back(std::move(item));

    if (willExecuteImmediately) {
        // 当前完全空闲，此条消息将被驱动循环立即弹出执行，不向客户端推送中间的 1->0 队列状态，避免
        // UI 闪烁
        queuePaused_ = false;
        wakeChannel_->try_send(ErrorCode{}, 1);
    } else {
        // 真正进入排队等待 (前有进行中轮次 / 前有积压消息)，同步队列给客户端
        sendMessageQueueUpdate();
    }
}

void SessionServerAgentIO::interruptAndRunNext() {
    if (messageQueue_.empty()) {
        return;
    }
    queuePaused_ = false;
    if (turnActive_.load(std::memory_order_acquire)) {
        pendingInsert_ = true;
        onCancel();
    } else {
        wakeChannel_->try_send(ErrorCode{}, 1);
    }
}

void SessionServerAgentIO::clearMessageQueue() {
    messageQueue_.clear();
    sendMessageQueueUpdate();
}

void SessionServerAgentIO::removeQueueItem(std::string_view itemId) {
    auto it = std::find_if(
        messageQueue_.begin(),
        messageQueue_.end(),
        [&](const MessageQueueItem& item) {
            return item.id == itemId;
        }
    );
    if (it != messageQueue_.end()) {
        messageQueue_.erase(it);
        sendMessageQueueUpdate();
    }
}

// ---------------------------------------------------------------------------
// AgentIOBase: 对端 (客户端) 发来的消息分发
// ---------------------------------------------------------------------------

void SessionServerAgentIO::onPeerMessage(WireMessage msg) {
    std::visit(
        [this](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, WireHello>) {
                handleHello(m);
            } else if constexpr (std::is_same_v<T, WireUserInput>) {
                cancelGraceTimer();
                pushMessageQueueItem(std::move(m.text), std::move(m.model));
            } else if constexpr (std::is_same_v<T, WireCancel>) {
                // 仅在轮次进行中时暂停队列: 空闲时收到取消 (无轮次可取消) 不应
                // 置位暂停, 否则后续所有新输入都会因队列被误暂停而永远等待执行
                if (turnActive_.load(std::memory_order_acquire)) {
                    queuePaused_ = true;
                }
                onCancel();
            } else if constexpr (std::is_same_v<T, WireInterruptAndRunNext>) {
                interruptAndRunNext();
            } else if constexpr (std::is_same_v<T, WireGetViewMessages>) {
                // 客户端历史分页请求: 切片 [max(0, before-count), before) 回应。
                // viewMessages 为 append-only, 绝对下标恒定, 轮次进行中追加
                // 新消息不影响既有下标, 无竞态; 全程 ex_ 线程 (= Session io 线程)
                handleGetViewMessages(m);
            } else if constexpr (std::is_same_v<T, WireClearMessageQueue>) {
                clearMessageQueue();
            } else if constexpr (std::is_same_v<T, WireRemoveQueueItem>) {
                removeQueueItem(m.itemId);
            } else if constexpr (std::is_same_v<T, WireSelectModel>) {
                auto agent = agent_.lock();
                if (agent) {
                    agent->selectModel(m.sessionId, m.model);
                }
            } else if constexpr (std::is_same_v<T, WireInterruptResponse>) {
                resolveInterrupt(m.id, std::move(m.result));
            } else if constexpr (std::is_same_v<T, WireGetModel>) {
                auto agent = agent_.lock();
                if (!agent) {
                    return;
                }
                std::string              currentModel = agent->getCurrentModelName(m.sessionId);
                std::vector<std::string> models;
                if (agent->agentContext && agent->agentContext->agentConfig) {
                    for (const auto& [name, mc] :
                         agent->agentContext->agentConfig->availableModels) {
                        models.push_back(name);
                    }
                }
                sendToPeer(WireModelInfo{std::move(currentModel), std::move(models)});
            } else if constexpr (std::is_same_v<T, WireGetAppendComponentInfo>) {
                auto agent = agent_.lock();
                if (!agent) {
                    return;
                }
                // 客户端拉取加载的组件信息: 收集已加载的 MCP/Skill/Memory 并回填
                std::vector<AppendComponentNotification> notifications;
                agent->collectAppendComponentInfo(notifications);
                sendToPeer(WireAppendComponentInfo{std::move(notifications)});
            } else if constexpr (std::is_same_v<T, WireGetContext>) {
                auto sess = session();
                if (!sess) {
                    sendToPeer(WireContextMessages{neograph::json::array()});
                    return;
                }
                sendToPeer(WireContextMessages{sess->llmMessages});
            } else if constexpr (std::is_same_v<T, WireListSessions>) {
                // 客户端请求持久化会话列表 (会话选择弹窗数据源):
                // 目录扫描 + SQLite 读取属阻塞 I/O, 卸载到 threadPool 执行,
                // 避免阻塞 agent io 线程; 完成后经 shared_from_this 回填响应。
                // 分页: limit > 0 时走 keyset 游标分页查询 (仅返回一页),
                // limit == 0 为旧行为 (全量列举, 兼容旧客户端)
                auto agent = agent_.lock();
                if (!agent || !agent->agentContext
                    || !agent->agentContext->sessions->sessionStore) {
                    sendToPeer(WireSessionList{});
                    return;
                }
                auto sessionStore = agent->agentContext->sessions->sessionStore;
                auto self         = shared_from_this();
                asio::co_spawn(
                    ex_,
                    [self, sessionStore, agent, req = std::move(m)]() -> asio::awaitable<void> {
                        WireSessionList resp;
                        if (agent->agentContext->threadPool) {
                            resp = co_await agentxx::util::offloadAsync<WireSessionList>(
                                *agent->agentContext->threadPool,
                                [sessionStore, req]() -> asio::awaitable<WireSessionList> {
                                    if (req.limit > 0) {
                                        // keyset 游标分页: 仅返回一页 + 总数/续取标志
                                        const auto p = sessionStore->listSessionsPage(
                                            req.beforeMs,
                                            req.beforeId,
                                            req.limit
                                        );
                                        co_return WireSessionList{
                                            std::move(p.sessions), p.totalCount, p.hasMore
                                        };
                                    }
                                    // 旧行为全量列举 (totalCount/hasMore 旧客户端不消费)
                                    auto sessions = sessionStore->listSessions();
                                    co_return WireSessionList{
                                        std::move(sessions), 0, false
                                    };
                                }
                            );
                        } else {
                            if (req.limit > 0) {
                                const auto p = sessionStore->listSessionsPage(
                                    req.beforeMs,
                                    req.beforeId,
                                    req.limit
                                );
                                resp = WireSessionList{std::move(p.sessions), p.totalCount, p.hasMore};
                            } else {
                                resp = WireSessionList{sessionStore->listSessions(), 0, false};
                            }
                        }
                        self->sendToPeer(std::move(resp));
                    },
                    asio::detached
                );
            } else if constexpr (std::is_same_v<T, WireSwitchSession>) {
                // 客户端请求切换会话 (弹窗选择后); 运行态拦截由客户端前置完成
                switchSession(std::move(m.sessionId));
            } else if constexpr (std::is_same_v<T, WireSetPermission>) {
                // 客户端记住权限选择: 注册路径规则到权限中间件,
                // 后续访问该路径或其子目录时按规则直接允许/拒绝, 不再询问
                auto agent = agent_.lock();
                if (!agent || !agent->agentContext || !agent->agentContext->permissionMiddleware) {
                    return;
                }
                auto& perm = agent->agentContext->permissionMiddleware;
                perm->setFilesystemPermission(
                    m.path,
                    m.allow ? agentxx::middleware::PermissionOperator::ALLOW
                            : agentxx::middleware::PermissionOperator::DENY,
                    m.index
                );
                XX_LOGI(
                    "[session_ctrl] remembered permission rule: {} {} (index={})",
                    m.path,
                    m.allow ? "ALLOW" : "DENY",
                    m.index
                );
            } else if constexpr (std::is_same_v<T, WirePluginDataUp>) {
                // client 插件事件上行: 发布到 agent 事件总线 topic
                // `plugin.client.{插件名}.{事件名}` (载荷 std::string), 由 agent
                // 侧插件经 subscribe("client.{插件名}.{事件名}") 订阅消费
                // (跨端插件数据通道 client → agent);
                // 前缀 "plugin.client." 的发布不会被 subscribePluginEvents 转发
                // 回客户端 (见该处环回跳过逻辑)
                auto agent = agent_.lock();
                if (agent && agent->agentContext && agent->agentContext->bus) {
                    // 对端缺失检测: agent 侧未加载同名插件时, 上行数据发布后
                    // 将无订阅者而静默丢弃 —— 按插件名冷却限频警告, 避免静默
                    // 丢数据造成"操作成功"假象 (如 client /sysinfo 开关同步)
                    if (agent->agentContext->pluginManager
                        && !agent->agentContext->pluginManager->find(m.plugin)) {
                        auto now  = std::chrono::steady_clock::now();
                        auto& at  = uplinkWarnAt_[m.plugin];
                        if (at < now - kUplinkWarnCooldown) {
                            at = now;
                            XX_LOGW(
                                "[session_ctrl] WirePluginDataUp from client: agent-side plugin "
                                "`{}` not loaded on server (event `{}.{}`, data will be dropped)",
                                m.plugin,
                                m.plugin,
                                m.event
                            );
                        }
                    }
                    auto bus   = agent->agentContext->bus;
                    auto topic = "plugin.client." + m.plugin + "." + m.event;
                    auto data  = m.data;
                    // EventBus::publish 为协程; 投递到总线 executor 执行
                    asio::co_spawn(
                        bus->executor(),
                        [bus, topic = std::move(topic), data = std::move(data)](
                        ) -> asio::awaitable<void> {
                            co_await bus->publish(topic, data);
                            co_return;
                        },
                        asio::detached
                    );
                } else {
                    XX_LOGW("[session_ctrl] WirePluginDataUp dropped (no agent bus): {}", m.plugin);
                }
            }
        },
        std::move(msg)
    );
}

// ---------------------------------------------------------------------------
// 连接管理
// ---------------------------------------------------------------------------

void SessionServerAgentIO::handleHello(const WireHello& hello, std::vector<std::string> models) {
    cancelGraceTimer();

    std::vector<Delta>                replayDeltas;
    std::optional<SyncPayload>        replaySync;
    std::string                       tailHash;
    std::vector<WireInterruptRequest> pendingInterrupts;

    auto sess = session();
    tailHash  = sess ? sess->getHashInfo().tailHex : std::string{};

    if (hello.lastSeq > 0) {
        auto deltas = deltasSince(hello.lastSeq);
        if (deltas.has_value()) {
            replayDeltas = std::move(deltas.value());
        } else {
            // delta 缓冲溢出回退全量 sync: 保证重连后客户端与服务端严格一致
            // (罕见路径, 不走尾窗; 客户端收到后整体重置历史窗口)
            replaySync = buildFullSync();
        }
    } else {
        // 首次接入: 按 initialSyncTailCount 决定全量或尾窗同步。
        // 尾窗同步时客户端仅持有末尾窗口, 上方更早历史由其分页拉取
        // (WireGetViewMessages), 避免长会话恢复时全量传输
        if (sess && sess->viewMessageCount() > 0) {
            replaySync = buildTailSync(config_.initialSyncTailCount);
        }
    }

    for (const auto& [id, p] : pending_) {
        pendingInterrupts.push_back(WireInterruptRequest{
            .id        = id,
            .sessionId = config_.sessionId,
            .node      = p.node,
            .value     = p.value,
            .argJson   = p.argJson,
        });
    }

    // 先发送 HelloAck 再重放: 客户端 connect() 握手循环会丢弃 HelloAck 之前的消息,
    // 若先重放后 HelloAck, 全量 Sync/增量 Delta 会被客户端丢弃 → 重连后历史丢失。
    // HelloAck 之后发送的重放消息经客户端 recvQueue 缓冲, 由 runTransportLoop 正常处理。
    // ack.plugins: 服务端已加载的 agent 侧插件名列表 (client 插件判断对端
    // 可用性的正式通道; 与下方 server_plugins 约定事件二选一消费均可)
    std::vector<std::string> loadedPlugins;
    if (auto agent = agent_.lock(); agent && agent->agentContext && agent->agentContext->pluginManager) {
        for (const auto& p : agent->agentContext->pluginManager->list()) {
            loadedPlugins.push_back(p.name);
        }
    }
    sendToPeer(WireHelloAck{
        .ok        = true,
        .sessionId = config_.sessionId,
        .tailHash  = std::move(tailHash),
        .models    = std::move(models),
        .plugins   = std::move(loadedPlugins),
    });

    for (const auto& d : replayDeltas) {
        sendToPeer(d);
    }
    if (replaySync.has_value()) {
        sendToPeer(std::move(replaySync.value()));
    }

    sendContextStats();

    for (auto& req : pendingInterrupts) {
        sendToPeer(std::move(req));
    }

    // 宿主约定事件 (见文件头 kHostPluginName 注释):
    // - server_plugins: 同 ack.plugins 的约定事件形态 (供已运行的 client 插件
    //   经 EVT_PLUGIN_DATA 订阅消费, 不依赖握手字段)
    // - client_attached: 每次连接握手后重发一次 (重连/同会话新客户端也能
    //   获得状态快照; 与 subscribePluginEvents 处的发布重复无害)
    if (auto agent = agent_.lock(); agent && agent->agentContext) {
        auto pluginNames = neograph::json::array();
        if (agent->agentContext->pluginManager) {
            for (const auto& p : agent->agentContext->pluginManager->list()) {
                pluginNames.push_back(p.name);
            }
        }
        publishHostEvent(
            agent->agentContext->bus,
            kEvtServerPlugins,
            neograph::json{{"plugins", pluginNames}}.dump()
        );
        publishHostEvent(
            agent->agentContext->bus,
            kEvtClientAttached,
            fmt::format(R"({{"sessionId":"{}"}})", config_.sessionId)
        );
    }
}

void SessionServerAgentIO::onDisconnect() {
    if (turnActive_.load(std::memory_order_acquire)) {
        startGraceTimer();
    }
}

void SessionServerAgentIO::switchSession(std::string newThreadId) {
    if (newThreadId.empty() || newThreadId == config_.sessionId) {
        // 空 id 非法; 同一会话无需切换 (历史已同步, 重复全量 Sync 反而闪烁)
        if (newThreadId == config_.sessionId) {
            // 仍回推一次 Sync 校准客户端 (本地状态异常时); 与首次接入一致
            // 按尾窗配置分页, 客户端整体重置窗口后可再分页拉取更早历史
            auto sync = buildTailSync(config_.initialSyncTailCount);
            sendToPeer(std::move(sync));
            sendContextStats();
        }
        return;
    }
    if (turnActive_.load(std::memory_order_acquire)) {
        // 双重保护: 客户端已拦截运行态切换, 此处再兜底拒绝,
        // 避免轮次进行中被换走导致 Delta/输入错投到新会话
        XX_LOGW(
            "[session_ctrl] switchSession rejected: turn active (thread={})",
            config_.sessionId
        );
        return;
    }

    auto agent = agent_.lock();
    if (!agent || !agent->agentContext) {
        return;
    }

    const std::string oldThreadId = config_.sessionId;
    config_.sessionId             = newThreadId;
    // delta 重放缓冲属于旧会话的 seq 空间, 新会话 seq 独立编号, 清空避免错配重放
    deltaBuffer_.clear();
    // 消息队列重置
    messageQueue_.clear();
    queuePaused_   = false;
    pendingInsert_ = false;
    // 新会话对当前连接而言等同于首次接入: 重置 firstTurn_ 使首条输入走
    // resume_if_exists=true 的恢复路径 (与会话重启恢复行为一致)
    firstTurn_ = true;

    XX_LOGI("[session_ctrl] switched session: {} -> {}", oldThreadId, config_.sessionId);

    // 回推新会话状态: Sync (历史消息, 按尾窗配置分页) + 模型信息 + 上下文统计
    auto sync = buildTailSync(config_.initialSyncTailCount);
    sendToPeer(std::move(sync));

    std::string              currentModel = agent->getCurrentModelName(config_.sessionId);
    std::vector<std::string> models;
    if (agent->agentContext->agentConfig) {
        for (const auto& [name, mc] : agent->agentContext->agentConfig->availableModels) {
            models.push_back(name);
        }
    }
    sendToPeer(WireModelInfo{std::move(currentModel), std::move(models)});

    sendContextStats();
}

void SessionServerAgentIO::resolveInterrupt(int64_t id, neograph::json result) {
    auto it = pending_.find(id);
    if (it != pending_.end()) {
        it->second.ch->try_send(ErrorCode{}, std::move(result));
    }
}

void SessionServerAgentIO::onCancel() {
    auto sess = session();
    if (sess) {
        auto token = sess->getCancelToken();
        if (token) {
            token->cancel();
        }
    }
}

// ---------------------------------------------------------------------------
// 插件事件转发
// ---------------------------------------------------------------------------

void SessionServerAgentIO::subscribePluginEvents() {
    if (pluginSubscribed_) {
        return;
    }
    auto agent = agent_.lock();
    if (!agent || !agent->agentContext || !agent->agentContext->bus) {
        return;
    }
    auto bus  = agent->agentContext->bus;
    auto self = shared_from_this();
    // 订阅全部插件事件 (topic `plugin.{插件名}.{事件名}`):
    // - 载荷均为 std::string (JSON); 类型不匹配跳过
    // - 原样转发为 WirePluginData, 宿主不解析语义; 频率由插件控制
    pluginSubId_ = bus->listenPrefix(
        "plugin.",
        [weakSelf = std::weak_ptr<SessionServerAgentIO>{self
         }](std::string_view topic, const std::any& payload) {
            auto sp = weakSelf.lock();
            if (!sp) {
                return;
            }
            if (payload.type() != typeid(std::string)) {
                return;
            }
            const auto& data = std::any_cast<const std::string&>(payload);
            // topic: "plugin.{插件名}.{事件名}" → 拆出插件名与事件名
            std::string_view rest = topic.substr(7); // 去掉 "plugin."
            // 环回跳过: client 插件上行事件 (topic "plugin.client.{...}") 仅面向
            // agent 侧插件订阅, 不得转发回客户端 (否则 client 插件会收到自己
            // send_plugin_data 发出的事件, 形成环回)
            if (rest.starts_with("client.")) {
                return;
            }
            auto dot = rest.find('.');
            if (dot == std::string_view::npos || dot == 0 || dot + 1 >= rest.size()) {
                return;
            }
            WirePluginData wpd;
            wpd.plugin = std::string{rest.substr(0, dot)};
            wpd.event  = std::string{rest.substr(dot + 1)};
            wpd.data   = data;
            // 回调运行在 bus executor (与 ex_ 同一 ioCtx); 仍 post 到 ex_
            // 统一串行化端点状态访问 (与插件侧线程解耦)
            asio::post(sp->ex_, [sp, w = std::move(wpd)]() mutable {
                sp->sendToPeer(std::move(w));
            });
        }
    );
    pluginSubscribed_ = true;
    // 发布宿主约定事件 client_attached: 双端插件据此重发当前状态快照,
    // 修复"status 等一次性事件先于本订阅发布而丢失 → 客户端滞留初始占位"
    // 的问题 (晚创建的控制器/晚接入的客户端由此获得快照)
    publishHostEvent(bus, kEvtClientAttached, fmt::format(R"({{"sessionId":"{}"}})", config_.sessionId));
}

// ---------------------------------------------------------------------------
// 驱动循环
// ---------------------------------------------------------------------------

asio::awaitable<void> SessionServerAgentIO::run() {
    running_.store(true, std::memory_order_release);
    // 订阅插件事件 (转发 WirePluginData 供客户端展示插件状态)
    subscribePluginEvents();
    while (!stopped_.load(std::memory_order_acquire)) {
        if (pendingInsert_) {
            pendingInsert_ = false;
            queuePaused_   = false;
        }

        if (queuePaused_ || messageQueue_.empty()) {
            turnActive_.store(false, std::memory_order_release);
            auto [ec, val]
                = co_await wakeChannel_->async_receive(asio::as_tuple(asio::use_awaitable));
            if (ec || stopped_.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }

        // 取出队首消息
        auto currentItem = std::move(messageQueue_.front());
        messageQueue_.pop_front();
        sendMessageQueueUpdate();

        turnActive_.store(true, std::memory_order_release);

        auto agent = agent_.lock();
        if (!agent) {
            turnActive_.store(false, std::memory_order_release);
            break;
        }

        std::string turnModel = std::move(currentItem.model);

        // catchErrorAsync: 取消类异常 (CancelledException/NodeInterrupt) 与普通异常
        // 一致转为错误消息通知客户端 (onRethrow), 避免异常逃逸 co_spawn 完成处理器;
        // 其余异常同样转为错误消息
        // 错误提示: agent 线程插入会话历史并发送 MessageTip Delta (覆盖
        // runTurnAsync 自身抛异常的兜底路径, 与主路径提示一致)
        auto sendErrorTip = [&](std::string_view errmsg) {
            auto sess = session();
            if (!sess) {
                return;
            }
            auto vm          = ViewMessage::makeText(ViewMessage::Role::Tip, std::string{errmsg});
            vm.tip->tipLevel = ViewMessage::TipLevel::Error;
            const auto id    = sess->appendViewMessage(std::move(vm));
            // 新产出的 Delta 必须分配会话级 seq (统一经 Session::nextDeltaSeq):
            // 重放缓冲依赖 seq 单调性, 未分配 seq (=0) 的 Delta 不会入缓冲,
            // 断线重连增量重放时该消息会丢失, 导致客户端历史与服务端不一致
            auto d = Delta{
                .type    = Delta::Type::MessageTip,
                .text    = std::string{errmsg},
                .msgId   = id,
                .tipType = Delta::TipType::Error,
            };
            d.seq = sess->nextDeltaSeq();
            sendToPeer(std::move(d));
        };

        BaseAgent::TurnResult turnResult;

        co_await agentxx::util::catchErrorAsync<bool>(
            [&]() -> asio::awaitable<bool> {
                turnResult = co_await agent->runTurnAsync(
                    config_.sessionId,
                    currentItem.text,
                    firstTurn_,
                    shared_from_this(),
                    turnModel
                );
                firstTurn_ = false;
                sendToPeer(WireTurnResult{
                    .sessionId    = config_.sessionId,
                    .hasError     = turnResult.hasError,
                    .errorMessage = turnResult.errorMessage,
                    .interrupted  = turnResult.interrupted,
                });
                sendContextStats();
                co_return true;
            },
            [&](std::string errmsg) -> asio::awaitable<bool> {
                XX_LOGE("[session_ctrl] turn error: {}", errmsg);
                sendErrorTip(errmsg);
                turnResult.hasError     = true;
                turnResult.errorMessage = errmsg;
                turnResult.interrupted  = false;
                sendToPeer(WireTurnResult{
                    .sessionId    = config_.sessionId,
                    .hasError     = true,
                    .errorMessage = std::move(errmsg),
                    .interrupted  = false,
                });
                co_return false;
            },
            [&](std::string& errmsg) -> std::optional<bool> {
                XX_LOGE("[session_ctrl] turn error: {}", errmsg);
                sendErrorTip(errmsg);
                turnResult.hasError     = true;
                turnResult.errorMessage = errmsg;
                turnResult.interrupted  = false;
                sendToPeer(WireTurnResult{
                    .sessionId    = config_.sessionId,
                    .hasError     = true,
                    .errorMessage = std::move(errmsg),
                    .interrupted  = false,
                });
                return false;
            }
        );

        // 仅当正常执行成功一轮后，才继续自动发送消息队列中的消息
        if (!turnResult.hasError && !turnResult.interrupted) {
            queuePaused_ = false;
        } else {
            queuePaused_ = true;
        }
        turnActive_.store(false, std::memory_order_release);
    }
    running_.store(false, std::memory_order_release);
}

void SessionServerAgentIO::stop() {
    if (stopped_.load(std::memory_order_acquire)) {
        return;
    }
    asio::dispatch(ex_, [self = shared_from_this()]() {
        self->stopImpl();
    });
}

void SessionServerAgentIO::stopImpl() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true)) {
        return;
    }
    cancelGraceTimer();
    failAllPending();
    wakeChannel_->close();
    onCancel();
    // 退订插件事件前缀 (防止端点析构后回调悬垂)
    if (pluginSubId_ != 0) {
        if (auto agent = agent_.lock(); agent && agent->agentContext && agent->agentContext->bus) {
            agent->agentContext->bus->unlistenPrefix(pluginSubId_);
        }
        pluginSubId_ = 0;
    }
    if (transport_) {
        transport_->close();
    }
}

// ---------------------------------------------------------------------------
// 推送 / 缓冲
// ---------------------------------------------------------------------------

std::optional<std::vector<Delta>> SessionServerAgentIO::deltasSince(uint64_t seq) {
    if (deltaBuffer_.empty()) {
        return std::nullopt;
    }
    uint64_t oldest = deltaBuffer_.front().seq;
    if (seq + 1 < oldest) {
        return std::nullopt;
    }
    std::vector<Delta> out;
    for (const auto& d : deltaBuffer_) {
        if (d.seq > seq) {
            out.push_back(d);
        }
    }
    return out;
}

SyncPayload SessionServerAgentIO::buildFullSync() {
    SyncPayload p;
    p.fromIndex = 0;
    auto sess   = session();
    if (sess) {
        p.messages      = sess->getFullViewMessagesCopy();
        p.tailHash      = sess->getHashInfo().tailHex;
        p.totalMessages = p.messages.size();
    }
    p.messageQueue = std::vector<MessageQueueItem>(messageQueue_.begin(), messageQueue_.end());
    return p;
}

SyncPayload SessionServerAgentIO::buildTailSync(size_t tailCount) {
    if (tailCount == 0) {
        return buildFullSync();
    }
    SyncPayload p;
    auto        sess = session();
    if (!sess) {
        p.messageQueue = std::vector<MessageQueueItem>(messageQueue_.begin(), messageQueue_.end());
        return p;
    }
    const size_t total = sess->viewMessageCount();
    // 窗口起始下标: 总数不足窗口大小时从 0 开始 (此时等价全量)
    const size_t start = (total > tailCount) ? (total - tailCount) : 0;
    p.fromIndex        = start;
    p.totalMessages    = total;
    p.messages         = sess->getViewMessagesRange(start, total);
    p.tailHash         = sess->getHashInfo().tailHex;
    p.messageQueue     = std::vector<MessageQueueItem>(messageQueue_.begin(), messageQueue_.end());
    return p;
}

std::string SessionServerAgentIO::currentTailHash() {
    auto sess = session();
    return sess ? sess->getHashInfo().tailHex : std::string{};
}

void SessionServerAgentIO::handleGetViewMessages(const WireGetViewMessages& req) {
    // 默认页大小: 客户端 count==0 时的兜底 (与 TUI 端请求页大小一致)
    static constexpr uint32_t kDefaultHistoryPageSize = 100;

    WireViewMessagesPage page;
    page.sessionId = config_.sessionId;
    // 会话校验: 端点绑定单一会话; 不匹配的请求按错投处理回空页
    // (切换会话后迟到的旧请求 / 客户端状态异常), 避免泄漏其他会话内容
    if (!req.sessionId.empty() && req.sessionId != config_.sessionId) {
        sendToPeer(std::move(page));
        return;
    }
    auto sess = session();
    if (!sess) {
        // 会话不存在 (已清理/未创建): 回空页, totalMessages=0 使客户端
        // 判定无更早历史并复位加载状态
        sendToPeer(std::move(page));
        return;
    }
    const size_t total = sess->viewMessageCount();
    page.totalCount    = total;
    // beforeIndex == 0 视为"从末尾向前取" (客户端首次加载兜底);
    // 正常分页流程窗口顶部为 0 时客户端不应再发起请求
    const uint64_t before
        = (req.beforeIndex == 0) ? total : std::min<uint64_t>(req.beforeIndex, total);
    const uint32_t count
        = (req.count == 0) ? kDefaultHistoryPageSize : std::min(req.count, kDefaultHistoryPageSize);
    const uint64_t cnt = std::min<uint64_t>(count, before);
    page.startIndex    = before - cnt;
    if (cnt > 0) {
        page.messages = sess->getViewMessagesRange(
            static_cast<size_t>(page.startIndex),
            static_cast<size_t>(before)
        );
    }
    sendToPeer(std::move(page));
}

void SessionServerAgentIO::sendContextStats() {
    auto sess = session();
    if (!sess || !sess->contextStats) {
        return;
    }
    auto ctxTokens = sess->contextStats->contextTokens;
    auto maxTokens = sess->contextStats->maxContextTokens;
    sendToPeer(WireContextStats{ctxTokens, maxTokens});
}

std::shared_ptr<Session> SessionServerAgentIO::session() {
    auto agent = agent_.lock();
    if (agent && agent->agentContext) {
        return agent->agentContext->getSession(config_.sessionId);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// grace / pending
// ---------------------------------------------------------------------------

void SessionServerAgentIO::startGraceTimer() {
    if (config_.gracePeriod.count() <= 0) {
        onCancel();
        failAllPending();
        return;
    }
    auto timer = std::make_shared<asio::steady_timer>(ex_);
    timer->expires_after(config_.gracePeriod);
    graceTimer_ = timer;
    auto self   = shared_from_this();
    asio::co_spawn(
        ex_,
        [self, timer]() -> asio::awaitable<void> {
            ErrorCode ec;
            co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                co_return;
            }
            bool hasTransport = self->transport_ && self->transport_->alive();
            if (!hasTransport && self->turnActive_.load(std::memory_order_acquire)) {
                XX_LOGW(
                    "[session_ctrl] grace period expired, cancelling turn (thread={})",
                    self->config_.sessionId
                );
                self->onCancel();
                self->failAllPending();
            }
            co_return;
        },
        asio::detached
    );
}

void SessionServerAgentIO::cancelGraceTimer() {
    auto t = std::move(graceTimer_);
    graceTimer_.reset();
    if (t) {
        t->cancel();
    }
}

void SessionServerAgentIO::failAllPending() {
    for (auto& [id, p] : pending_) {
        // 通知客户端该中断已过期 (停止/断线宽限期满/会话取消), 使客户端
        // 将对应未操作的中断消息标记为过期并结束等待
        if (transport_ && transport_->alive()) {
            sendToPeer(WireInterruptExpired{id, config_.sessionId});
        }
        p.ch->close();
    }
    pending_.clear();
}

} // namespace agent
} // namespace agentxx
