#include "agentxx/agent/remote/session_controller.h"

#include "agentxx/agent/context.h"
#include "agentxx/agent/deepagent.h"
#include "agentxx/agent/remote/wire_protocol.h"
#include "agentxx/util/log.h"
#include "asio/cancel_after.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/redirect_error.hpp"
#include "asio/use_awaitable.hpp"
#include "neograph/graph/cancel.h"

namespace agentxx {
namespace agent {
namespace remote {

SessionController::SessionController(
    asio::any_io_executor    ex,
    std::weak_ptr<DeepAgent> agent,
    Config                   config
) :
    ex_(std::move(ex)),
    agent_(std::move(agent)),
    config_(std::move(config)),
    inputChannel_(std::make_shared<InputChannel>(ex_, 64)) {}

SessionController::~SessionController() {
    stop();
}

// ---------------------------------------------------------------------------
// AgentIOBase
// ---------------------------------------------------------------------------

void SessionController::onDelta(const Delta& delta) {
    // onDelta 可能来自 engine 线程; 在 bufferMutex_ 下完成"记录+推送",
    // 与 attach 的重放串行化, 保证 delta 到达顺序与 seq 一致 (避免 grace 重挂时乱序)
    std::lock_guard<std::mutex> lock(bufferMutex_);
    deltaBuffer_.push_back(delta);
    while (deltaBuffer_.size() > config_.deltaBufferCap) {
        deltaBuffer_.pop_front();
    }
    std::shared_ptr<IConnectionSink> conn;
    {
        std::lock_guard<std::mutex> lock2(connMutex_);
        conn = activeConn_.lock();
    }
    if (conn && conn->alive()) {
        conn->pushMessage(makeDeltaMsg(delta));
    }
}

void SessionController::onSync(const SyncPayload& payload) {
    pushToActive(makeSyncMsg(payload));
}

asio::awaitable<std::optional<std::string>> SessionController::getInput() {
    co_return co_await waitInput();
}

asio::awaitable<std::optional<std::string>> SessionController::waitInput() {
    try {
        co_return co_await inputChannel_->async_receive(asio::use_awaitable);
    } catch (const boost::system::system_error&) {
        co_return std::nullopt; // channel 关闭 (stop)
    }
}

asio::awaitable<neograph::json> SessionController::handleInterrupt(
    const std::string& /*threadId*/,
    const std::string& interruptNode,
    const std::string& interruptValue,
    const std::string& interruptArgJson
) {
    // 请求级超时: 权限询问与一般中断分别配置
    auto timeout
        = (interruptNode == "permission") ? config_.permissionTimeout : config_.interruptTimeout;

    auto    ch = std::make_shared<RespChannel>(ex_, 1);
    int64_t id;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        id           = nextReqId_++;
        pending_[id] = PendingInterrupt{ch, interruptNode, interruptValue, interruptArgJson};
    }

    pushToActive(
        makeInterruptRequest(id, config_.threadId, interruptNode, interruptValue, interruptArgJson)
    );

    neograph::json result = neograph::json::array();
    try {
        result = co_await ch->async_receive(asio::cancel_after(timeout, asio::use_awaitable));
    } catch (const boost::system::system_error& e) {
        // 超时 / 断线 (resp channel 被 close)
        XX_LOGW("[session_ctrl] interrupt #{} ended early: {}", id, e.what());
    }
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending_.erase(id);
    }
    co_return result;
}

// ---------------------------------------------------------------------------
// 驱动循环
// ---------------------------------------------------------------------------

asio::awaitable<void> SessionController::run() {
    running_.store(true, std::memory_order_release);
    while (!stopped_.load(std::memory_order_acquire)) {
        auto input = co_await waitInput();
        if (!input.has_value()) {
            break; // stop
        }
        if (input->empty()) {
            continue;
        }

        turnActive_.store(true, std::memory_order_release);
        auto cancelToken = std::make_shared<neograph::graph::CancelToken>();
        {
            std::lock_guard<std::mutex> lock(cancelMutex_);
            currentCancel_ = cancelToken;
        }

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
            pushToActive(makeTurnResult(
                config_.threadId,
                result.hasError,
                result.errorMessage,
                result.interrupted
            ));
            sendContextStats();
        } catch (const std::exception& e) {
            XX_LOGE("[session_ctrl] turn error: {}", e.what());
            pushToActive(makeTurnResult(config_.threadId, true, e.what(), false));
        }

        turnActive_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(cancelMutex_);
            currentCancel_.reset();
        }
    }
    running_.store(false, std::memory_order_release);
}

void SessionController::stop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true)) {
        return;
    }
    cancelGraceTimer();
    failAllPending();
    inputChannel_->close();
    onCancel();
}

// ---------------------------------------------------------------------------
// 连接管理
// ---------------------------------------------------------------------------

