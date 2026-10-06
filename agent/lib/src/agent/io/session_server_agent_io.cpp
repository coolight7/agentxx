#include "agentxx/agent/io/session_server_agent_io.h"

#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/config_writer.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
#include "agentxx/middlewares/permission.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/exception.h"
#include "asio/bind_cancellation_slot.hpp"
#include "asio/cancel_after.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/dispatch.hpp"
#include "asio/redirect_error.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "neograph/graph/cancel.h"
#include "utilxx/async_offload.h"
#include "utilxx/crypto.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <chrono>
#include <set>

namespace agentxx {
namespace agent {

/// 增量补拉单次最多回补的展示消息条数 (计划 STO-4)
/// - 超过该值说明客户端离线时间很长, 回退全量/尾窗同步, 避免一次传输过大
static constexpr size_t kIncrementalReplayMaxMessages = 512;

/// 会话标题规范化 (计划 RET-1a): 去首尾空白、换行/制表符折叠为空格、限长
/// - 标题是会话列表里的单行文本, 不是正文: 多行内容会被截断为一行
/// - 超长截断按 UTF-8 字符边界处理, 避免产生半个字符
static std::string normalizeSessionTitle(std::string_view raw) {
    static constexpr size_t kMaxTitleChars = 120;
    std::string out;
    out.reserve(std::min(raw.size(), kMaxTitleChars * 4));
    bool lastWasSpace = true; // 前导空白一并吃掉
    size_t chars = 0;
    for (size_t i = 0; i < raw.size();) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        const size_t        len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        if (i + len > raw.size()) {
            break; // 尾部不完整的多字节序列直接丢弃
        }
        const bool isSpace = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (isSpace) {
            lastWasSpace = true;
        } else {
            if (lastWasSpace && !out.empty()) {
                out.push_back(' ');
                ++chars;
            }
            if (chars >= kMaxTitleChars) {
                break;
            }
            out.append(raw.substr(i, len));
            ++chars;
            lastWasSpace = false;
        }
        i += len;
    }
    return out;
}

/// 会话列表/检索请求的统一取数实现 (计划 RET-1a)
///
/// 三种形态共用同一条消息 (`WireListSessions`):
/// - 关键词非空: 按标题/展示历史正文检索 (忽略游标字段), 命中片段随条目回传
/// - limit > 0:  keyset 游标分页, 只返回一页 + 总数/续取标志
/// - 其余:        旧行为全量列举
///
/// 目录扫描与 SQLite 读取属阻塞 I/O, 调用方负责把它卸载到线程池执行。
static WireSessionList
    listSessionsFor(const WireListSessions& req, agentxx::agent::SessionStore& store) {
    if (!req.keyword.empty()) {
        auto       hits = store.searchSessions(req.keyword, req.limit);
        std::vector<SessionInfo> sessions;
        sessions.reserve(hits.size());
        for (auto& hit : hits) {
            SessionInfo info = std::move(hit.info);
            // 正文命中时把命中片段带上, 客户端在列表第二行展示"命中的是哪一段"
            info.snippet = std::move(hit.snippet);
            sessions.push_back(std::move(info));
        }
        // 检索不做分页: 一次返回全部命中 (数量上限由 SessionStore::kSearchDefaultLimit
        // / 客户端传入的 limit 决定), hasMore 恒为 false
        const auto count = static_cast<uint64_t>(sessions.size());
        return WireSessionList{std::move(sessions), count, false};
    }
    if (req.limit > 0) {
        const auto p = store.listSessionsPage(req.beforeMs, req.beforeId, req.limit);
        return WireSessionList{std::move(p.sessions), p.totalCount, p.hasMore};
    }
    // 旧行为全量列举 (totalCount/hasMore 旧客户端不处理)
    auto sessions = store.listSessions();
    return WireSessionList{std::move(sessions), 0, false};
}

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
    collectTimer_(std::make_shared<asio::steady_timer>(ex_)),
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
    bool              gotResponse = false;
    bool              cancelled   = false;
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
        .state     = std::string{sessionQueueStateText(queueState_)},
    });
}

void SessionServerAgentIO::setQueueState(SessionQueueState state, std::string_view reason) {
    if (queueState_ == state) {
        return;
    }
    XX_LOGD(
        "[session_ctrl] queue state {} -> {} ({}; session={})",
        sessionQueueStateText(queueState_),
        sessionQueueStateText(state),
        reason,
        config_.sessionId
    );
    queueState_ = state;
}

