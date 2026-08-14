#include "agentxx/agent/io/session_server_agent_io.h"

#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/session_persistence.h"
#include "agentxx/middlewares/permission.h"
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

namespace agentxx {
namespace agent {

SessionServerAgentIO::SessionServerAgentIO(
    asio::any_io_executor    ex,
    std::weak_ptr<BaseAgent> agent,
    Config                   config
) :
    ex_(std::move(ex)),
    agent_(std::move(agent)),
    config_(std::move(config)),
    inputChannel_(std::make_shared<InputChannel>(ex_, 64)) {}

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
    co_return co_await waitInput();
}

asio::awaitable<std::optional<std::string>> SessionServerAgentIO::waitInput() {
    co_return co_await agentxx::util::catchErrorToOptionalAsync<std::string>(
        [&]() -> asio::awaitable<std::optional<std::string>> {
            co_return co_await inputChannel_->async_receive(asio::use_awaitable);
        }
    );
}

asio::awaitable<neograph::json> SessionServerAgentIO::handleInterrupt(
    std::string_view /*threadId*/,
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
        .id       = id,
        .threadId = config_.threadId,
        .node     = std::string{interruptNode},
        .value    = std::string{interruptValue},
        .argJson  = std::string{interruptArgJson},
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
            sendToPeer(WireInterruptExpired{id, config_.threadId});
        }
    }
    co_return result;
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
                inputChannel_->try_send(ErrorCode{}, m.text);
            } else if constexpr (std::is_same_v<T, WireCancel>) {
                onCancel();
            } else if constexpr (std::is_same_v<T, WireSelectModel>) {
                auto agent = agent_.lock();
                if (agent) {
                    agent->selectModel(m.threadId, m.model);
                }
            } else if constexpr (std::is_same_v<T, WireInterruptResponse>) {
                resolveInterrupt(m.id, std::move(m.result));
            } else if constexpr (std::is_same_v<T, WireGetModel>) {
                auto agent = agent_.lock();
                if (!agent) {
                    return;
                }
                std::string              currentModel = agent->getCurrentModelName(m.threadId);
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
                // 目录扫描 + SQLite 读取属阻塞 I/O, 卸载到 blockingPool 执行,
                // 避免阻塞 agent io 线程; 完成后经 shared_from_this 回填响应
                auto agent = agent_.lock();
                if (!agent || !agent->agentContext || !agent->agentContext->sessionPersistence) {
                    sendToPeer(WireSessionList{});
                    return;
                }
                auto persistence = agent->agentContext->sessionPersistence;
                auto self        = shared_from_this();
                asio::co_spawn(
                    ex_,
                    [self, persistence, agent]() -> asio::awaitable<void> {
                        std::vector<SessionInfo> sessions;
                        if (agent->agentContext->blockingPool) {
                            sessions
                                = co_await agentxx::util::offloadAsync<std::vector<SessionInfo>>(
                                    *agent->agentContext->blockingPool,
                                    [persistence]() -> asio::awaitable<std::vector<SessionInfo>> {
                                        co_return persistence->listSessions();
                                    }
                                );
                        } else {
                            sessions = persistence->listSessions();
                        }
                        self->sendToPeer(WireSessionList{std::move(sessions)});
                    },
                    asio::detached
                );
            } else if constexpr (std::is_same_v<T, WireSwitchSession>) {
                // 客户端请求切换会话 (弹窗选择后); 运行态拦截由客户端前置完成
                switchSession(std::move(m.threadId));
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
            } else if constexpr (std::is_same_v<T, WireGetSystemUsage>) {
                // 客户端 (TUI) 周期请求系统资源占用 (CPU/内存/GPU):
                // 采集迁移到 agent-server 侧 (远端模式下展示 server 主机的资源;
                // 本地一体模式由本进程读取), 完成后回传 WireSystemUsage。
                // 与 WireListSessions 同模式: 查询卸载到 blockingPool 执行,
                // 避免占用 agent io 线程; 完成后经 shared_from_this 回传响应
                auto self = shared_from_this();
                asio::co_spawn(
                    ex_,
                    [self]() -> asio::awaitable<void> {
                        if (!self->sysMonitor_) {
                            self->sysMonitor_
                                = std::make_shared<agentxx::expand::CpuGpuMonitor>();
                        }
                        auto monitor = self->sysMonitor_;
                        auto usage   = std::make_shared<agentxx::expand::CpuGpuUsage>();
                        co_await agentxx::util::catchErrorAsync<bool>(
                            [&]() -> asio::awaitable<bool> {
                                // query() 为协程 (内部含 100ms 采样间隔定时器与
                                // 文件读取), 整体投递到 agent 的 blockingPool 线程池
                                // 执行, 完成后自动恢复回 ex_ 线程
                                auto agent = self->agent_.lock();
                                if (agent && agent->agentContext
                                    && agent->agentContext->blockingPool) {
                                    *usage = co_await agentxx::util::offloadAsync<
                                        agentxx::expand::CpuGpuUsage>(
                                        *agent->agentContext->blockingPool,
                                        [monitor]() -> asio::awaitable<
                                            agentxx::expand::CpuGpuUsage> {
                                            co_return co_await monitor->query();
                                        }
                                    );
                                } else {
                                    *usage = co_await monitor->query();
                                }
                                self->sendToPeer(WireSystemUsage{*usage});
                                co_return true;
                            },
                            [](std::string errmsg) -> asio::awaitable<bool> {
                                XX_LOGE("[session_ctrl] system usage query failed: {}", errmsg);
                                co_return false;
                            }
                        );
                        co_return;
                    },
                    asio::detached
                );
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
            replaySync = buildFullSync();
        }
    } else {
        if (sess && !sess->getFullHistoryCopy().empty()) {
            replaySync = buildFullSync();
        }
    }

    for (const auto& [id, p] : pending_) {
        pendingInterrupts.push_back(WireInterruptRequest{
            .id       = id,
            .threadId = config_.threadId,
            .node     = p.node,
            .value    = p.value,
            .argJson  = p.argJson,
        });
    }

    // 先发送 HelloAck 再重放: 客户端 connect() 握手循环会丢弃 HelloAck 之前的消息,
    // 若先重放后 HelloAck, 全量 Sync/增量 Delta 会被客户端丢弃 → 重连后历史丢失。
    // HelloAck 之后发送的重放消息经客户端 recvQueue 缓冲, 由 runTransportLoop 正常处理。
    sendToPeer(WireHelloAck{
        .ok       = true,
        .threadId = config_.threadId,
        .tailHash = std::move(tailHash),
        .models   = std::move(models),
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
}

void SessionServerAgentIO::onDisconnect() {
    if (turnActive_.load(std::memory_order_acquire)) {
        startGraceTimer();
    }
}

void SessionServerAgentIO::switchSession(std::string newThreadId) {
    if (newThreadId.empty() || newThreadId == config_.threadId) {
        // 空 id 非法; 同一会话无需切换 (历史已同步, 重复全量 Sync 反而闪烁)
        if (newThreadId == config_.threadId) {
            // 仍回推一次全量 Sync: 客户端可能因本地状态异常需要校准
            auto sync = buildFullSync();
            sendToPeer(std::move(sync));
            sendContextStats();
        }
        return;
    }
    if (turnActive_.load(std::memory_order_acquire)) {
        // 双重保护: 客户端已拦截运行态切换, 此处再兜底拒绝,
        // 避免轮次进行中被换走导致 Delta/输入错投到新会话
        XX_LOGW("[session_ctrl] switchSession rejected: turn active (thread={})", config_.threadId);
        return;
    }

    auto agent = agent_.lock();
    if (!agent || !agent->agentContext) {
        return;
    }

    const std::string oldThreadId = config_.threadId;
    config_.threadId              = newThreadId;
    // delta 重放缓冲属于旧会话的 seq 空间, 新会话 seq 独立编号, 清空避免错配重放
    deltaBuffer_.clear();
    // 新会话对当前连接而言等同于首次接入: 重置 firstTurn_ 使首条输入走
    // resume_if_exists=true 的恢复路径 (与会话重启恢复行为一致)
    firstTurn_ = true;

    XX_LOGI("[session_ctrl] switched session: {} -> {}", oldThreadId, config_.threadId);

    // 回推新会话状态: 全量 Sync (历史消息) + 模型信息 + 上下文统计
    auto sync = buildFullSync();
    sendToPeer(std::move(sync));

    std::string              currentModel = agent->getCurrentModelName(config_.threadId);
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
// 驱动循环
// ---------------------------------------------------------------------------

asio::awaitable<void> SessionServerAgentIO::run() {
    running_.store(true, std::memory_order_release);
    while (!stopped_.load(std::memory_order_acquire)) {
        auto input = co_await waitInput();
        if (!input.has_value()) {
            break;
        }
        if (input->empty()) {
            continue;
        }

        turnActive_.store(true, std::memory_order_release);

        auto agent = agent_.lock();
        if (!agent) {
            turnActive_.store(false, std::memory_order_release);
            break;
        }
        // catchErrorAsync: 取消类异常 (CancelledException/NodeInterrupt) 与普通异常
        // 一致转为错误消息通知客户端 (onRethrow), 避免异常逃逸 co_spawn 完成处理器;
        // 其余异常同样转为错误消息
        // 错误提示: agent 线程插入会话历史并发送 MessageTip Delta (覆盖
        // runConversationTurnAsync 自身抛异常的兜底路径, 与主路径提示一致)
        auto sendErrorTip = [&](std::string_view errmsg) {
            auto sess = session();
            if (!sess) {
                return;
            }
            auto vm          = ViewMessage::makeText(ViewMessage::Role::Tip, std::string{errmsg});
            vm.tip->tipLevel = ViewMessage::TipLevel::Error;
            const auto id    = sess->appendHistory(std::move(vm));
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
        co_await agentxx::util::catchErrorAsync<bool>(
            [&]() -> asio::awaitable<bool> {
                auto result = co_await agent->runConversationTurnAsync(
                    config_.threadId,
                    *input,
                    firstTurn_,
                    shared_from_this()
                );
                firstTurn_ = false;
                sendToPeer(WireTurnResult{
                    .threadId     = config_.threadId,
                    .hasError     = result.hasError,
                    .errorMessage = result.errorMessage,
                    .interrupted  = result.interrupted,
                });
                sendContextStats();
                co_return true;
            },
            [&](std::string errmsg) -> asio::awaitable<bool> {
                XX_LOGE("[session_ctrl] turn error: {}", errmsg);
                sendErrorTip(errmsg);
                sendToPeer(WireTurnResult{
                    .threadId     = config_.threadId,
                    .hasError     = true,
                    .errorMessage = std::move(errmsg),
                    .interrupted  = false,
                });
                co_return false;
            },
            [&](std::string& errmsg) -> std::optional<bool> {
                XX_LOGE("[session_ctrl] turn error: {}", errmsg);
                sendErrorTip(errmsg);
                sendToPeer(WireTurnResult{
                    .threadId     = config_.threadId,
                    .hasError     = true,
                    .errorMessage = std::move(errmsg),
                    .interrupted  = false,
                });
                return false;
            }
        );

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
    inputChannel_->close();
    onCancel();
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
        p.messages = sess->getFullHistoryCopy();
        p.tailHash = sess->getHashInfo().tailHex;
    }
    return p;
}

std::string SessionServerAgentIO::currentTailHash() {
    auto sess = session();
    return sess ? sess->getHashInfo().tailHex : std::string{};
}

void SessionServerAgentIO::sendContextStats() {
    auto sess = session();
    if (!sess || !sess->contextStats) {
        return;
    }
    auto ctxTokens = sess->contextStats->contextTokens.load(std::memory_order_relaxed);
    auto maxTokens = sess->contextStats->maxContextTokens.load(std::memory_order_relaxed);
    sendToPeer(WireContextStats{ctxTokens, maxTokens});
}

std::shared_ptr<Session> SessionServerAgentIO::session() {
    auto agent = agent_.lock();
    if (agent && agent->agentContext) {
        return agent->agentContext->getSession(config_.threadId);
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
                    self->config_.threadId
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
            sendToPeer(WireInterruptExpired{id, config_.threadId});
        }
        p.ch->close();
    }
    pending_.clear();
}

} // namespace agent
} // namespace agentxx
