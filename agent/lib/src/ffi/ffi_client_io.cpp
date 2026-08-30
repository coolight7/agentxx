#include "ffi_client_io.h"

#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include <thread>
#include <utility>

namespace agentxx {
namespace ffi {

FfiClientAgentIO::FfiClientAgentIO(asio::any_io_executor ex, AgentxxCallbacks callbacks) :
    ex_(std::move(ex)),
    callbacks_(callbacks) {}

FfiClientAgentIO::~FfiClientAgentIO() {
    failAllPendingInterrupts();
}

void FfiClientAgentIO::setSessionId(std::string sessionId) {
    sessionId_ = std::move(sessionId);
}

void FfiClientAgentIO::setClientThreadId(std::thread::id tid) {
    clientThreadId_ = tid;
}

void FfiClientAgentIO::setAgentThreadId(std::thread::id tid) {
    agentThreadId_ = tid;
}

bool FfiClientAgentIO::isOnClientThread() const {
    return clientThreadId_ != std::thread::id{} && std::this_thread::get_id() == clientThreadId_;
}

bool FfiClientAgentIO::isOnAgentThread() const {
    return agentThreadId_ != std::thread::id{} && std::this_thread::get_id() == agentThreadId_;
}

void FfiClientAgentIO::notifyServerReady() {
    if (isOnClientThread()) {
        onServerReady();
    } else {
        asio::post(ex_, [self = shared_from_this()]() {
            self->onServerReady();
        });
    }
}

void FfiClientAgentIO::notifyError(int code, std::string message) {
    auto emit = [this, code, message = std::move(message)]() {
        neograph::json j = neograph::json::object();
        j["code"]        = code;
        j["message"]     = message;
        emitEvent(AGENTXX_EVT_ERROR, dump(j));
    };
    if (isOnClientThread()) {
        emit();
    } else {
        asio::post(ex_, std::move(emit));
    }
}

// ---------------------------------------------------------------------------
// AgentIOBase 纯虚实现
// ---------------------------------------------------------------------------

asio::awaitable<std::optional<std::string>> FfiClientAgentIO::getInput() {
    co_return std::nullopt;
}

asio::awaitable<neograph::json> FfiClientAgentIO::handleInterrupt(
    std::string_view /*sessionId*/,
    std::string_view /*interruptNode*/,
    std::string_view /*interruptValue*/,
    std::string_view /*interruptArgJson*/
) {
    co_return neograph::json::array();
}

// ---------------------------------------------------------------------------
// FFI 应答通道 (Lock-Free 无锁设计)
// ---------------------------------------------------------------------------

bool FfiClientAgentIO::hasPendingInterrupt(int64_t interruptId) const {
    return interruptId > 0
           && currentPendingInterruptId_.load(std::memory_order_acquire) == interruptId;
}

bool FfiClientAgentIO::submitInterruptResponse(int64_t interruptId, neograph::json values) {
    int64_t expected = interruptId;
    if (!currentPendingInterruptId_
             .compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
        return false;
    }
    auto it = pending_.find(interruptId);
    if (it == pending_.end()) {
        return false;
    }
    // try_send: 容量 1, 宿主重复应答会被丢弃 (以首次为准) 并释放 channel
    it->second->try_send(ErrorCode{}, std::move(values));
    return true;
}

void FfiClientAgentIO::failAllPendingInterrupts() {
    currentPendingInterruptId_.store(0, std::memory_order_release);
    for (auto& [id, ch] : pending_) {
        if (ch) {
            ch->close();
        }
    }
    pending_.clear();
}

// ---------------------------------------------------------------------------
// 被动接收回调
// ---------------------------------------------------------------------------

void FfiClientAgentIO::onDelta(const agent::WireDelta& delta) {
    emitEvent(AGENTXX_EVT_DELTA, dump(agent::io::makeDeltaMsg(delta)));
}

void FfiClientAgentIO::onSync(const agent::WireSyncPayload& payload) {
    emitEvent(AGENTXX_EVT_SYNC, dump(agent::io::syncToJson(payload)));
}

void FfiClientAgentIO::onTurnResult(const agent::WireTurnResult& result) {
    emitEvent(
        AGENTXX_EVT_TURN_END,
        dump(agent::io::makeTurnResult(
            result.sessionId,
            result.hasError,
            result.errorMessage,
            result.interrupted,
            result.startTimeMs,
            result.durationMs
        ))
    );
}

void FfiClientAgentIO::onContextStats(const agent::WireContextStats& stats) {
    if (stats.contextTokens == 0 && stats.maxContextTokens == 0 && stats.tps <= 0.0) {
        return;
    }
    emitEvent(
        AGENTXX_EVT_CONTEXT_STATS,
        dump(agent::io::makeContextStats(stats.contextTokens, stats.maxContextTokens, stats.tps))
    );
}

void FfiClientAgentIO::onServerReady() {
    neograph::json j = neograph::json::object();
    j["sessionId"]   = sessionId_;
    emitEvent(AGENTXX_EVT_READY, dump(j));
}

void FfiClientAgentIO::onPeerMessage(agent::WireMessage msg) {
    std::visit(
        [this](auto&& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, agent::WireInterruptRequest>) {
                auto    ch   = std::make_shared<RespChannel>(ex_, 1);
                int64_t id   = m.id;
                pending_[id] = ch;
                currentPendingInterruptId_.store(id, std::memory_order_release);

                // 事件: 完整中断信息
                neograph::json j = neograph::json::object();
                j["interruptId"] = id;
                j["sessionId"]   = m.sessionId;
                j["node"]        = m.node;
                j["value"]       = m.value;
                j["argJson"]     = m.argJson;
                emitEvent(AGENTXX_EVT_INTERRUPT_REQ, dump(j));

                // 挂起等待宿主 agentxx_interrupt_respond
                auto self = shared_from_this();
                asio::co_spawn(
                    ex_,
                    [self, ch, id, sessionId = m.sessionId]() mutable -> asio::awaitable<void> {
                        auto [answered, result] = co_await self->waitHostInterrupt(id, ch);
                        if (answered) {
                            self->sendToPeer(agent::WireInterruptResponse{id, std::move(result)});
                        }
                        co_return;
                    },
                    asio::detached
                );
            } else if constexpr (std::is_same_v<T, agent::WireInterruptExpired>) {
                int64_t expected = m.id;
                currentPendingInterruptId_
                    .compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
                auto it = pending_.find(m.id);
                if (it != pending_.end()) {
                    it->second->close();
                    pending_.erase(it);
                }
                neograph::json j = neograph::json::object();
                j["interruptId"] = m.id;
                emitEvent(AGENTXX_EVT_INTERRUPT_EXPIRED, dump(j));
            } else if constexpr (std::is_same_v<T, agent::WireModelInfo>) {
                auto j = agent::io::makeModelInfo(m.currentModel, m.models);
                emitEvent(AGENTXX_EVT_MODEL_INFO, dump(j));
                if (onSyncReply) {
                    onSyncReply(SyncKind::ModelInfo, std::move(j));
                }
            } else if constexpr (std::is_same_v<T, agent::WireContextMessages>) {
                auto j = agent::io::makeContextMessages(m.messages);
                if (onSyncReply) {
                    onSyncReply(SyncKind::ContextMessages, std::move(j));
                }
            } else if constexpr (std::is_same_v<T, agent::WireSessionList>) {
                auto j = agent::io::makeSessionList(m.sessions, m.totalCount, m.hasMore);
                if (onSyncReply) {
                    onSyncReply(SyncKind::SessionList, std::move(j));
                }
            } else if constexpr (std::is_same_v<T, agent::WireAppendComponentInfo>) {
                emitEvent(
                    AGENTXX_EVT_COMPONENTS,
                    dump(agent::io::makeAppendComponentInfo(m.notifications))
                );
            } else if constexpr (std::is_same_v<T, agent::WirePluginData>) {
                emitEvent(AGENTXX_EVT_PLUGIN_DATA, dump(agent::io::makePluginData(m)));
            } else if constexpr (std::is_same_v<T, agent::WireError>) {
                neograph::json j = neograph::json::object();
                j["code"]        = m.code;
                j["message"]     = m.message;
                emitEvent(AGENTXX_EVT_ERROR, dump(j));
            } else {
                agent::AgentIOBase::onPeerMessage(agent::WireMessage{std::move(m)});
            }
        },
        std::move(msg)
    );
}

// ---------------------------------------------------------------------------
// 内部
// ---------------------------------------------------------------------------

asio::awaitable<std::pair<bool, neograph::json>>
    FfiClientAgentIO::waitHostInterrupt(int64_t id, std::shared_ptr<RespChannel> ch) {
    neograph::json result  = neograph::json::array();
    bool           gotResp = false;
    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
            result  = co_await ch->async_receive(asio::use_awaitable);
            gotResp = true;
            co_return true;
        },
        [&](std::string errmsg) -> asio::awaitable<bool> {
            XX_LOGW("[ffi] interrupt #{} ended early: {}", id, errmsg);
            co_return false;
        }
    );
    int64_t expected = id;
    currentPendingInterruptId_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    pending_.erase(id);
    co_return std::make_pair(gotResp, std::move(result));
}

void FfiClientAgentIO::emitEvent(AgentxxEventType type, std::string json) {
    if (callbacks_.on_event == nullptr) {
        return; // headless
    }
    try {
        callbacks_.on_event(type, json.c_str(), callbacks_.user_data);
    } catch (const std::exception& e) {
        XX_LOGE("[ffi] on_event callback threw: {}", e.what());
    } catch (...) {
        XX_LOGE("[ffi] on_event callback threw unknown exception");
    }
}

std::string FfiClientAgentIO::dump(const neograph::json& j) {
    return j.dump();
}

} // namespace ffi
} // namespace agentxx