std::string SessionServerAgentIO::pushMessageQueueItem(
    std::string                  text,
    std::string                  model,
    std::vector<MediaAttachment> attachments,
    std::string                  delivery,
    bool                         recovered
) {
    if (text.empty() && attachments.empty()) {
        return {};
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
    item.delivery    = std::move(delivery);
    item.recovered   = recovered;

    // 注意: 空闲状态下收到用户新输入时, 无论队列是否已有积压消息, 均解除暂停并唤醒执行:
    // - 暂停态来自上一轮的异常/取消/中断 (run() 轮末按结果置位), 用于阻止
    //   "积压消息"自动继续执行; 此刻用户主动发送了新输入, 视为明确的新轮次指令
    // - 若空闲且队列为空: 本条消息将被驱动循环立即弹出执行, 此时不向客户端推送中间的
    //   1->0 队列状态, 避免 UI 闪烁
    // - 若空闲且队列已有积压消息: 解除暂停并唤醒驱动循环, 驱动循环从队首依次恢复执行,
    //   同时向客户端推送包含全部排队项的最新队列状态
    const bool isIdle   = !turnActive_.load(std::memory_order_acquire);
    const bool wasEmpty = messageQueue_.empty();
    const bool wasHalted = isQueueHalted();

    const std::string itemId = item.id;
    messageQueue_.push_back(std::move(item));

    if (isIdle) {
        if (wasHalted) {
            setQueueState(
                wasEmpty ? SessionQueueState::Idle : SessionQueueState::Draining,
                "user input"
            );
        }
        wakeChannel_->try_send(ErrorCode{}, 1);
        if (!wasEmpty) {
            sendMessageQueueUpdate();
        }
    } else {
        // 真正进入排队等待 (前有进行中轮次)，同步队列给客户端
        sendMessageQueueUpdate();
    }
    return itemId;
}

void SessionServerAgentIO::interruptAndRunNext() {
    if (messageQueue_.empty()) {
        return;
    }
    setQueueState(
        turnActive_.load(std::memory_order_acquire) ? SessionQueueState::Draining
                                                    : SessionQueueState::Idle,
        "interrupt and run next"
    );
    if (turnActive_.load(std::memory_order_acquire)) {
        pendingInsert_ = true;
        onCancel();
    } else {
        wakeChannel_->try_send(ErrorCode{}, 1);
    }
}

void SessionServerAgentIO::clearMessageQueue() {
    // 清空的条目在收件箱中标记终态: 重启后不再作为"待确认"重新出现
    for (const auto& item : messageQueue_) {
        persistInputStatus(item.id, SessionStore::SessionInputStatus::Dropped);
    }
    // 待合并的 collect 输入一并丢弃: 必须回执, 否则对应客户端会一直等受理结果
    for (auto& entry : collectPending_) {
        sendInputAck(
            entry.sender,
            entry.requestId,
            InputDelivery::Collect,
            InputStatus::Rejected,
            InputRejectReason::QueueCleared,
            "message queue cleared before the input was submitted"
        );
    }
    collectPending_.clear();
    messageQueue_.clear();
    sendMessageQueueUpdate();
}

/// 删除消息队列条目
/// - `return` true = 找到并删除; false = 条目不存在 (调用方据此回 `MessageNotFound`,
///   让客户端能区分"删掉了"与"条目已经不在"而不是静默成功)
bool SessionServerAgentIO::removeQueueItem(std::string_view itemId) {
    auto it = std::find_if(
        messageQueue_.begin(),
        messageQueue_.end(),
        [&](const MessageQueueItem& item) {
            return item.id == itemId;
        }
    );
    if (it == messageQueue_.end()) {
        return false;
    }
    persistInputStatus(itemId, SessionStore::SessionInputStatus::Dropped);
    messageQueue_.erase(it);
    sendMessageQueueUpdate();
    return true;
}

// ---------------------------------------------------------------------------
// AgentIOBase: 对端 (客户端) 发来的消息分发
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 输入投递 (LOOP-1/2/3/4/11)
// ---------------------------------------------------------------------------

void SessionServerAgentIO::sendInputAck(
    const std::shared_ptr<AgentIOTransportBase>& sender,
    uint64_t                                     requestId,
    std::string_view                             delivery,
    std::string_view                             status,
    std::string_view                             reason,
    std::string                                  detail,
    std::string_view                             itemId
) {
    // 旧客户端没有 requestId (也不认识 InputAck 消息类型): 不回执, 保持旧行为
    if (requestId == 0) {
        return;
    }
    WireInputAck ack;
    ack.requestId = requestId;
    ack.sessionId = config_.sessionId;
    ack.delivery  = std::string{delivery};
    ack.status    = std::string{status};
    ack.reason    = std::string{reason};
    ack.detail    = std::move(detail);
    ack.itemId    = std::string{itemId};
    sendToClient(sender, std::move(ack));
}

void SessionServerAgentIO::handleUserInput(
    WireUserInput                                input,
    const std::shared_ptr<AgentIOTransportBase>& sender
) {
    const std::string_view delivery = InputDelivery::normalize(input.delivery);

    // 会话范围校验: 不匹配时除了既有 WireError, 再回一条结构化拒绝 (客户端据此展示)
    if (!acceptSessionScope(input.sessionId, sender, "user_input")) {
        sendInputAck(
            sender,
            input.requestId,
            delivery,
            InputStatus::Rejected,
            InputRejectReason::SessionMismatch,
            fmt::format(
                "request session '{}' does not match the bound session '{}'",
                input.sessionId,
                config_.sessionId
            )
        );
        return;
    }
    if (stopped_.load(std::memory_order_acquire)) {
        sendInputAck(
            sender,
            input.requestId,
            delivery,
            InputStatus::Rejected,
            InputRejectReason::ServerStopped,
            "session controller is stopping"
        );
        return;
    }
    // agent 侧分阶段关闭 (计划 ARC-5): agent 已进入关闭阶段时同样不受理新输入
    // - 与上面的端点停止相互独立: 端点在进程退出时由调用方 stop, agent 可能
    //   更早开始关闭 (例如宿主先关 agent 再停端点)
    if (auto agent = agent_.lock();
        agent && agent->agentContext && agent->agentContext->isShuttingDown()) {
        sendInputAck(
            sender,
            input.requestId,
            delivery,
            InputStatus::Rejected,
            InputRejectReason::ServerStopped,
            "agent is shutting down"
        );
        return;
    }
    if (!InputDelivery::known(delivery)) {
        XX_LOGW(
            "[session_ctrl] user_input rejected: unknown delivery '{}' (session={})",
            input.delivery,
            config_.sessionId
        );
        sendInputAck(
            sender,
            input.requestId,
            delivery,
            InputStatus::Rejected,
            InputRejectReason::BadDelivery,
            fmt::format("unknown delivery mode '{}'", input.delivery)
        );
        return;
    }
    if (input.text.empty() && input.attachments.empty()) {
        sendInputAck(
            sender,
            input.requestId,
            delivery,
            InputStatus::Rejected,
            InputRejectReason::EmptyContent,
            "input has neither text nor attachments"
        );
        return;
    }

    cancelGraceTimer();

    // `collect`: 进入静默合并窗口 (窗口内同一客户端的连续输入合并为一条)
    if (delivery == InputDelivery::Collect && config_.collectWindow.count() > 0) {
        collectPending_.push_back(CollectEntry{
            .requestId   = input.requestId,
            .text        = std::move(input.text),
            .model       = std::move(input.model),
            .attachments = std::move(input.attachments),
            .sender      = sender,
        });
        if (collectTimer_) {
            collectTimer_->expires_after(config_.collectWindow);
            collectTimer_->async_wait([self = shared_from_this()](ErrorCode ec) {
                if (!ec) {
                    self->flushCollectWindow();
                }
            });
        }
        XX_LOGD(
            "[session_ctrl] input collected ({} pending, session={})",
            collectPending_.size(),
            config_.sessionId
        );
        return;
    }

    const bool turnActive = turnActive_.load(std::memory_order_acquire);

    // `inject` / `next-step`: 登记为待注入输入 (由 modelcall 的请求装配边界取用)
    // - inject:    只进入下一次请求 (不改写上下文, 不唤醒会话)
    // - next-step: 轮次进行中才注入 (空闲时按 next-turn 立即开轮, 否则用户会
    //              发现"发了消息却没有任何反应")
    const bool steerAtStep
        = (delivery == InputDelivery::Inject) || (delivery == InputDelivery::NextStep && turnActive);
    if (steerAtStep) {
        auto sess = session();
        if (!sess) {
            sendInputAck(
                sender,
                input.requestId,
                delivery,
                InputStatus::Rejected,
                InputRejectReason::SessionNotFound,
                fmt::format("session '{}' is not available", config_.sessionId)
            );
            return;
        }
        // 带附件的输入无法在请求装配边界处理 (附件需要服务端加载并转 Base64,
        // 只在轮次启动路径实现): 按 next-turn 排队, 保证内容不丢失
        if (!input.attachments.empty()) {
            // 收件箱载荷 (元数据) 需在 move 前构造
            MessageQueueItem inboxItem;
            inboxItem.text        = input.text;
            inboxItem.model       = input.model;
            inboxItem.attachments = input.attachments;
            inboxItem.delivery    = std::string{InputDelivery::NextTurn};
            const auto itemId     = pushMessageQueueItem(
                std::move(input.text),
                std::move(input.model),
                std::move(input.attachments),
                std::string{InputDelivery::NextTurn}
            );
            inboxItem.id = itemId;
            persistInputAdmitted(inboxItem, input.requestId);
            sendInputAck(
                sender,
                input.requestId,
                delivery,
                turnActive ? InputStatus::Queued : InputStatus::Started,
                {},
                "input carries attachments: queued as a new turn",
                itemId
            );
            return;
        }

        SessionPendingInput pending;
        pending.id          = fmt::format("i-{}", nextInboxId_++);
        pending.text        = std::move(input.text);
        pending.model       = std::move(input.model);
        pending.source      = "user";
        pending.delivery    = std::string{delivery};
        pending.createdAtMs = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            )
                .count()
        );
        // 收件箱载荷需要在 move 进会话前拷贝 (move 后 input.text 为空)
        MessageQueueItem inboxItem{
            .id          = pending.id,
            .text        = pending.text,
            .model       = pending.model,
            .attachments = {},
            .createdAtMs = pending.createdAtMs,
            .delivery    = pending.delivery,
            .recovered   = false,
        };
        const auto pendingId = pending.id;
        sess->enqueuePendingInput(std::move(pending));
        // 收件箱: 受理即落库 (真正进入上下文/请求时标记 promoted)
        persistInputAdmitted(inboxItem, input.requestId);
        XX_LOGI(
            "[session_ctrl] input steered as {} (turn_active={}, session={})",
            delivery,
            turnActive,
            config_.sessionId
        );
        sendInputAck(
            sender,
            input.requestId,
            delivery,
            turnActive ? InputStatus::Steered : InputStatus::Queued,
            {},
            turnActive ? "input will join the running turn at the next request"
                       : "input will join the next request",
            pendingId
        );
        return;
    }

    // `next-turn` (以及空闲时的 next-step): 进入消息队列
    const bool startsNow = !turnActive && messageQueue_.empty();
    // 队列落库需要文本/模型副本 (pushMessageQueueItem 会 move 走)
    const std::string textCopy  = input.text;
    const std::string modelCopy = input.model;
    const auto        itemId    = pushMessageQueueItem(
        std::move(input.text),
        std::move(input.model),
        std::move(input.attachments),
        std::string{delivery}
    );
    persistInputAdmitted(
        MessageQueueItem{
            .id          = itemId,
            .text        = textCopy,
            .model       = modelCopy,
            .attachments = {},
            .createdAtMs = 0,
            .delivery    = std::string{delivery},
            .recovered   = false,
        },
        input.requestId
    );
    sendInputAck(
        sender,
        input.requestId,
        delivery,
        startsNow ? InputStatus::Started : InputStatus::Queued,
        {},
        startsNow ? "turn started" : "queued behind the running turn",
        itemId
    );
}