void SessionController::attach(
    const std::shared_ptr<IConnectionSink>& conn,
    uint64_t                                lastSeq,
    const std::string& /*tailHash*/
) {
    cancelGraceTimer();
    // 在 bufferMutex_ 下设置活动连接并重放, 与 onDelta 的实时推送串行化 (保证顺序)
    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        {
            std::lock_guard<std::mutex> lock2(connMutex_);
            activeConn_ = conn;
        }

        auto     sess   = session();
        uint64_t curSeq = sess ? sess->deltaSeq : 0;
        if (lastSeq > 0) {
            auto deltas = deltasSinceLocked(lastSeq);
            if (deltas.has_value()) {
                for (const auto& d : deltas.value()) {
                    conn->pushMessage(makeDeltaMsg(d));
                }
            } else {
                conn->pushMessage(makeSyncMsg(buildFullSync(), curSeq));
            }
        } else {
            // 新连接到已有历史的会话 -> 全量 sync 供客户端展示
            if (sess && !sess->fullHistory.empty()) {
                conn->pushMessage(makeSyncMsg(buildFullSync(), curSeq));
            }
        }
    }

    // 非 delta 消息 (统计/挂起中断) 无需与 delta 严格有序, 在 bufferMutex_ 外推送
    sendContextStats();
    resendPendingInterrupts(conn);
}

void SessionController::detach(IConnectionSink* conn) {
    bool wasActive = false;
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        auto                        active = activeConn_.lock();
        if (active.get() == conn) {
            activeConn_.reset();
            wasActive = true;
        }
    }
    if (wasActive && turnActive_.load(std::memory_order_acquire)) {
        // 轮次进行中断线 -> 启动 grace period (期内重连可重挂)
        startGraceTimer();
    }
    // 空闲断线: 驱动循环继续在 waitInput 等待, 不取消轮次
}

void SessionController::onUserInput(std::string text) {
    cancelGraceTimer();
    inputChannel_->try_send(ErrorCode{}, std::move(text));
}

void SessionController::resolveInterrupt(int64_t id, neograph::json result) {
    std::shared_ptr<RespChannel> ch;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        auto                        it = pending_.find(id);
        if (it != pending_.end()) {
            ch = it->second.ch;
        }
    }
    if (ch) {
        ch->try_send(ErrorCode{}, std::move(result));
    }
}

void SessionController::onCancel() {
    std::shared_ptr<neograph::graph::CancelToken> tok;
    {
        std::lock_guard<std::mutex> lock(cancelMutex_);
        tok = currentCancel_;
    }
    if (tok) {
        tok->cancel();
    }
}

// ---------------------------------------------------------------------------
// 推送 / 缓冲
// ---------------------------------------------------------------------------

void SessionController::pushToActive(neograph::json msg) {
    std::shared_ptr<IConnectionSink> conn;
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        conn = activeConn_.lock();
    }
    if (conn && conn->alive()) {
        conn->pushMessage(std::move(msg));
    }
}

std::optional<std::vector<Delta>> SessionController::deltasSinceLocked(uint64_t seq) {
    // 调用方已持有 bufferMutex_
    if (deltaBuffer_.empty()) {
        return std::nullopt; // 空缓冲无法确认连续性 -> 回退全量 sync
    }
    uint64_t oldest = deltaBuffer_.front().seq;
    if (seq + 1 < oldest) {
        return std::nullopt; // 缓冲已不覆盖, 需全量 sync
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
        p.messages = sess->fullHistory;
        p.tailHash = sess->chainHash.tailHex();
    }
    return p;
}

std::string SessionController::currentTailHash() {
    auto sess = session();
    return sess ? sess->chainHash.tailHex() : std::string{};
}

void SessionController::sendContextStats() {
    auto sess = session();
    if (!sess || !sess->contextStats) {
        return;
    }
    pushToActive(makeContextStats(
        sess->contextStats->contextTokens.load(std::memory_order_relaxed),
        sess->contextStats->maxContextTokens.load(std::memory_order_relaxed)
    ));
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
        // 无 grace: 立即取消轮次
        onCancel();
        failAllPending();
        return;
    }
    auto timer = std::make_shared<asio::steady_timer>(ex_);
    timer->expires_after(config_.gracePeriod);
    {
        std::lock_guard<std::mutex> lock(graceMutex_);
        graceTimer_ = timer;
    }
    auto self = shared_from_this();
    asio::co_spawn(
        ex_,
        [self, timer]() -> asio::awaitable<void> {
            ErrorCode ec;
            co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                co_return; // 被取消 (已重连)
            }
            bool hasConn;
            {
                std::lock_guard<std::mutex> lock(self->connMutex_);
                hasConn = !self->activeConn_.expired();
            }
            if (!hasConn && self->turnActive_.load(std::memory_order_acquire)) {
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
    std::shared_ptr<asio::steady_timer> t;
    {
        std::lock_guard<std::mutex> lock(graceMutex_);
        t = std::move(graceTimer_);
        graceTimer_.reset();
    }
    if (t) {
        t->cancel();
    }
}

void SessionController::failAllPending() {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    for (auto& [id, p] : pending_) {
        p.ch->close();
    }
    pending_.clear();
}

void SessionController::resendPendingInterrupts(const std::shared_ptr<IConnectionSink>& conn) {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    for (const auto& [id, p] : pending_) {
        conn->pushMessage(makeInterruptRequest(id, config_.threadId, p.node, p.value, p.argJson));
    }
}

} // namespace remote
} // namespace agent
} // namespace agentxx
