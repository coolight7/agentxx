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
                // 目录扫描 + SQLite 读取属阻塞 I/O, 卸载到 blockingPool 执行,
                // 避免阻塞 agent io 线程; 完成后经 shared_from_this 回填响应
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
                    [self, sessionStore, agent]() -> asio::awaitable<void> {
                        std::vector<SessionInfo> sessions;
                        if (agent->agentContext->blockingPool) {
                            sessions
                                = co_await agentxx::util::offloadAsync<std::vector<SessionInfo>>(
                                    *agent->agentContext->blockingPool,
                                    [sessionStore]() -> asio::awaitable<std::vector<SessionInfo>> {
                                        co_return sessionStore->listSessions();
                                    }
                                );
                        } else {
                            sessions = sessionStore->listSessions();
                        }
                        self->sendToPeer(WireSessionList{std::move(sessions)});
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
            replaySync = buildFullSync();
        }
    } else {
        if (sess && !sess->getFullViewMessagesCopy().empty()) {
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
    // 若先重放后 HelloAck, 全量 Sync/增量 Delta 会被客户端丢弃 → 重连后历史丢失。
    // HelloAck 之后发送的重放消息经客户端 recvQueue 缓冲, 由 runTransportLoop 正常处理。
    sendToPeer(WireHelloAck{
        .ok        = true,
        .sessionId = config_.sessionId,
        .tailHash  = std::move(tailHash),
        .models    = std::move(models),
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
    if (newThreadId.empty() || newThreadId == config_.sessionId) {
        // 空 id 非法; 同一会话无需切换 (历史已同步, 重复全量 Sync 反而闪烁)
        if (newThreadId == config_.sessionId) {
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
    // 新会话对当前连接而言等同于首次接入: 重置 firstTurn_ 使首条输入走
    // resume_if_exists=true 的恢复路径 (与会话重启恢复行为一致)
    firstTurn_ = true;

    XX_LOGI("[session_ctrl] switched session: {} -> {}", oldThreadId, config_.sessionId);

    // 回推新会话状态: 全量 Sync (历史消息) + 模型信息 + 上下文统计
    auto sync = buildFullSync();
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
    pluginSubId_ = bus->subscribePrefix(
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
}

// ---------------------------------------------------------------------------
// 驱动循环
// ---------------------------------------------------------------------------

asio::awaitable<void> SessionServerAgentIO::run() {
    running_.store(true, std::memory_order_release);
    // 订阅插件事件 (转发 WirePluginData 供客户端展示插件状态)
    subscribePluginEvents();
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
        co_await agentxx::util::catchErrorAsync<bool>(
            [&]() -> asio::awaitable<bool> {
                auto result
                    = co_await agent
                          ->runTurnAsync(config_.sessionId, *input, firstTurn_, shared_from_this());
                firstTurn_ = false;
                sendToPeer(WireTurnResult{
                    .sessionId    = config_.sessionId,
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
                sendToPeer(WireTurnResult{
                    .sessionId    = config_.sessionId,
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
    // 退订插件事件前缀 (防止端点析构后回调悬垂)
    if (pluginSubId_ != 0) {
        if (auto agent = agent_.lock(); agent && agent->agentContext && agent->agentContext->bus) {
            agent->agentContext->bus->unsubscribePrefix(pluginSubId_);
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
        p.messages = sess->getFullViewMessagesCopy();
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