void SessionServerAgentIO::flushCollectWindow() {
    if (collectPending_.empty()) {
        return;
    }
    auto pending = std::move(collectPending_);
    collectPending_.clear();

    // 合并: 文本按输入顺序用换行拼接, 附件累加, 模型取最后一个非空
    std::string                         mergedText;
    std::string                         mergedModel;
    std::vector<MediaAttachment>        mergedAttachments;
    for (auto& entry : pending) {
        if (!mergedText.empty()) {
            mergedText += "\n";
        }
        mergedText += entry.text;
        if (!entry.model.empty()) {
            mergedModel = entry.model;
        }
        for (auto& att : entry.attachments) {
            mergedAttachments.push_back(std::move(att));
        }
    }
    const bool turnActive = turnActive_.load(std::memory_order_acquire);
    const bool startsNow  = !turnActive && messageQueue_.empty();
    // 收件箱落库需要文本副本 (pushMessageQueueItem 会 move 走)
    const std::string mergedTextCopy = mergedText;

    const auto itemId = pushMessageQueueItem(
        std::move(mergedText),
        std::move(mergedModel),
        std::move(mergedAttachments),
        std::string{InputDelivery::NextTurn}
    );
    persistInputAdmitted(
        MessageQueueItem{
            .id          = itemId,
            .text        = mergedTextCopy,
            .model       = {},
            .attachments = {},
            .createdAtMs = 0,
            .delivery    = std::string{InputDelivery::NextTurn},
            .recovered   = false,
        },
        pending.empty() ? 0 : pending.front().requestId
    );

    // 合并结果按条目逐条回执: 每个请求都能得到"已开始/已排队 + 合并条目 id"
    for (auto& entry : pending) {
        sendInputAck(
            entry.sender,
            entry.requestId,
            InputDelivery::Collect,
            startsNow ? InputStatus::Started : InputStatus::Queued,
            {},
            fmt::format("merged with {} other collected input(s)", pending.size() - 1),
            itemId
        );
    }
    XX_LOGI(
        "[session_ctrl] collected {} input(s) merged into one turn (session={})",
        pending.size(),
        config_.sessionId
    );
}

void SessionServerAgentIO::persistInputAdmitted(const MessageQueueItem& item, uint64_t requestId) {
    if (!item.id.empty()) {
        // 记录"本实例已受理": 启动恢复时据此跳过本进程刚受理、尚未轮到执行的条目
        // (它们不是上个进程的遗留, 不该被当成待确认项而暂停队列)
        locallyAdmittedInputs_.insert(item.id);
    }
    auto agent = agent_.lock();
    if (!agent || !agent->agentContext || !agent->agentContext->sessions
        || !agent->agentContext->sessions->sessionStore || item.id.empty()) {
        XX_LOGD(
            "[session_ctrl] inbox disabled for input {} (persistence off or invalid id)",
            item.id
        );
        return;
    }
    auto store = agent->agentContext->sessions->sessionStore;
    SessionStore::SessionInputRecord record;
    record.id       = item.id;
    record.delivery = item.delivery;
    record.status   = std::string{SessionStore::SessionInputStatus::Admitted};
    record.createdMs = item.createdAtMs > 0
                           ? item.createdAtMs
                           : static_cast<int64_t>(
                                 std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch()
                                 )
                                     .count()
                             );
    // 载荷只保留元数据 (文本/模型/投递模式/附件元数据): 附件 Base64 不进库,
    // 恢复后由服务端按 pathOrUrl 重新读取, 避免会话库被大块 Base64 撑大
    utilxx_base::Json payload{
        {"text",     item.text    },
        {"model",    item.model   },
        {"delivery", item.delivery},
    };
    if (requestId != 0) {
        payload["requestId"] = requestId;
    }
    if (!item.attachments.empty()) {
        utilxx_base::Json arr = utilxx_base::Json::array();
        for (const auto& att : item.attachments) {
            auto meta = att;
            meta.dataUrl.clear();
            arr.push_back(meta.toJson());
        }
        payload["attachments"] = std::move(arr);
    }
    record.payload = utilxx_base::Json(payload).dump();
    auto sess      = session();
    record.admittedSeq = sess ? static_cast<int64_t>(sess->deltaSeq) : 0;
    store->addSessionInput(config_.sessionId, record);
    XX_LOGD(
        "[session_ctrl] inbox admitted input {} (session={}, delivery={})",
        record.id,
        config_.sessionId,
        record.delivery
    );
}

