#include "agentxx/agent/remote/session_controller.h"

#include "agentxx/agent/base_agent.h"
#include "agentxx/agent/context.h"
#include "agentxx/util/log.h"
#include "asio/cancel_after.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/dispatch.hpp"
#include "asio/redirect_error.hpp"
#include "asio/use_awaitable.hpp"
#include "neograph/graph/cancel.h"

namespace agentxx {
namespace agent {

SessionController::SessionController(
    asio::any_io_executor    ex,
    std::weak_ptr<BaseAgent> agent,
    Config                   config
) :
    ex_(std::move(ex)),
    agent_(std::move(agent)),
    config_(std::move(config)),
    inputChannel_(std::make_shared<InputChannel>(ex_, 64)) {}

SessionController::~SessionController() {
    stopImpl();
}

// ---------------------------------------------------------------------------
// AgentIOBase: BaseAgent 产出的事件, 经 transport 发给客户端
// ---------------------------------------------------------------------------

void SessionController::onDelta(const Delta& delta) {
    deltaBuffer_.push_back(delta);
    while (deltaBuffer_.size() > config_.deltaBufferCap) {
        deltaBuffer_.pop_front();
    }
    sendToPeer(delta);
}

void SessionController::onSync(const SyncPayload& payload) {
    sendToPeer(payload);
}

asio::awaitable<std::optional<std::string>> SessionController::getInput() {
    co_return co_await waitInput();
}

asio::awaitable<std::optional<std::string>> SessionController::waitInput() {
    try {
        co_return co_await inputChannel_->async_receive(asio::use_awaitable);
    } catch (...) {
        co_return std::nullopt;
    }
}

asio::awaitable<neograph::json> SessionController::handleInterrupt(
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

    neograph::json result = neograph::json::array();
    try {
        result = co_await ch->async_receive(asio::cancel_after(timeout, asio::use_awaitable));
    } catch (const boost::system::system_error& e) {
        XX_LOGW("[session_ctrl] interrupt #{} ended early: {}", id, e.what());
    }
    pending_.erase(id);
    co_return result;
}

// ---------------------------------------------------------------------------
// AgentIOBase: 对端 (客户端) 发来的消息分发
// ---------------------------------------------------------------------------

void SessionController::onPeerMessage(WireMessage msg) {
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
            }
        },
        std::move(msg)
    );
}

// ---------------------------------------------------------------------------
// 连接管理
// ---------------------------------------------------------------------------

void SessionController::handleHello(const WireHello& hello, std::vector<std::string> models) {
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

void SessionController::onDisconnect() {
    if (turnActive_.load(std::memory_order_acquire)) {
        startGraceTimer();
    }
}

void SessionController::resolveInterrupt(int64_t id, neograph::json result) {
    auto it = pending_.find(id);
    if (it != pending_.end()) {
        it->second.ch->try_send(ErrorCode{}, std::move(result));
    }
}

void SessionController::onCancel() {
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

asio::awaitable<void> SessionController::run() {
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
        try {
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
        } catch (const std::exception& e) {
            XX_LOGE("[session_ctrl] turn error: {}", e.what());
            sendToPeer(WireTurnResult{
                .threadId     = config_.threadId,
                .hasError     = true,
                .errorMessage = e.what(),
                .interrupted  = false,
            });
        }

        turnActive_.store(false, std::memory_order_release);
    }
    running_.store(false, std::memory_order_release);
}

void SessionController::stop() {
    if (stopped_.load(std::memory_order_acquire)) {
        return;
    }
    asio::dispatch(ex_, [self = shared_from_this()]() {
        self->stopImpl();
    });
}

void SessionController::stopImpl() {
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

std::optional<std::vector<Delta>> SessionController::deltasSince(uint64_t seq) {
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

SyncPayload SessionController::buildFullSync() {
    SyncPayload p;
    p.fromIndex = 0;
    auto sess   = session();
    if (sess) {
        p.messages = sess->getFullHistoryCopy();
        p.tailHash = sess->getHashInfo().tailHex;
    }
    return p;
}

std::string SessionController::currentTailHash() {
    auto sess = session();
    return sess ? sess->getHashInfo().tailHex : std::string{};
}

void SessionController::sendContextStats() {
    auto sess = session();
    if (!sess || !sess->contextStats) {
        return;
    }
    auto ctxTokens = sess->contextStats->contextTokens.load(std::memory_order_relaxed);
    auto maxTokens = sess->contextStats->maxContextTokens.load(std::memory_order_relaxed);
    sendToPeer(WireContextStats{ctxTokens, maxTokens});
}

std::shared_ptr<Session> SessionController::session() {
    auto agent = agent_.lock();
    if (agent && agent->agentContext) {
        return agent->agentContext->getSession(config_.threadId);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// grace / pending
// ---------------------------------------------------------------------------

void SessionController::startGraceTimer() {
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

void SessionController::cancelGraceTimer() {
    auto t = std::move(graceTimer_);
    graceTimer_.reset();
    if (t) {
        t->cancel();
    }
}

void SessionController::failAllPending() {
    for (auto& [id, p] : pending_) {
        p.ch->close();
    }
    pending_.clear();
}

} // namespace agent
} // namespace agentxx
