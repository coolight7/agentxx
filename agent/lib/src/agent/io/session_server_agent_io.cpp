#include "agentxx/agent/io/session_server_agent_io.h"

#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
#include "agentxx/middlewares/permission.h"
#include "agentxx/plugin/plugin_manager.h"
#include "utilxx/async_offload.h"
#include "agentxx/util/exception.h"
#include "utilxx_base/log.h"
#include "utilxx/crypto.h"
#include "asio/bind_cancellation_slot.hpp"
#include "asio/cancel_after.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/dispatch.hpp"
#include "asio/redirect_error.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "neograph/graph/cancel.h"
#include <algorithm>
#include <chrono>

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
// - `agentxx_host.server_plugins`: 载荷 {"plugins":[{"name","version",
//   "interfaces":[...]},...]} (服务端已加载的 agent 侧插件结构化信息), 随
//   handleHello 发布。client 插件经 EVT_PLUGIN_DATA 或 get_client_state
//   ("agentPlugins") 查询对端可用性与声明的接口, 缺失时可降级提示, 避免
//   静默丢弃造成"操作成功"假象。
// - `agentxx_host.client_interfaces`: 载荷 {"sessionId","interfaces":[...]}
//   (client 宿主支持的接口名集合)。由客户端在 READY 时经上行插件数据通道
//   上报 (ClientPluginManager::onReady); controller 与 client 是 1:N (同
//   会话可多 client 接入/重连), 服务端不存储, 仅在 WirePluginDataUp 分支
//   拦截并发布到 agent 总线 —— agent 侧插件订阅
//   "agentxx_host.client_interfaces" 按事件到达感知各 client 快照, 据此
//   自适应 (如 emit_message_tip 在无 toast 接口的宿主上降级)。
//
// 注意: server_plugins/client_interfaces 会转发到对端 (不以 "client." 开头,
// 不触环回跳过), 对端插件同样可订阅处理。
static constexpr std::string_view kHostPluginName    = "agentxx_host";
static constexpr std::string_view kEvtClientAttached = "client_attached";
static constexpr std::string_view kEvtServerPlugins  = "server_plugins";
/// client 宿主接口集上报 (client → server; 见文件头注释)
static constexpr std::string_view kEvtClientInterfaces = "client_interfaces";
/// 上行 WirePluginDataUp 对端缺失警告冷却 (同一插件名两次警告最小间隔)
static constexpr auto kUplinkWarnCooldown = std::chrono::seconds{30};