void SessionServerAgentIO::persistInputStatus(
    std::string_view id,
    std::string_view status,
    uint64_t         promotedSeq
) {
    if (id.empty()) {
        return;
    }
    auto agent = agent_.lock();
    if (!agent || !agent->agentContext || !agent->agentContext->sessions
        || !agent->agentContext->sessions->sessionStore) {
        return;
    }
    auto store = agent->agentContext->sessions->sessionStore;
    if (status == SessionStore::SessionInputStatus::Promoted) {
        store->markSessionInputPromoted(config_.sessionId, id, promotedSeq);
    } else {
        store->setSessionInputStatus(config_.sessionId, id, status);
    }
}

void SessionServerAgentIO::recoverPendingInputs() {
    auto agent = agent_.lock();
    if (!agent || !agent->agentContext || !agent->agentContext->sessions
        || !agent->agentContext->sessions->sessionStore) {
        return;
    }
    auto store = agent->agentContext->sessions->sessionStore;
    auto rows  = store->listSessionInputs(
        config_.sessionId,
        SessionStore::SessionInputStatus::Admitted
    );
    if (rows.empty()) {
        return;
    }
    size_t recoveredCount = 0;
    for (auto& row : rows) {
        // 跳过"本进程已受理"的条目: 它们可能只是还没轮到执行 (启动恢复与
        // 客户端首次输入可能交错), 不应被当成待确认项而暂停队列
        if (locallyAdmittedInputs_.count(row.id) != 0) {
            continue;
        }
        {
            // 队列里已在等待的条目同理 (防御: 恢复只在启动时执行一次)
            const bool queued = std::any_of(
                messageQueue_.begin(),
                messageQueue_.end(),
                [&](const MessageQueueItem& it) {
                    return it.id == row.id;
                }
            );
            if (queued) {
                continue;
            }
        }
        MessageQueueItem item;
        item.id = row.id;
        item.delivery = row.delivery.empty() ? std::string{InputDelivery::NextTurn} : row.delivery;
        item.recovered = true;
        item.createdAtMs = row.createdMs;
        // 载荷解析失败只跳过该条 (历史脏数据不应阻断会话启动)
        agentxx::util::catchError<bool>(
            [&]() -> bool {
                auto payload = utilxx_base::Json::parse(row.payload);
                item.text    = payload.value("text", std::string{});
                item.model   = payload.value("model", std::string{});
                if (payload.contains("attachments") && payload["attachments"].is_array()) {
                    for (const auto& att : payload["attachments"]) {
                        item.attachments.push_back(MediaAttachment::fromJson(att));
                    }
                }
                return true;
            },
            [&](std::string errmsg) -> bool {
                XX_LOGW(
                    "[session_ctrl] recovered input {} payload unreadable ({}): {}",
                    row.id,
                    config_.sessionId,
                    errmsg
                );
                return false;
            }
        );
        if (item.text.empty() && item.attachments.empty()) {
            store->setSessionInputStatus(
                config_.sessionId,
                row.id,
                SessionStore::SessionInputStatus::Dropped
            );
            continue;
        }
        messageQueue_.push_back(std::move(item));
        ++recoveredCount;
    }
    // 恢复只在本端点启动时执行一次: 之后不再需要"本进程已受理"的记录 (有界内存)
    locallyAdmittedInputs_.clear();
    if (recoveredCount == 0) {
        return;
    }
    // 恢复的输入不自动执行: 置暂停由用户确认 (发新输入解除暂停, 或删除条目),
    // 避免进程重启后重放副作用
    setQueueState(SessionQueueState::Paused, "recovered pending inputs");
    XX_LOGW(
        "[session_ctrl] recovered {} pending input(s) after restart; queue paused until user "
        "confirms (session={})",
        recoveredCount,
        config_.sessionId
    );
    sendMessageQueueUpdate();
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
    // 连接阶段校验 (计划 PRO-5): 传输报告"尚未握手"时, 除 hello 之外的业务消息
    // 一律按 InvalidState 拒绝并给出原因, 而不是按正常流程处理或静默丢弃。
    // - 进程内 Channel 传输恒为 ready, 因此同进程宿主的行为不变
    // - WS 传输在对端 hello 被接受之前为 unhandshaken
    if (sender && !std::holds_alternative<WireHello>(msg)
        && sender->stage() == WireConnectionStage::Unhandshaken) {
        XX_LOGW(
            "[session_ctrl] reject business message before handshake (session={})",
            config_.sessionId
        );
        sendToClient(
            sender,
            WireError{
                .code    = WireErrorCode::InvalidState,
                .message = "handshake not completed for this connection; send hello first",
            }
        );
        return;
    }

    std::visit(
        [this, &sender](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, WireHello>) {
                handleHello(m, {}, sender);
            } else if constexpr (std::is_same_v<T, WireUserInput>) {
                // 输入受理 (投递模式分发 + 回执 + 收件箱落库, 见 LOOP-1/2/3/11)
                handleUserInput(std::move(m), sender);
            } else if constexpr (std::is_same_v<T, WireCancel>) {
                if (!acceptSessionScope(m.sessionId, sender, "cancel")) {
                    return;
                }
                // 仅在轮次进行中时暂停队列: 空闲时收到取消 (无轮次可取消) 不应
                // 置位暂停, 否则后续所有新输入都会因队列被误暂停而永远等待执行
                if (turnActive_.load(std::memory_order_acquire)) {
                    setQueueState(SessionQueueState::Paused, "cancel");
                }
                onCancel();
            } else if constexpr (std::is_same_v<T, WireInterruptAndRunNext>) {
                if (!acceptSessionScope(m.sessionId, sender, "interrupt_and_run_next")) {
                    return;
                }
                interruptAndRunNext();
            } else if constexpr (std::is_same_v<T, WireGetViewMessages>) {
                // 客户端历史分页请求: 切片 [max(0, before-count), before) 回应。
                // viewMessages 为 append-only, 绝对下标恒定, 轮次进行中追加
                // 新消息不影响既有下标, 无竞态; 全程 ex_ 线程 (= Session io 线程)
                // (会话不匹配由 handleGetViewMessages 回空页处理: 客户端按页解析,
                //  回错误会打断其分页状态机)
                handleGetViewMessages(m, sender);
            } else if constexpr (std::is_same_v<T, WireClearMessageQueue>) {
                if (!acceptSessionScope(m.sessionId, sender, "clear_message_queue")) {
                    return;
                }
                clearMessageQueue();
            } else if constexpr (std::is_same_v<T, WireRemoveQueueItem>) {
                if (!acceptSessionScope(m.sessionId, sender, "remove_queue_item")) {
                    return;
                }
                if (!removeQueueItem(m.itemId)) {
                    // 条目已不在 (被清空/已受理/重复删除): 明确告知对端, 不静默成功
                    auto err = WireError{
                        .code    = WireErrorCode::MessageNotFound,
                        .message = fmt::format(
                            "queue item '{}' not found (already removed or submitted)",
                            m.itemId
                        ),
                    };
                    if (sender) {
                        sendToClient(sender, std::move(err));
                    } else {
                        sendToPeer(std::move(err));
                    }
                }
            } else if constexpr (std::is_same_v<T, WireSelectModel>) {
                if (!acceptSessionScope(m.sessionId, sender, "select_model")) {
                    return;
                }
                auto agent = agent_.lock();
                if (agent) {
                    agent->selectModel(m.sessionId, m.model);
                }
            } else if constexpr (std::is_same_v<T, WireInterruptResponse>) {
                resolveInterrupt(m.id, std::move(m.result));
            } else if constexpr (std::is_same_v<T, WireGetModel>) {
                if (!acceptSessionScope(m.sessionId, sender, "get_model")) {
                    return;
                }
                if (!agent_.lock()) {
                    return;
                }
                sendToClient(sender, buildModelInfo(m.sessionId));
            } else if constexpr (std::is_same_v<T, WireAddModel>) {
                handleAddModel(m, sender);
            } else if constexpr (std::is_same_v<T, WireGetAppendComponentInfo>) {
                if (!acceptSessionScope(m.sessionId, sender, "get_append_component_info")) {
                    return;
                }
                auto agent = agent_.lock();
                if (!agent) {
                    return;
                }
                // 客户端拉取加载的组件信息: 收集已加载的 MCP/Skill/Memory 并回填
                std::vector<AppendComponentNotification> notifications;
                agent->collectAppendComponentInfo(notifications);
                sendToClient(sender, WireAppendComponentInfo{std::move(notifications)});
            } else if constexpr (std::is_same_v<T, WireGetContext>) {
                if (!acceptSessionScope(m.sessionId, sender, "get_context")) {
                    return;
                }
                auto              agent = agent_.lock();
                auto              sess  = session();
                // 上下文取自会话 (唯一权威); llmMessagesJson() 为惰性生成的 Json 形态
                utilxx_base::Json msgs  = utilxx_base::Json::array();
                if (sess) {
                    msgs = sess->llmMessagesJson();
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
                        sysMsg["role"]            = "system";
                        sysMsg["content"]         = std::move(sysPrompt);
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
                if (!acceptSessionScope(m.sessionId, sender, "compact_context")) {
                    return;
                }
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
                                    co_return listSessionsFor(req, *sessionStore);
                                }
                            );
                        } else {
                            resp = listSessionsFor(req, *sessionStore);
                        }
                        self->sendToClient(sender, std::move(resp));
                    },
                    asio::detached
                );
            } else if constexpr (std::is_same_v<T, WireRenameSession>) {
                // 会话重命名 (计划 RET-1a): 标题写入会话库 meta.title 并把来源标为
                // 用户命名 (此后不再被自动标题覆盖); 结果回执让客户端区分成功与原因
                handleRenameSession(m, sender);
            } else if constexpr (std::is_same_v<T, WireListDir>) {
                // 目录列举不带 sessionId (只读服务端文件系统, 与会话无关)
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

                            // 路径字符串在客户端/服务端之间一律以 UTF-8 传递
                            // (Windows 下 path::string() 为本地代码页): 访问文件
                            // 系统前经 [utf8ToPath] 转换
                            std::string targetDir = req.path;
                            if (targetDir.empty()) {
                                if (agent && agent->agentContext) {
                                    targetDir = agent->agentContext->getSessionWorkDir(
                                        self->config_.sessionId
                                    );
                                }
                                if (targetDir.empty()) {
                                    std::error_code ec;
                                    targetDir
                                        = utilxx_base::pathToUtf8Generic(
                                            std::filesystem::current_path(ec)
                                        );
                                }
                            }

                            std::error_code ec;
                            auto            canonical
                                = std::filesystem::canonical(utilxx_base::utf8ToPath(targetDir), ec);
                            if (ec) {
                                result.ok    = false;
                                result.error = ec.message();
                                return result;
                            }
                            result.currentDir = utilxx_base::pathToUtf8Generic(canonical);
                            if (canonical.has_parent_path()
                                && canonical.parent_path() != canonical) {
                                result.parentDir
                                    = utilxx_base::pathToUtf8Generic(canonical.parent_path());
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
                                std::string filename
                                    = utilxx_base::pathToUtf8Generic(entry.path().filename());
                                if (!filename.empty() && filename[0] == '.') {
                                    continue;
                                }

                                if (std::filesystem::is_directory(status)) {
                                    WireDirEntry de;
                                    de.name     = filename;
                                    de.fullPath = utilxx_base::pathToUtf8Generic(entry.path());
                                    de.isDir    = true;
                                    de.supported = true;
                                    dirs.push_back(std::move(de));
                                } else if (std::filesystem::is_regular_file(status)) {
                                    auto ext = utilxx_base::toLower(
                                        utilxx_base::pathToUtf8Generic(entry.path().extension())
                                    );
                                    auto mt = agentxx::agent::mediaTypeFromExtension(ext);
                                    if (!mt.has_value()) {
                                        continue;
                                    }
                                    WireDirEntry de;
                                    de.name     = filename;
                                    de.fullPath = utilxx_base::pathToUtf8Generic(entry.path());
                                    de.isDir    = false;
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
            } else if constexpr (std::is_same_v<T, WireGetPermissionState>) {
                // 客户端查询权限状态 (TUI Info 侧边栏的授权按钮初值/刷新)
                WirePermissionState state;
                state.fullAuth = fullAuthorized();
                sendToClient(sender, std::move(state));
            } else if constexpr (std::is_same_v<T, WireSetFullAuth>) {
                // 客户端切换"完全授权所有权限":
                // - 立即向本端点客户端广播新状态 (界面即时生效; 幂等)
                // - 权限中间件同时发布状态变更事件 (见 subscribePermissionEvents),
                //   供其他会话端点/其他客户端同步; 询问卡片勾选路径只走该事件
                if (setFullAuthorized(m.fullAuth)) {
                    sendToPeer(WirePermissionState{m.fullAuth});
                }
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

    // 协议版本协商 (计划 PRO-3): 对端版本更高时明确拒绝, 不静默降级
    // - 0 = 老客户端未声明版本: 按最低兼容版本继续 (向后兼容)
    // - 高于本服务端版本: 新客户端可能依赖本端不认识的消息/字段, 继续跑会出现
    //   难以排查的行为, 因此回 ok=false + 可读原因
    if (hello.protocolVersion > agentxx::agent::WireProtocol::kVersion) {
        XX_LOGW(
            "[session_ctrl] hello rejected: client protocol version {} > server {}",
            hello.protocolVersion,
            agentxx::agent::WireProtocol::kVersion
        );
        WireHelloAck reject;
        reject.ok        = false;
        reject.sessionId = config_.sessionId;
        reject.error     = fmt::format(
            "client protocol version {} is newer than server version {}; "
            "please update the server side",
            hello.protocolVersion,
            agentxx::agent::WireProtocol::kVersion
        );
        reject.protocolVersion = agentxx::agent::WireProtocol::kVersion;
        reject.capabilities    = agentxx::agent::serverWireCapabilities();
        if (sender) {
            // 连接阶段保持"未握手": 该对端不能发业务消息 (见 onPeerMessage 的阶段校验)
            sender->setStage(
                agentxx::agent::WireConnectionStage::Unhandshaken,
                "hello rejected: protocol version newer than server"
            );
            sendToClient(sender, WireMessage{std::move(reject)});
        } else {
            sendToPeer(WireMessage{std::move(reject)});
        }
        return;
    }
    if (hello.protocolVersion == 0) {
        // 老客户端: 按最低兼容版本处理 (能力按未声明降级)
        XX_LOGD("[session_ctrl] hello without protocol version: treating as legacy client");
    } else {
        XX_LOGD(
            "[session_ctrl] hello protocol version {} capabilities={}",
            hello.protocolVersion,
            hello.capabilities.size()
        );
    }

    if (!hello.language.empty()) {
        auto agent = agent_.lock();
        if (agent) {
            // 配置显式指定会话语言时不被客户端界面语言覆盖 (客户端界面语言属
            // 各端本机偏好, 会话语言属可分发配置; 见 AgentContext::isLanguageExplicit)
            if (agent->isLanguageExplicit()) {
                XX_LOGD(
                    "SessionServerAgentIO: session language '{}' from config, ignore client "
                    "language '{}'",
                    agent->getLanguage(config_.sessionId),
                    hello.language
                );
            } else {
                agent->setLanguage(hello.language, config_.sessionId);
            }
        }
    }

    std::vector<WireDelta>            replayDeltas;
    std::optional<WireSyncPayload>    replaySync;
    /// 客户端 lastSeq 已超出 delta 缓冲: 必须下发一份快照 (全量或增量补拉)
    bool                              deltaBufferMissed = false;
    std::string                       tailHash;
    std::vector<WireInterruptRequest> pendingInterrupts;

    auto sess = session();
    tailHash  = sess ? sess->getHashInfo().tailHex : std::string{};

    // 同步策略 (按代价从低到高, 先满足者胜出):
    // ① delta 重放: 实时重连 (客户端 lastSeq 仍在缓冲内) 时传输量最小
    // ② 增量补拉 (计划 STO-4): 客户端给出已持有的展示历史序号时只补差量
    // ③ 全量/尾窗同步: 首次接入或序列不匹配时的兜底
    if (hello.lastSeq > 0) {
        auto deltas = deltasSince(hello.lastSeq);
        if (deltas.has_value()) {
            replayDeltas = std::move(deltas.value());
        } else {
            deltaBufferMissed = true;
        }
        // 缓冲溢出 (客户端离线过久): 落到 ②/③ 处理, 不再无条件下发全量
        // 但必须给出快照 (客户端在等): 由下方兜底保证
    }

    // 增量补拉 (计划 STO-4): 客户端给出已持有的展示历史序号时只补差量
    // - 优先于全量/尾窗同步, 但只有在序号落在服务端历史范围内时可用
    //   (序号超前说明客户端持有的是另一份历史, 如服务端会话被重建/换会话)
    // - 差量条数超过 [kIncrementalReplayMaxMessages] 时回退常规同步,
    //   避免长时间离线后一次传输过大
    // - 需要会话库可读 (序号是持久化序号): 内存模式 (无 SessionStore) 回退常规同步
    if (replayDeltas.empty() && hello.afterViewSeq > 0 && sess
        && hello.afterViewSeq <= sess->lastViewSeq()) {
        auto agent = agent_.lock();
        auto store = (agent && agent->agentContext && agent->agentContext->sessions)
                         ? agent->agentContext->sessions->sessionStore
                         : nullptr;
        if (store) {
            auto rows = store->loadViewMessagesAfter(
                config_.sessionId,
                hello.afterViewSeq,
                kIncrementalReplayMaxMessages + 1
            );
            if (rows.size() <= kIncrementalReplayMaxMessages) {
                replaySync = buildIncrementalSync(
                    std::move(rows),
                    static_cast<uint64_t>(hello.afterViewSeq)
                );
            } else {
                XX_LOGD(
                    "[session_ctrl] incremental replay too large ({} rows > {}), fall back to "
                    "full/tail sync (session={})",
                    rows.size(),
                    kIncrementalReplayMaxMessages,
                    config_.sessionId
                );
            }
        }
    }

    // 兜底: 首次接入或 ①/② 均不可用 → 按 initialSyncTailCount 决定全量或尾窗同步。
    // 尾窗同步时客户端仅持有末尾窗口, 上方更早历史由其分页拉取
    // (WireGetViewMessages), 避免长会话恢复时全量传输
    // - delta 缓冲溢出 (deltaBufferMissed) 时客户端在等一份快照, 即使当前没有
    //   消息也必须下发 (空全量), 否则客户端握手后一直等不到同步
    if (!replaySync.has_value() && replayDeltas.empty()
        && (deltaBufferMissed || hello.lastSeq == 0)) {
        if (sess && sess->viewMessageCount() > 0) {
            replaySync = buildTailSync(config_.initialSyncTailCount);
        } else if (deltaBufferMissed) {
            replaySync = buildFullSync();
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
    // 协议版本与能力声明 (计划 PRO-3): 客户端据此判断可用功能 (如 afterViewSeq 补拉)
    helloAck.protocolVersion = agentxx::agent::WireProtocol::kVersion;
    helloAck.capabilities    = agentxx::agent::serverWireCapabilities();
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

    // 握手完成 (计划 PRO-5): 该传输进入可收发业务消息的阶段
    if (sender) {
        sender->setStage(agentxx::agent::WireConnectionStage::Ready, "hello accepted");
    }

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

    // 权限状态随握手下发: 客户端接入即知道当前是否已完全授权 (Info 侧边栏
    // 授权按钮的初值; 后续变更由权限状态事件广播保持同步)
    // - 仅在 agent 装配了权限中间件时下发: 该状态本就由权限中间件持有, 未装配
    //   (如无 agent 的纯协议端点) 时下发一个恒 false 的状态没有意义, 且会让
    //   "握手后按序读消息" 的对端多收一条
    if (permissionMiddleware() != nullptr) {
        WirePermissionState permState;
        permState.fullAuth = fullAuthorized();
        doSend(std::move(permState));
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
    // 消息队列与待合并输入重置 (旧会话的排队输入不再适用于新会话)
    messageQueue_.clear();
    collectPending_.clear();
    setQueueState(SessionQueueState::Idle, "session switched");
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
    // 权限状态变更订阅 (完全授权切换广播)
    subscribePermissionEvents(bus);
    // 发布宿主约定事件 client_attached: 双端插件据此重发当前状态快照,
    // 修复"status 等一次性事件先于本订阅发布而丢失 → 客户端滞留初始占位"
    // 的问题 (晚创建的控制器/晚接入的客户端由此获得快照)
    publishHostEvent(
        bus,
        kEvtClientAttached,
        fmt::format(R"({{"sessionId":"{}"}})", config_.sessionId)
    );
}

void SessionServerAgentIO::subscribePermissionEvents(
    const std::shared_ptr<agentxx::events::EventBus>& bus
) {
    if (!bus) {
        return;
    }
    auto self = shared_from_this();
    fullAuthSubId_
        = bus->get<events::EventPermissionFullAuthChanged>(events::Topic::PermissionFullAuth)
              .subscribe(
                  [weakSelf = std::weak_ptr<SessionServerAgentIO>{self}](
                      const events::EventPermissionFullAuthChanged& evt
                  ) -> asio::awaitable<void> {
                      auto sp = weakSelf.lock();
                      if (!sp) {
                          co_return;
                      }
                      // 回调运行在 bus executor; post 到 ex_ 统一串行化端点状态
                      // 访问 (与插件事件转发同一处理方式)
                      asio::post(sp->ex_, [sp, fullAuth = evt.fullAuth] {
                          sp->sendToPeer(WirePermissionState{fullAuth});
                      });
                      co_return;
                  }
              );
}

agentxx::middleware::PermissionMiddlewareHandle* SessionServerAgentIO::permissionMiddleware() const {
    auto agent = agent_.lock();
    if (!agent || !agent->agentContext || !agent->agentContext->middlewareHandleContext) {
        return nullptr;
    }
    for (auto& handle : agent->agentContext->middlewareHandleContext->handles) {
        if (auto* permission
            = dynamic_cast<agentxx::middleware::PermissionMiddlewareHandle*>(handle.get())) {
            return permission;
        }
    }
    return nullptr;
}

bool SessionServerAgentIO::fullAuthorized() const {
    auto* permission = permissionMiddleware();
    return permission != nullptr && permission->isFullAuthorized();
}

bool SessionServerAgentIO::setFullAuthorized(bool authorized) {
    auto* permission = permissionMiddleware();
    if (permission == nullptr) {
        XX_LOGW("[session_ctrl] set full-auth ignored: permission middleware not assembled");
        return false;
    }
    // 权限中间件为 agent 级 (跨会话共享): 状态变更会发布事件, 由本端点
    // (以及其他会话端点) 的订阅广播给各自客户端
    permission->setFullAuthorized(authorized);
    XX_LOGI("[session_ctrl] full authorization set to {}", authorized ? "true" : "false");
    return true;
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
    // 持久化收件箱恢复 (计划 LOOP-1): 未投递的输入作为"待确认"回到队列并暂停,
    // 不自动执行 —— 进程重启后重放副作用的代价高于"少跑一轮"
    recoverPendingInputs();
    while (!stopped_.load(std::memory_order_acquire)) {
        if (pendingInsert_) {
            pendingInsert_ = false;
            setQueueState(SessionQueueState::Draining, "insert after interrupt");
        }

        if (isQueueHalted() || messageQueue_.empty()) {
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
        setQueueState(
            queueState_ == SessionQueueState::Draining ? SessionQueueState::Draining
                                                       : SessionQueueState::Running,
            "turn started"
        );
        // 收件箱: 该条目已进入执行路径 (重启后不再作为"待确认"恢复)
        persistInputStatus(
            currentItem.id,
            SessionStore::SessionInputStatus::Promoted,
            session() ? session()->deltaSeq : 0
        );

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
                .message = std::make_shared<ViewMessage>(std::move(vm)),
                .type    = WireDelta::Type::InsertMessage,
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
                    .errorMessage = turnResult.errorMessage,
                    .hasError     = turnResult.hasError,
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
                    .errorMessage = std::move(errmsg),
                    .hasError     = true,
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
                    .errorMessage = std::move(errmsg),
                    .hasError     = true,
                    .interrupted  = false,
                });
                return false;
            }
        );

        // 仅当正常执行成功一轮后，才继续自动发送消息队列中的消息
        if (!turnResult.hasError && !turnResult.interrupted) {
            setQueueState(
                messageQueue_.empty() ? SessionQueueState::Idle : SessionQueueState::Running,
                "turn completed"
            );
        } else {
            setQueueState(SessionQueueState::Paused, "turn failed or interrupted");
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
    // 待合并输入丢弃 (客户端已断开, 无需回执)
    collectPending_.clear();
    collectTimer_->cancel();
    onCancel();
    // 退订插件事件前缀 (防止端点析构后回调悬垂)
    if (pluginSubId_ != 0) {
        if (auto agent = agent_.lock(); agent && agent->agentContext && agent->agentContext->bus) {
            agent->agentContext->bus->unlistenPrefix(pluginSubId_);
        }
        pluginSubId_ = 0;
    }
    // 退订权限状态变更事件 (同上)
    if (fullAuthSubId_ != 0) {
        if (auto agent = agent_.lock(); agent && agent->agentContext && agent->agentContext->bus) {
            agent->agentContext->bus
                ->get<events::EventPermissionFullAuthChanged>(events::Topic::PermissionFullAuth)
                .unsubscribe(fullAuthSubId_);
        }
        fullAuthSubId_ = 0;
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
    // 客户端记录的序号超过服务端当前 seq: 服务端进程重启/会话重建后 seq 从 0 重新
    // 计数, 按增量续传只能得到空列表 (客户端将永远收不到新增量, 界面不再刷新),
    // 故回退全量 sync (客户端据 sync 的 deltaSeq 重置序号)
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
        // 快照序号: 客户端据此重置去重用的序号 (服务端 seq 可能已重新计数)
        p.deltaSeq    = sess->deltaSeq;
        p.lastViewSeq = sess->lastViewSeq();
    }
    p.messageQueue = std::vector<MessageQueueItem>(messageQueue_.begin(), messageQueue_.end());
    p.queueState   = std::string{sessionQueueStateText(queueState_)};
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
    p.queueState   = std::string{sessionQueueStateText(queueState_)};
        return p;
    }
    const size_t total = sess->viewMessageCount();
    // 窗口起始下标: 总数不足窗口大小时从 0 开始 (此时等价全量)
    const size_t start = (total > tailCount) ? (total - tailCount) : 0;
    p.fromIndex        = start;
    p.totalMessages    = total;
    p.messages         = sess->getViewMessagesRange(start, total);
    p.tailHash         = sess->getHashInfo().tailHex;
    // 快照序号: 客户端据此重置去重用的序号 (服务端 seq 可能已重新计数)
    p.deltaSeq     = sess->deltaSeq;
    p.lastViewSeq  = sess->lastViewSeq();
    p.messageQueue = std::vector<MessageQueueItem>(messageQueue_.begin(), messageQueue_.end());
    p.queueState   = std::string{sessionQueueStateText(queueState_)};
    return p;
}

WireSyncPayload SessionServerAgentIO::buildIncrementalSync(
    std::vector<SequencedViewMessage> rows,
    uint64_t                          afterSeq
) {
    WireSyncPayload p;
    p.incremental = true;
    auto sess     = session();
    if (!sess) {
        // 会话不可用 (已释放): 回退全量同步语义 (客户端整体重置)
        return buildFullSync();
    }
    // 老数据 (无 msg id) 无法在客户端去重: 回退全量同步
    for (const auto& row : rows) {
        if (row.message.id.empty()) {
            XX_LOGD(
                "[session_ctrl] incremental replay unavailable: message without id (seq={}, session={})",
                row.seq,
                config_.sessionId
            );
            return buildFullSync();
        }
    }
    const auto total = sess->viewMessageCount();
    p.messages.reserve(rows.size());
    for (auto& row : rows) {
        p.messages.push_back(std::move(row.message));
    }
    // 追加位置: 本批消息在服务端完整 viewMessages 中的起始绝对下标
    // (增量补拉时必然位于末尾, 条数不超过服务端总量)
    p.fromIndex     = total > p.messages.size() ? total - p.messages.size() : 0;
    p.totalMessages = total;
    p.tailHash      = sess->getHashInfo().tailHex;
    p.deltaSeq      = sess->deltaSeq;
    p.lastViewSeq   = sess->lastViewSeq();
    p.messageQueue  = std::vector<MessageQueueItem>(messageQueue_.begin(), messageQueue_.end());
    p.queueState    = std::string{sessionQueueStateText(queueState_)};
    XX_LOGD(
        "[session_ctrl] incremental replay {} message(s) after seq {} (session={})",
        p.messages.size(),
        afterSeq,
        config_.sessionId
    );
    return p;
}

std::string SessionServerAgentIO::currentTailHash() {
    auto sess = session();
    return sess ? sess->getHashInfo().tailHex : std::string{};
}

void SessionServerAgentIO::handleRenameSession(
    const WireRenameSession&                     req,
    const std::shared_ptr<AgentIOTransportBase>& sender
) {
    auto reply = [this, &sender](WireRenameSessionResult result) {
        if (sender) {
            sendToClient(sender, WireMessage{std::move(result)});
        } else {
            sendToPeer(WireMessage{std::move(result)});
        }
    };

    // 会话 id 为空 = 当前绑定会话 (与其它请求同口径); 非空时允许改列表里的任意会话
    const std::string sessionId = req.sessionId.empty() ? config_.sessionId : req.sessionId;
    const std::string title     = normalizeSessionTitle(req.title);

    WireRenameSessionResult result;
    result.sessionId = sessionId;

    if (title.empty()) {
        result.error = "session title must not be empty";
        reply(std::move(result));
        return;
    }

    auto agent = agent_.lock();
    auto store = (agent && agent->agentContext && agent->agentContext->sessions)
                     ? agent->agentContext->sessions->sessionStore
                     : nullptr;
    if (!store) {
        result.error = "session store is not available (persistence disabled)";
        reply(std::move(result));
        return;
    }
    // 不存在的会话不建目录: 改名是"给已有会话起名", 不是新建会话的入口
    if (!store->sessionDataDirExists(sessionId)) {
        result.error = fmt::format("session '{}' does not exist", sessionId);
        reply(std::move(result));
        return;
    }
    if (!store->setSessionTitle(sessionId, title)) {
        result.error = "failed to persist the session title";
        reply(std::move(result));
        return;
    }

    XX_LOGI(
        "[session_ctrl] session '{}' renamed to '{}'",
        sessionId,
        title
    );
    result.ok    = true;
    result.title = title;
    reply(std::move(result));
}

bool SessionServerAgentIO::acceptSessionScope(
    std::string_view                             sessionId,
    const std::shared_ptr<AgentIOTransportBase>& sender,
    std::string_view                             what
) {
    // 空 sessionId = 未指定: 按当前绑定会话处理 (旧客户端兼容, 语义与拆分前一致)
    if (sessionId.empty() || sessionId == config_.sessionId) {
        return true;
    }
    XX_LOGW(
        "SessionServerAgentIO: reject '{}' for session '{}' (endpoint bound to '{}')",
        what,
        sessionId,
        config_.sessionId
    );
    sendToClient(
        sender,
        WireError{
            .code    = WireErrorCode::SessionMismatch,
            .message = fmt::format(
                "request '{}' targets session '{}' but this endpoint serves '{}'",
                what,
                sessionId,
                config_.sessionId
            ),
        }
    );
    return false;
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

void SessionServerAgentIO::handleAddModel(
    const WireAddModel&                          req,
    const std::shared_ptr<AgentIOTransportBase>& sender
) {
    WireAddModelResult result;
    result.name = req.name;

    auto agent = agent_.lock();
    if (!agent || !agent->agentContext || !agent->agentContext->agentConfig) {
        result.error = "agent 尚未就绪, 无法添加模型";
        sendToClient(sender, std::move(result));
        return;
    }
    auto              cfg = agent->agentContext->agentConfig;
    const ModelConfig mc  = agentxx::agent::io::addModelToConfig(req);

    // 校验: 与客户端表单同一套规则 (客户端已先校验一次, 此处是权威校验)
    std::set<std::string, std::less<>> existing;
    for (const auto& entry : cfg->availableModels) {
        existing.insert(entry.first);
    }
    if (auto check = validateNewModelConfig(mc, existing); !check.has_value()) {
        result.error = check.error();
        sendToClient(sender, std::move(result));
        return;
    }

    // 先落盘再注册: 写盘失败时不注册, 避免"本次可用、重启后消失"的半生效状态
    // (配置文件为几 KB 级同步写, 直接在本线程完成, 不卸载到线程池)
    const std::string configPath = modelConfigYamlPath(cfg->dataDir);
    if (auto written = appendModelConfigToYamlFile(configPath, mc); !written.has_value()) {
        XX_LOGE("[model] add model '{}' failed: {}", mc.name, written.error());
        result.error = written.error();
        sendToClient(sender, std::move(result));
        return;
    }

    // 注册到运行时 (模型选择弹窗列表 + 切换模型都取自这里)
    cfg->availableModels[mc.name] = mc;
    if (agent->agentContext->modelRegistry) {
        agent->agentContext->modelRegistry->registerModel(mc.name, mc);
    }
    // 保存成功即使用: 发起请求的会话立即切换为新模型
    agent->selectModel(req.sessionId, mc.name);

    result.ok = true;
    sendToClient(sender, std::move(result));
    // 模型列表已变化: 回推模型信息刷新客户端弹窗列表 (其他客户端在其下次请求时刷新)
    sendToClient(sender, buildModelInfo(req.sessionId));
    XX_LOGI(
        "[model] added model '{}' (config: {}), session '{}' switched to it",
        mc.name,
        configPath,
        req.sessionId
    );
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
