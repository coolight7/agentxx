#include "agentxx/agent/io/embedded_client_agent_io.h"

#include "agentxx/util/exception.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "asio/use_awaitable.hpp"
#include "utilxx_base/log.h"
#include <utility>

namespace agentxx {
namespace agent {

EmbeddedClientAgentIO::EmbeddedClientAgentIO(asio::any_io_executor ex) :
    ex_(std::move(ex)) {}

EmbeddedClientAgentIO::~EmbeddedClientAgentIO() {
    // 上层对象已销毁: 挂起的中断不再有人应答, 直接失败掉 (避免协程帧卡在 channel 上)
    failAllPendingInterrupts();
}

void EmbeddedClientAgentIO::setSessionId(std::string sessionId) {
    sessionId_ = std::move(sessionId);
}

void EmbeddedClientAgentIO::setClientThreadId(std::thread::id tid) {
    clientThreadId_ = tid;
}

void EmbeddedClientAgentIO::setAgentThreadId(std::thread::id tid) {
    agentThreadId_ = tid;
}

bool EmbeddedClientAgentIO::isOnClientThread() const {
    return clientThreadId_ != std::thread::id{} && std::this_thread::get_id() == clientThreadId_;
}

bool EmbeddedClientAgentIO::isOnAgentThread() const {
    return agentThreadId_ != std::thread::id{} && std::this_thread::get_id() == agentThreadId_;
}

void EmbeddedClientAgentIO::initSelf(const std::shared_ptr<EmbeddedClientAgentIO>& self) {
    self_ = self;
}

void EmbeddedClientAgentIO::postToClientThread(std::function<void()> fn) {
    if (!fn) {
        return;
    }
    if (isOnClientThread()) {
        fn();
        return;
    }
    asio::post(ex_, std::move(fn));
}

// ---------------------------------------------------------------------------
// AgentIOBase 实现
// ---------------------------------------------------------------------------

asio::awaitable<std::optional<std::string>> EmbeddedClientAgentIO::getInput() {
    // 嵌入式端点的输入由上层主动投递, 不从这里拉取; 驱动循环不会用本返回值
    co_return std::nullopt;
}

asio::awaitable<utilxx_base::Json> EmbeddedClientAgentIO::handleInterrupt(
    std::string_view /*sessionId*/,
    std::string_view /*interruptNode*/,
    std::string_view /*interruptValue*/,
    std::string_view /*interruptArgJson*/
) {
    // 本端点不注册会话总线: HIL 走 onPeerMessage → awaitInterruptResponse
    co_return utilxx_base::Json::array();
}

void EmbeddedClientAgentIO::onRuntimeStopping() {
    failAllPendingInterrupts();
}

// ---------------------------------------------------------------------------
// 中断应答
// ---------------------------------------------------------------------------

void EmbeddedClientAgentIO::setCurrentPendingInterrupt(int64_t id) {
    currentPendingInterruptId_.store(id, std::memory_order_release);
}

bool EmbeddedClientAgentIO::hasPendingInterrupt(int64_t interruptId) const {
    return interruptId > 0
           && currentPendingInterruptId_.load(std::memory_order_acquire) == interruptId;
}

std::shared_ptr<EmbeddedClientAgentIO::RespChannel>
    EmbeddedClientAgentIO::beginInterrupt(int64_t id) {
    // 容量 1: 上层重复应答被丢弃 (以首次为准)
    auto ch      = std::make_shared<RespChannel>(ex_, 1);
    pending_[id] = ch;
    setCurrentPendingInterrupt(id);
    return ch;
}

bool EmbeddedClientAgentIO::submitInterruptResponse(
    int64_t            interruptId,
    utilxx_base::Json  values
) {
    int64_t expected = interruptId;
    if (!currentPendingInterruptId_.compare_exchange_strong(
            expected,
            0,
            std::memory_order_acq_rel
        )) {
        return false;
    }
    auto it = pending_.find(interruptId);
    if (it == pending_.end()) {
        return false;
    }
    it->second->try_send(utilxx_base::AsioErrorCode{}, std::move(values));
    return true;
}

void EmbeddedClientAgentIO::expireInterrupt(int64_t id) {
    int64_t expected = id;
    currentPendingInterruptId_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    auto it = pending_.find(id);
    if (it != pending_.end()) {
        it->second->close();
        pending_.erase(it);
    }
}

void EmbeddedClientAgentIO::failAllPendingInterrupts() {
    currentPendingInterruptId_.store(0, std::memory_order_release);
    for (auto& [id, ch] : pending_) {
        if (ch) {
            ch->close();
        }
    }
    pending_.clear();
    flushDeltaBatch();
}

asio::awaitable<void> EmbeddedClientAgentIO::awaitInterruptResponse(
    int64_t                                    id,
    std::shared_ptr<EmbeddedClientAgentIO::RespChannel> ch
) {
    utilxx_base::Json result  = utilxx_base::Json::array();
    bool              gotResp = false;
    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
            result  = co_await ch->async_receive(asio::use_awaitable);
            gotResp = true;
            co_return true;
        },
        [&](std::string errmsg) -> asio::awaitable<bool> {
            XX_LOGW("[io] interrupt #{} ended early: {}", id, errmsg);
            co_return false;
        }
    );
    int64_t expected = id;
    currentPendingInterruptId_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    pending_.erase(id);
    if (gotResp) {
        sendToPeer(agent::WireInterruptResponse{id, std::move(result)});
    }
    co_return;
}

// ---------------------------------------------------------------------------
// 增量合批
// ---------------------------------------------------------------------------

void EmbeddedClientAgentIO::setDeltaBatch(int32_t maxDelayMs) {
    deltaBatchMs_ = maxDelayMs;
    if (deltaBatchMs_ <= 0) {
        flushDeltaBatch();
        if (deltaBatchTimer_) {
            deltaBatchTimer_->cancel();
        }
    } else if (!deltaBatchTimer_) {
        deltaBatchTimer_ = std::make_shared<asio::steady_timer>(ex_);
    }
}

void EmbeddedClientAgentIO::flushDeltaBatch() {
    if (pendingDeltaText_.empty()) {
        return;
    }
    auto kind = std::move(pendingDeltaKind_);
    auto text = std::move(pendingDeltaText_);
    pendingDeltaKind_.clear();
    pendingDeltaText_.clear();
    emitDeltaText(kind, text);
}

void EmbeddedClientAgentIO::onDelta(const agent::WireDelta& delta) {
    using T = agent::WireDelta::Type;
    if (deltaBatchMs_ > 0 && (delta.type == T::TextToken || delta.type == T::ThinkToken)) {
        const std::string kind = (delta.type == T::TextToken) ? "text" : "thinking";
        if (!pendingDeltaText_.empty() && pendingDeltaKind_ != kind) {
            flushDeltaBatch();
        }
        pendingDeltaKind_  = kind;
        pendingDeltaText_ += delta.text;
        if (deltaBatchTimer_) {
            deltaBatchTimer_->expires_after(std::chrono::milliseconds(deltaBatchMs_));
            deltaBatchTimer_->async_wait([self = self_](const utilxx_base::AsioErrorCode& ec) {
                if (ec) {
                    return;
                }
                if (auto sp = self.lock()) {
                    sp->flushDeltaBatch();
                }
            });
        }
        return;
    }
    flushDeltaBatch();
    emitDeltaEvent(delta);
}

} // namespace agent
} // namespace agentxx