/// 向总线发布宿主约定事件 (异步投递到总线 executor, 不阻塞调用方;
/// 总线为空时跳过)。topic 组装为 "plugin.{kHostPluginName}.{event}"。
static void publishHostEvent(
    const std::shared_ptr<agentxx::events::EventBus>& bus,
    std::string_view                                  event,
    std::string                                       dataJson
) {
    if (!bus) {
        return;
    }
    auto topic = fmt::format("plugin.{}.{}", kHostPluginName, event);
    asio::co_spawn(
        bus->executor(),
        [bus, topic = std::move(topic), data = std::move(dataJson)]() -> asio::awaitable<void> {
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
// 多客户端 Transport 管理 (一对多 1:N)
// ---------------------------------------------------------------------------

void SessionServerAgentIO::setTransport(std::shared_ptr<AgentIOTransportBase> transport) {
    AgentIOBase::setTransport(transport);
    if (transport) {
        attachClient(std::move(transport));
    }
}

void SessionServerAgentIO::attachClient(std::shared_ptr<AgentIOTransportBase> transport) {
    if (!transport) {
        return;
    }
    cancelGraceTimer();
    auto it = std::find(clients_.begin(), clients_.end(), transport);
    if (it == clients_.end()) {
        clients_.push_back(transport);
    }
    if (!transport_) {
        transport_ = transport;
    }
}

void SessionServerAgentIO::detachClient(const std::shared_ptr<AgentIOTransportBase>& transport) {
    if (!transport) {
        return;
    }
    auto it = std::find(clients_.begin(), clients_.end(), transport);
    if (it != clients_.end()) {
        clients_.erase(it);
    }
    if (transport_ == transport) {
        transport_ = clients_.empty() ? nullptr : clients_.back();
    }
}

size_t SessionServerAgentIO::clientCount() const noexcept {
    return clients_.size();
}

bool SessionServerAgentIO::hasAliveClient() const noexcept {
    for (const auto& t : clients_) {
        if (t && t->alive()) {
            return true;
        }
    }
    return false;
}

std::vector<std::shared_ptr<AgentIOTransportBase>> SessionServerAgentIO::clients() const {
    return clients_;
}

void SessionServerAgentIO::broadcastToClients(const WireMessage& msg) {
    for (auto it = clients_.begin(); it != clients_.end();) {
        auto& t = *it;
        if (!t || !t->alive()) {
            if (transport_ == t) {
                transport_.reset();
            }
            it = clients_.erase(it);
            continue;
        }
        t->send(msg);
        ++it;
    }
    if (!transport_ && !clients_.empty()) {
        transport_ = clients_.back();
    }
}

void SessionServerAgentIO::sendToClient(
    const std::shared_ptr<AgentIOTransportBase>& transport,
    WireMessage                                  msg
) {
    if (transport && transport->alive()) {
        transport->send(std::move(msg));
    } else {
        sendToPeer(std::move(msg));
    }
}

asio::awaitable<void> SessionServerAgentIO::runTransportLoop() {
    auto transport = transport_;
    if (!transport) {
        co_return;
    }
    co_await runTransportLoop(std::move(transport));
}

asio::awaitable<void>
    SessionServerAgentIO::runTransportLoop(std::shared_ptr<AgentIOTransportBase> transport) {
    if (!transport) {
        co_return;
    }
    while (transport->alive()) {
        auto msg = co_await transport->recv();
        if (!msg.has_value()) {
            break;
        }
        onPeerMessage(std::move(*msg), transport);
    }
}

// ---------------------------------------------------------------------------
// AgentIOBase: 主动发送 (BaseAgent 产出的事件经此广播给所有在线客户端)
// ---------------------------------------------------------------------------

void SessionServerAgentIO::sendToPeer(WireMessage msg) {
    // 新产出的 delta 写入重放缓冲, 供断线重连 hello 时按 seq 增量重放。
    // 以 seq 单调性区分两类 delta:
    // - 新 delta: seq 由 BaseAgent deltaSeq 单调递增分配, 严格大于缓冲尾 seq → 入缓冲
    // - 重放 delta: 来自缓冲内部 (handleHello 重放), seq <= 缓冲尾 seq → 不重复入缓冲
    // 由此重放路径无需特殊发送通道, 也不会污染缓冲
    if (const auto* d = std::get_if<WireDelta>(&msg)) {
        if (deltaBuffer_.empty() || d->seq > deltaBuffer_.back().seq) {
            deltaBuffer_.push_back(*d);
            while (deltaBuffer_.size() > config_.deltaBufferCap) {
                deltaBuffer_.pop_front();
            }
        }
    }
    broadcastToClients(msg);
}

// ---------------------------------------------------------------------------
// AgentIOBase: 被动接收回调 (server 端点不会从 client 收到这些消息)
// ---------------------------------------------------------------------------

void SessionServerAgentIO::onDelta(const WireDelta& /*delta*/) {
    // 空实现仅满足纯虚契约; client→server 协议不包含 WireDelta
}

void SessionServerAgentIO::onSync(const WireSyncPayload& /*payload*/) {
    // 空实现仅满足纯虚契约; client→server 协议不包含 WireSyncPayload
}

asio::awaitable<std::optional<std::string>> SessionServerAgentIO::getInput() {
    co_return std::nullopt;
}

asio::awaitable<utilxx_base::Json> SessionServerAgentIO::handleInterrupt(
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

    utilxx_base::Json result      = utilxx_base::Json::array();
    bool                gotResponse = false;
    bool                cancelled   = false;
    // HIL 等待必须可取消: 取当前会话 token 的 fork 子并绑定 slot,
    // WireCancel 到达 (onCancel 置旗级联) 时打断 async_receive,
    // 返回 {"__cancelled__":true} 使 AgentRunner 不 resume 而抛取消
    // (fork 子随本协程帧持有, 生命周期安全; 父 S 由 Session 持有)
    auto                                          sessForCancel = session();
    std::shared_ptr<neograph::graph::CancelToken> waitOp;
    if (sessForCancel) {
        if (auto sessToken = sessForCancel->getCancelToken()) {
            waitOp = sessToken->fork();
            waitOp->bind_executor(ex_);
            if (waitOp->is_cancelled()) {
                cancelled = true;
            }
        }
    }
    if (!cancelled) {
        co_await agentxx::util::catchErrorAsync<bool>(
            [&]() -> asio::awaitable<bool> {
                // timeout <= 0 表示不限制: 不启用 cancel_after, 无限等待客户端响应
                // (由 resolveInterrupt / failAllPending 正常结束等待)
                if (timeout.count() > 0) {
                    if (waitOp) {
                        result = co_await ch->async_receive(asio::bind_cancellation_slot(
                            waitOp->slot(),
                            asio::cancel_after(timeout, asio::use_awaitable)
                        ));
                    } else {
                        result = co_await ch->async_receive(
                            asio::cancel_after(timeout, asio::use_awaitable)
                        );
                    }
                } else {
                    if (waitOp) {
                        result = co_await ch->async_receive(
                            asio::bind_cancellation_slot(waitOp->slot(), asio::use_awaitable)
                        );
                    } else {
                        result = co_await ch->async_receive(asio::use_awaitable);
                    }
                }
                gotResponse = true;
                co_return true;
            },
            [&](std::string errmsg) -> asio::awaitable<bool> {
                // 取消信号打断的等待按取消处理, 不按超时/过期处理
                if (waitOp && waitOp->is_cancelled()) {
                    cancelled = true;
                    co_return false;
                }
                XX_LOGW("[session_ctrl] interrupt #{} ended early: {}", id, errmsg);
                co_return false;
            },
            nullptr,
            waitOp
        );
    }
    pending_.erase(id);
    if (cancelled) {
        // 被取消: 不发过期通知 (客户端的取消已由 WireCancel 驱动本地收尾),
        // 返回取消标记, 调用方不 resume
        co_return utilxx_base::Json{
            {"__cancelled__", true}
        };
    }
    if (!gotResponse) {
        // 超时/异常结束 (用户未响应): 通知客户端该中断已过期,
        // 使客户端将对应未操作的中断消息标记为过期并结束等待
        if (hasAliveClient()) {
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

void SessionServerAgentIO::pushMessageQueueItem(
    std::string                  text,
    std::string                  model,
    std::vector<MediaAttachment> attachments
) {
    if (text.empty() && attachments.empty()) {
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
    item.attachments = std::move(attachments);
    item.createdAtMs = nowMs;

    // 注意: 空闲状态下收到用户新输入时, 无论队列是否已有积压消息, 均解除暂停并唤醒执行:
    // - 暂停态来自上一轮的异常/取消/中断 (run() 轮末按结果置位), 用于阻止
    //   "积压消息"自动继续执行; 此刻用户主动发送了新输入, 视为明确的新轮次指令
    // - 若空闲且队列为空: 本条消息将被驱动循环立即弹出执行, 此时不向客户端推送中间的
    //   1->0 队列状态, 避免 UI 闪烁
    // - 若空闲且队列已有积压消息: 解除暂停并唤醒驱动循环, 驱动循环从队首依次恢复执行,
    //   同时向客户端推送包含全部排队项的最新队列状态
    const bool isIdle   = !turnActive_.load(std::memory_order_acquire);
    const bool wasEmpty = messageQueue_.empty();

    messageQueue_.push_back(std::move(item));

    if (isIdle) {
        queuePaused_ = false;
        wakeChannel_->try_send(ErrorCode{}, 1);
        if (!wasEmpty) {
            sendMessageQueueUpdate();
        }
    } else {
        // 真正进入排队等待 (前有进行中轮次)，同步队列给客户端
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
    onPeerMessage(std::move(msg), nullptr);
}

void SessionServerAgentIO::onPeerMessage(
    WireMessage                                  msg,
    const std::shared_ptr<AgentIOTransportBase>& sender
) {
    std::visit(
        [this, &sender](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, WireHello>) {
                handleHello(m, {}, sender);
            } else if constexpr (std::is_same_v<T, WireUserInput>) {
                cancelGraceTimer();
                pushMessageQueueItem(
                    std::move(m.text),
                    std::move(m.model),
                    std::move(m.attachments)
                );
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
                handleGetViewMessages(m, sender);
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
                if (!agent_.lock()) {
                    return;
                }
                sendToClient(sender, buildModelInfo(m.sessionId));
            } else if constexpr (std::is_same_v<T, WireGetAppendComponentInfo>) {
                auto agent = agent_.lock();
                if (!agent) {
                    return;
                }
                // 客户端拉取加载的组件信息: 收集已加载的 MCP/Skill/Memory 并回填
                std::vector<AppendComponentNotification> notifications;
                agent->collectAppendComponentInfo(notifications);
                sendToClient(sender, WireAppendComponentInfo{std::move(notifications)});
            } else if constexpr (std::is_same_v<T, WireGetContext>) {
                auto                agent = agent_.lock();
                auto                sess  = session();
                utilxx_base::Json msgs  = utilxx_base::Json::array();
                if (sess && sess->llmMessages.is_array()) {
                    msgs = sess->llmMessages;
                }
                // 确保包含 systemPrompt:
                // 若上下文首条不是 system 消息, 补充当前会话拼装的 systemPrompt
                bool hasSystem = false;
                if (!msgs.empty() && msgs.front().is_object()
                    && msgs.front().value("role", std::string{}) == "system") {
                    hasSystem = true;
                }
                if (!hasSystem && agent) {
                    std::string sysPrompt = agent->buildSystemPrompt(m.sessionId);
                    if (!sysPrompt.empty()) {
                        utilxx_base::Json sysMsg  = utilxx_base::Json::object();
                        sysMsg["role"]              = "system";
                        sysMsg["content"]           = std::move(sysPrompt);
                        utilxx_base::Json newMsgs = utilxx_base::Json::array();
                        newMsgs.push_back(std::move(sysMsg));
                        for (auto item : msgs.items()) {
                            newMsgs.push_back(std::move(item.second));
                        }
                        msgs = std::move(newMsgs);
                    }
                }
                sendToClient(sender, WireContextMessages{std::move(msgs)});
            } else if constexpr (std::is_same_v<T, WireCompactContext>) {
                auto agent = agent_.lock();
                if (!agent || !agent->agentContext || !agent->agentContext->bus) {
                    return;
                }
                auto bus = agent->agentContext->bus;
                auto sid = std::string{m.sessionId};
                asio::co_spawn(
                    bus->executor(),
                    [bus, sid = std::move(sid)]() -> asio::awaitable<void> {
                        co_await bus->publish<events::EventCompactContext>(
                            events::Topic::SummarizationCompact,
                            events::EventCompactContext{.sessionId = sid}
                        );
                    },
                    asio::detached
                );
            } else if constexpr (std::is_same_v<T, WireListSessions>) {
                // 客户端请求持久化会话列表 (会话选择弹窗数据源):
                // 目录扫描 + SQLite 读取属阻塞 I/O, 卸载到 threadPool 执行,
                // 避免阻塞 agent io 线程; 完成后经 shared_from_this 回填响应。
                // 分页: limit > 0 时走 keyset 游标分页查询 (仅返回一页),
                // limit == 0 为全量列举
                auto agent = agent_.lock();
                if (!agent || !agent->agentContext
                    || !agent->agentContext->sessions->sessionStore) {
                    sendToClient(sender, WireSessionList{});
                    return;
                }
                auto sessionStore = agent->agentContext->sessions->sessionStore;
                auto self         = shared_from_this();
                asio::co_spawn(
                    ex_,
                    [self, sessionStore, agent, req = std::move(m), sender](
                    ) -> asio::awaitable<void> {
                        WireSessionList resp;
                        if (agent->agentContext->threadPool) {
                            resp = co_await utilxx::offloadAsync<WireSessionList>(
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
                                            std::move(p.sessions),
                                            p.totalCount,
                                            p.hasMore
                                        };
                                    }
                                    // 旧行为全量列举 (totalCount/hasMore 旧客户端不处理)
                                    auto sessions = sessionStore->listSessions();
                                    co_return WireSessionList{std::move(sessions), 0, false};
                                }
                            );
                        } else {
                            if (req.limit > 0) {
                                const auto p = sessionStore->listSessionsPage(
                                    req.beforeMs,
                                    req.beforeId,
                                    req.limit
                                );
                                resp = WireSessionList{
                                    std::move(p.sessions),
                                    p.totalCount,
                                    p.hasMore
                                };
                            } else {
                                resp = WireSessionList{sessionStore->listSessions(), 0, false};
                            }
                        }
                        self->sendToClient(sender, std::move(resp));
                    },
                    asio::detached
                );
            } else if constexpr (std::is_same_v<T, WireListDir>) {
                // 客户端请求服务端目录列举 (跨设备附件选择):
                // 目录扫描属阻塞 I/O, 卸载到 threadPool 执行, 避免阻塞 agent io 线程
                auto agent = agent_.lock();
                auto self  = shared_from_this();
                asio::co_spawn(
                    ex_,
                    [self, agent, req = std::move(m), sender]() -> asio::awaitable<void> {
                        auto scanDir = [self, agent, req]() -> WireListDirResult {
                            WireListDirResult result;
                            result.reqId = req.reqId;

                            std::string targetDir = req.path;
                            if (targetDir.empty()) {
                                if (agent && agent->agentContext) {
                                    targetDir = agent->agentContext->getSessionWorkDir(
                                        self->config_.sessionId
                                    );
                                }
                                if (targetDir.empty()) {
                                    std::error_code ec;
                                    targetDir = std::filesystem::current_path(ec).string();
                                }
                            }

                            std::error_code ec;
                            auto            canonical = std::filesystem::canonical(targetDir, ec);
                            if (ec) {
                                result.ok    = false;
                                result.error = ec.message();
                                return result;
                            }
                            result.currentDir = canonical.string();
                            if (canonical.has_parent_path()
                                && canonical.parent_path() != canonical) {
                                result.parentDir = canonical.parent_path().string();
                            }

                            std::set<std::string> allowedSet(
                                req.allowedExtensions.begin(),
                                req.allowedExtensions.end()
                            );

                            std::vector<WireDirEntry> dirs;
                            std::vector<WireDirEntry> files;

                            auto iter = std::filesystem::directory_iterator(canonical, ec);
                            if (ec) {
                                result.ok    = false;
                                result.error = ec.message();
                                return result;
                            }

                            for (const auto& entry : iter) {
                                std::error_code ec2;
                                auto            status = entry.status(ec2);
                                if (ec2) {
                                    continue;
                                }
                                std::string filename = entry.path().filename().string();
                                if (!filename.empty() && filename[0] == '.') {
                                    continue;
                                }

                                if (std::filesystem::is_directory(status)) {
                                    WireDirEntry de;
                                    de.name      = filename;
                                    de.fullPath  = entry.path().string();
                                    de.isDir     = true;
                                    de.supported = true;
                                    dirs.push_back(std::move(de));
                                } else if (std::filesystem::is_regular_file(status)) {
                                    auto ext
                                        = utilxx_base::toLower(entry.path().extension().string());
                                    auto mt = agentxx::agent::mediaTypeFromExtension(ext);
                                    if (!mt.has_value()) {
                                        continue;
                                    }
                                    WireDirEntry de;
                                    de.name      = filename;
                                    de.fullPath  = entry.path().string();
                                    de.isDir     = false;
                                    de.sizeBytes = std::filesystem::file_size(entry.path(), ec2);
                                    de.mediaType = *mt;
                                    de.supported
                                        = (allowedSet.empty() || allowedSet.count(ext) > 0);
                                    files.push_back(std::move(de));
                                }
                            }

                            std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) {
                                return a.fullPath < b.fullPath;
                            });
                            std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
                                return a.fullPath < b.fullPath;
                            });

                            for (auto& d : dirs) {
                                result.entries.push_back(std::move(d));
                            }
                            for (auto& f : files) {
                                result.entries.push_back(std::move(f));
                            }
                            result.ok = true;
                            return result;
                        };

                        WireListDirResult res;
                        if (agent && agent->agentContext && agent->agentContext->threadPool) {
                            res = co_await utilxx::offloadAsync<WireListDirResult>(
                                *agent->agentContext->threadPool,
                                [scanDir]() -> asio::awaitable<WireListDirResult> {
                                    co_return scanDir();
                                }
                            );
                        } else {
                            res = scanDir();
                        }
                        if (sender) {
                            self->sendToClient(sender, std::move(res));
                        } else {
                            self->sendToPeer(std::move(res));
                        }
                    },
                    asio::detached
                );
            } else if constexpr (std::is_same_v<T, WireSwitchSession>) {
                // 客户端请求切换会话 (弹窗选择后); 运行态拦截由客户端前置完成
                // 先在线程池中异步预热加载目标会话历史, 避免在 io 线程产生阻塞 SQLite 读
                asio::co_spawn(
                    ex_,
                    [self  = shared_from_this(),
                     newId = std::move(m.sessionId)]() -> asio::awaitable<void> {
                        if (auto agent = self->agent_.lock(); agent && agent->agentContext) {
                            co_await agent->agentContext->getSessionAsync(newId);
                        }
                        self->switchSession(std::move(newId));
                    },
                    asio::detached
                );
            } else if constexpr (std::is_same_v<T, WirePluginDataUp>) {
                // 宿主约定上行事件拦截: client_interfaces (client 宿主接口集
                // 上报, 三期6) —— controller 与 client 是 1:N (同会话可多
                // client 接入/重连), 不做单值存储; 仅向 agent 总线转发
                // (topic plugin.agentxx_host.client_interfaces, 订阅方按事件
                // 到达感知各 client 快照), agent 侧插件据此自适应 (如无
                // toast 接口的宿主上降级提示)
                if (m.plugin == kHostPluginName && m.event == kEvtClientInterfaces) {
                    if (auto agt = agent_.lock(); agt && agt->agentContext) {
                        publishHostEvent(agt->agentContext->bus, kEvtClientInterfaces, m.data);
                    }
                    return;
                }
                // client 插件事件上行: 发布到 agent 事件总线 topic
                // `plugin.client.{插件名}.{事件名}` (载荷 std::string), 由 agent
                // 侧插件经 subscribe("client.{插件名}.{事件名}") 订阅处理
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
                        auto  now = std::chrono::steady_clock::now();
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

void SessionServerAgentIO::handleHello(
    const WireHello&                             hello,
    std::vector<std::string>                     models,
    const std::shared_ptr<AgentIOTransportBase>& sender
) {
    cancelGraceTimer();

    if (!hello.language.empty()) {
        auto agent = agent_.lock();
        if (agent) {
            agent->setLanguage(hello.language, config_.sessionId);
        }
    }

    std::vector<WireDelta>            replayDeltas;
    std::optional<WireSyncPayload>    replaySync;
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
    // 若先重放后 HelloAck, 全量 Sync/增量 WireDelta 会被客户端丢弃 → 重连后历史丢失。
    // HelloAck 之后发送的重放消息经客户端 recvQueue 缓冲, 由 runTransportLoop 正常处理。
    // ack.plugins: 服务端已加载 agent 侧插件结构化信息 (名字+版本+声明接口;
    // client 插件判断对端可用性与能力的正式通道; 与下方 server_plugins 约定
    // 事件二选一处理均可)
    std::vector<WireHelloAck::PluginInfo> loadedPlugins;
    if (auto agent = agent_.lock();
        agent && agent->agentContext && agent->agentContext->pluginManager) {
        for (const auto& p : agent->agentContext->pluginManager->list()) {
            WireHelloAck::PluginInfo info{.name = p.name, .version = p.version};
            info.interfaces = p.requiredInterfaces;
            for (const auto& n : p.optionalInterfaces) {
                info.interfaces.push_back(n);
            }
            loadedPlugins.push_back(std::move(info));
        }
    }
    WireHelloAck helloAck;
    helloAck.ok        = true;
    helloAck.sessionId = config_.sessionId;
    helloAck.tailHash  = std::move(tailHash);
    helloAck.models    = std::move(models);
    helloAck.plugins   = std::move(loadedPlugins);
    helloAck.deviceId  = utilxx::getDeviceId();
    if (auto agent = agent_.lock(); agent && agent->agentContext) {
        helloAck.workDir = agent->agentContext->getSessionWorkDir(config_.sessionId);
    }

    auto doSend = [&](WireMessage m) {
        if (sender) {
            sendToClient(sender, std::move(m));
        } else {
            sendToPeer(std::move(m));
        }
    };

    doSend(std::move(helloAck));

    for (const auto& d : replayDeltas) {
        doSend(d);
    }
    if (replaySync.has_value()) {
        doSend(std::move(replaySync.value()));
    }

    if (sender) {
        sendContextStats(sender);
    } else {
        sendContextStats();
    }

    for (auto& req : pendingInterrupts) {
        doSend(std::move(req));
    }

    // 宿主约定事件 (见文件头 kHostPluginName 注释):
    // - server_plugins: 同 ack.plugins 的约定事件形态 (结构化对象数组, 供
    //   已运行的 client 插件经 EVT_PLUGIN_DATA 订阅处理, 不依赖握手字段)
    // - client_attached: 每次连接握手后重发一次 (重连/同会话新客户端也能
    //   获得状态快照; 与 subscribePluginEvents 处的发布重复无害)
    if (auto agent = agent_.lock(); agent && agent->agentContext) {
        auto pluginInfos = utilxx_base::Json::array();
        if (agent->agentContext->pluginManager) {
            for (const auto& p : agent->agentContext->pluginManager->list()) {
                auto interfaces = p.requiredInterfaces;
                for (const auto& n : p.optionalInterfaces) {
                    interfaces.push_back(n);
                }
                pluginInfos.push_back({
                    {"name",       p.name    },
                    {"version",    p.version },
                    {"interfaces", interfaces}
                });
            }
        }
        publishHostEvent(
            agent->agentContext->bus,
            kEvtServerPlugins,
            utilxx_base::Json{
                {"plugins", pluginInfos}
        }.dump()
        );
        publishHostEvent(
            agent->agentContext->bus,
            kEvtClientAttached,
            fmt::format(R"({{"sessionId":"{}"}})", config_.sessionId)
        );
    }
}

void SessionServerAgentIO::onDisconnect(const std::shared_ptr<AgentIOTransportBase>& transport) {
    if (transport) {
        detachClient(transport);
    } else {
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (!(*it) || !(*it)->alive()) {
                if (transport_ == *it) {
                    transport_.reset();
                }
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
        if (!transport_ && !clients_.empty()) {
            transport_ = clients_.back();
        }
    }
    if (!hasAliveClient() && turnActive_.load(std::memory_order_acquire)) {
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
        // 避免轮次进行中被换走导致 WireDelta/输入错投到新会话
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

    XX_LOGI("[session_ctrl] switched session: {} -> {}", oldThreadId, config_.sessionId);

    // 回推新会话状态: Sync (历史消息, 按尾窗配置分页) + 模型信息 + 上下文统计
    auto sync = buildTailSync(config_.initialSyncTailCount);
    sendToPeer(std::move(sync));

    // 模型信息必须与客户端接入路径 (WireGetModel) 同构: 除当前模型名与可用
    // 模型列表外还要带上各模型的多模态能力 —— 客户端切换会话后收到的
    // WireModelInfo 若缺 capabilities, 其能力表将为空, 输入栏的附件按钮消失
    sendToPeer(buildModelInfo(config_.sessionId));

    sendContextStats();
}

void SessionServerAgentIO::resolveInterrupt(int64_t id, utilxx_base::Json result) {
    auto it = pending_.find(id);
    if (it != pending_.end()) {
        it->second.ch->try_send(ErrorCode{}, std::move(result));
        // 该中断已被某一客户端处理解决, 广播通知所有客户端此中断已结束
        sendToPeer(WireInterruptExpired{id, config_.sessionId});
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
    publishHostEvent(
        bus,
        kEvtClientAttached,
        fmt::format(R"({{"sessionId":"{}"}})", config_.sessionId)
    );
}

// ---------------------------------------------------------------------------
// 驱动循环
// ---------------------------------------------------------------------------

asio::awaitable<void> SessionServerAgentIO::run() {
    running_.store(true, std::memory_order_release);
    // 订阅插件事件 (转发 WirePluginData 供客户端展示插件状态)
    subscribePluginEvents();
    // 预热会话: 会话历史从 SQLite 加载是阻塞操作, 若留到首个请求 (hello/分页/
    // 用户输入) 才做, 会同步卡住 io 线程 (长历史会话可达数百毫秒), 期间所有
    // 会话的 LLM 流/工具执行全部停摆。此处先异步预热一次 (loadSession 卸载到
    // 线程池), 之后的 session() 都是缓存命中
    if (auto agent = agent_.lock(); agent && agent->agentContext) {
        co_await agent->agentContext->getSessionAsync(config_.sessionId);
    }
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
        // 错误提示: agent 线程插入会话历史并发送 MessageTip WireDelta (覆盖
        // runTurnAsync 自身抛异常的兜底路径, 与主路径提示一致)
        auto sendErrorTip = [&](std::string_view errmsg) {
            auto sess = session();
            if (!sess) {
                return;
            }
            auto vm          = ViewMessage::makeText(ViewMessage::Role::Tip, std::string{errmsg});
            vm.tip->tipLevel = ViewMessage::TipLevel::Error;
            vm.collapsed     = true;
            vm.id            = sess->appendViewMessage(vm);
            // 新产出的 WireDelta 必须分配会话级 seq (统一经 Session::nextDeltaSeq):
            // 重放缓冲依赖 seq 单调性, 未分配 seq (=0) 的 WireDelta 不会入缓冲,
            // 断线重连增量重放时该消息会丢失, 导致客户端历史与服务端不一致
            auto d = WireDelta{
                .type    = WireDelta::Type::InsertMessage,
                .message = std::make_shared<ViewMessage>(std::move(vm)),
            };
            d.seq = sess->nextDeltaSeq();
            sendToPeer(std::move(d));
            sess->flushViewMessages();
        };

        BaseAgent::TurnResult turnResult;

        co_await agentxx::util::catchErrorAsync<bool>(
            [&]() -> asio::awaitable<bool> {
                turnResult = co_await agent->runTurnAsync(
                    config_.sessionId,
                    currentItem.text,
                    shared_from_this(),
                    turnModel,
                    std::move(currentItem.attachments)
                );
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
            if (!messageQueue_.empty()) {
                sendMessageQueueUpdate();
            }
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
    for (auto& t : clients_) {
        if (t) {
            t->close();
        }
    }
    clients_.clear();
    if (transport_) {
        transport_->close();
        transport_.reset();
    }
}

// ---------------------------------------------------------------------------
// 推送 / 缓冲
// ---------------------------------------------------------------------------

std::optional<std::vector<WireDelta>> SessionServerAgentIO::deltasSince(uint64_t seq) {
    if (deltaBuffer_.empty()) {
        return std::nullopt;
    }
    uint64_t oldest = deltaBuffer_.front().seq;
    uint64_t newest = deltaBuffer_.back().seq;
    if (seq + 1 < oldest) {
        return std::nullopt;
    }
    // 客户端水位超过服务端当前 seq: 服务端进程重启/会话重建后 seq 从 0 重新
    // 计数, 按增量续传只能得到空列表 (客户端将永远收不到新增量, 界面不再刷新),
    // 故回退全量 sync (客户端据 sync 的 deltaSeq 复位水位)
    if (seq > newest) {
        XX_LOGI(
            "[session_ctrl] delta seq regressed (client={}, server={}), fallback to full sync "
            "(thread={})",
            seq,
            newest,
            config_.sessionId
        );
        return std::nullopt;
    }
    std::vector<WireDelta> out;
    for (const auto& d : deltaBuffer_) {
        if (d.seq > seq) {
            out.push_back(d);
        }
    }
    return out;
}

WireSyncPayload SessionServerAgentIO::buildFullSync() {
    WireSyncPayload p;
    p.fromIndex = 0;
    auto sess   = session();
    if (sess) {
        p.messages      = sess->getFullViewMessagesCopy();
        p.tailHash      = sess->getHashInfo().tailHex;
        p.totalMessages = p.messages.size();
        // 快照水位: 客户端据此复位去重水位 (服务端 seq 可能已重新计数)
        p.deltaSeq = sess->deltaSeq;
    }
    p.messageQueue = std::vector<MessageQueueItem>(messageQueue_.begin(), messageQueue_.end());
    return p;
}

WireSyncPayload SessionServerAgentIO::buildTailSync(size_t tailCount) {
    if (tailCount == 0) {
        return buildFullSync();
    }
    WireSyncPayload p;
    auto            sess = session();
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
    // 快照水位: 客户端据此复位去重水位 (服务端 seq 可能已重新计数)
    p.deltaSeq     = sess->deltaSeq;
    p.messageQueue = std::vector<MessageQueueItem>(messageQueue_.begin(), messageQueue_.end());
    return p;
}

std::string SessionServerAgentIO::currentTailHash() {
    auto sess = session();
    return sess ? sess->getHashInfo().tailHex : std::string{};
}

void SessionServerAgentIO::handleGetViewMessages(
    const WireGetViewMessages&                   req,
    const std::shared_ptr<AgentIOTransportBase>& target
) {
    // 默认页大小: 客户端 count==0 时的兜底 (与 TUI 端请求页大小一致)
    static constexpr uint32_t kDefaultHistoryPageSize = 100;

    WireViewMessagesPage page;
    page.sessionId = config_.sessionId;
    // 会话校验: 端点绑定单一会话; 不匹配的请求按错投处理回空页
    // (切换会话后迟到的旧请求 / 客户端状态异常), 避免泄漏其他会话内容
    if (!req.sessionId.empty() && req.sessionId != config_.sessionId) {
        sendToClient(target, std::move(page));
        return;
    }
    auto sess = session();
    if (!sess) {
        // 会话不存在 (已清理/未创建): 回空页, totalMessages=0 使客户端
        // 判定无更早历史并复位加载状态
        sendToClient(target, std::move(page));
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
    sendToClient(target, std::move(page));
}

void SessionServerAgentIO::sendContextStats(const std::shared_ptr<AgentIOTransportBase>& target) {
    auto sess = session();
    if (!sess || !sess->contextStats) {
        return;
    }
    auto             ctxTokens = sess->contextStats->contextTokens;
    auto             maxTokens = sess->contextStats->maxContextTokens;
    WireContextStats stats{ctxTokens, maxTokens};
    if (target) {
        sendToClient(target, stats);
    } else {
        sendToPeer(stats);
    }
}

WireModelInfo SessionServerAgentIO::buildModelInfo(std::string_view sessionId) {
    WireModelInfo info;
    auto          agent = agent_.lock();
    if (!agent) {
        return info;
    }
    info.currentModel = agent->getCurrentModelName(sessionId);
    if (!agent->agentContext || !agent->agentContext->agentConfig) {
        return info;
    }
    // 可用模型与各模型多模态能力均取自 agent 配置 (与会话无关的静态配置),
    // 供客户端填充模型选择弹窗与判断是否展示附件按钮
    for (const auto& [name, mc] : agent->agentContext->agentConfig->availableModels) {
        info.models.push_back(name);
        info.capabilities.push_back(ModelCapabilityInfo{
            .name       = name,
            .imageInput = mc.imageInput,
            .audioInput = mc.audioInput,
            .videoInput = mc.videoInput,
        });
    }
    return info;
}

std::shared_ptr<Session> SessionServerAgentIO::session() {
    auto agent = agent_.lock();
    if (agent && agent->agentContext) {
        // 缓存命中 (绝大多数调用): 零开销直接返回
        if (agent->agentContext->sessions) {
            if (auto cached = agent->agentContext->sessions->get(config_.sessionId)) {
                return cached;
            }
        }
        // 未命中 (首次接入/切换会话): 此处会同步从 SQLite 加载会话历史,
        // 记录耗时便于发现 io 线程被阻塞的情况 (正常路径已由 run() 预热)
        auto begin = std::chrono::steady_clock::now();
        auto sess  = agent->agentContext->getSession(config_.sessionId);
        XX_LOGD(
            "[session_ctrl] session '{}' loaded synchronously in {} ms (cache miss)",
            config_.sessionId,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin
            )
                .count()
        );
        return sess;
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
    // 先取消上一个宽限定时器: 1:N 模式下多个客户端相继断开会对同一轮次重复触发
    // 本函数, 旧定时器若不取消仍会在自己的到期时刻执行, 使宽限期被"最早创建的
    // 定时器"提前结束 (实际宽限期短于配置), 并让旧协程多持有一份 self 引用
    cancelGraceTimer();
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
            bool hasTransport = self->hasAliveClient();
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
        if (hasAliveClient()) {
            sendToPeer(WireInterruptExpired{id, config_.sessionId});
        }
        p.ch->close();
    }
    pending_.clear();
}

} // namespace agent
} // namespace agentxx
