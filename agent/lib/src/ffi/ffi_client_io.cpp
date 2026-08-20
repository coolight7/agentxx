#include "ffi_client_io.h"

#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
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

void FfiClientAgentIO::setAgentThreadId(std::thread::id tid) {
    agentThreadId_ = tid;
}

bool FfiClientAgentIO::isOnAgentThread() const {
    return agentThreadId_ != std::thread::id{} && std::this_thread::get_id() == agentThreadId_;
}

void FfiClientAgentIO::notifyServerReady() {
    // 公开入口 (FfiAgentRuntime 调用; AgentIOBase::onServerReady 为 protected)
    onServerReady();
}

void FfiClientAgentIO::notifyError(int code, std::string message) {
    // 任意线程可调用: io 线程内直接发事件, 其他线程投递后发
    auto emit = [this, code, message = std::move(message)]() {
        neograph::json j = neograph::json::object();
        j["code"]        = code;
        j["message"]     = message;
        emitEvent(AGENTXX_EVT_ERROR, dump(j));
    };
    if (isOnAgentThread()) {
        emit();
    } else {
        asio::post(ex_, std::move(emit));
    }
}

// ---------------------------------------------------------------------------
// AgentIOBase 纯虚实现
// ---------------------------------------------------------------------------

asio::awaitable<std::optional<std::string>> FfiClientAgentIO::getInput() {
    // FFI 模式输入由宿主主动调用 agentxx_send_input 经 Wire 消息进入, 客户端
    // 不设输入拉取循环; 本方法不会被调用, 恒返回输入结束
    co_return std::nullopt;
}

asio::awaitable<neograph::json> FfiClientAgentIO::handleInterrupt(
    std::string_view /*sessionId*/,
    std::string_view /*interruptNode*/,
    std::string_view /*interruptValue*/,
    std::string_view /*interruptArgJson*/
) {
    // server 端 (SessionServerAgentIO) 经总线调用的是服务端点自身的
    // handleInterrupt; 本 client 端点不注册总线, 该纯虚实现仅满足契约,
    // 真实流程见 onPeerMessage(WireInterruptRequest) → waitHostInterrupt()
    co_return neograph::json::array();
}

// ---------------------------------------------------------------------------
// FFI 应答通道
// ---------------------------------------------------------------------------

bool FfiClientAgentIO::hasPendingInterrupt(int64_t interruptId) const {
    std::lock_guard<std::mutex> lock(idsMutex_);
    return activeIds_.count(interruptId) != 0;
}

bool FfiClientAgentIO::submitInterruptResponse(int64_t interruptId, neograph::json values) {
    auto it = pending_.find(interruptId);
    if (it == pending_.end()) {
        return false;
    }
    // try_send: 容量 1, 宿主重复应答会被丢弃 (以首次为准) 并释放 channel
    it->second->try_send(ErrorCode{}, std::move(values));
    return true;
}

void FfiClientAgentIO::failAllPendingInterrupts() {
    for (auto& [id, ch] : pending_) {
        ch->close();
    }
    pending_.clear();
    std::lock_guard<std::mutex> lock(idsMutex_);
    activeIds_.clear();
}

// ---------------------------------------------------------------------------
// 被动接收回调
// ---------------------------------------------------------------------------

void FfiClientAgentIO::onDelta(const agent::Delta& delta) {
    // 与服务端 wire delta JSON 一致 (type=delta, kind=具体类型)
    emitEvent(AGENTXX_EVT_DELTA, dump(agent::io::makeDeltaMsg(delta)));
}

void FfiClientAgentIO::onSync(const agent::SyncPayload& payload) {
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
        return; // 空统计不打扰宿主
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
                // 记录 wire id 供 waitHostInterrupt 使用 (同线程顺序执行);
                // 过期通知 (WireInterruptExpired) 按该 id 匹配并终止等待
                auto    ch = std::make_shared<RespChannel>(ex_, 1);
                int64_t id = m.id;
                {
                    std::lock_guard<std::mutex> lock(idsMutex_);
                    activeIds_.insert(id);
                }
                pending_[id] = ch;

                // 事件: 完整中断信息 (argJson 原样透传, 宿主据此渲染询问 UI)
                neograph::json j = neograph::json::object();
                j["interruptId"] = id;
                j["sessionId"]   = m.sessionId;
                j["node"]        = m.node;
                j["value"]       = m.value;
                j["argJson"]     = m.argJson;
                emitEvent(AGENTXX_EVT_INTERRUPT_REQ, dump(j));

                // 挂起等待宿主 agentxx_interrupt_respond, 收到后回 WireInterruptResponse
                auto self = shared_from_this();
                asio::co_spawn(
                    ex_,
                    [self, ch, id, sessionId = m.sessionId]() mutable -> asio::awaitable<void> {
                        auto [answered, result] = co_await self->waitHostInterrupt(id, ch);
                        // 仅当宿主真实应答 (非过期/停止关闭通道) 时回送;
                        // 过期路径 server 已发 WireInterruptExpired, 无需回送
                        if (answered) {
                            self->sendToPeer(agent::WireInterruptResponse{id, std::move(result)});
                        }
                        co_return;
                    },
                    asio::detached
                );
            } else if constexpr (std::is_same_v<T, agent::WireInterruptExpired>) {
                // server 通知中断已过期 (超时/会话取消): 结束等待并通知宿主
                auto it = pending_.find(m.id);
                if (it != pending_.end()) {
                    it->second->close();
                    pending_.erase(it);
                }
                {
                    std::lock_guard<std::mutex> lock(idsMutex_);
                    activeIds_.erase(m.id);
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
                auto j = agent::io::makeSessionList(m.sessions);
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
            // 通道被关闭 (过期/停止): 视为无应答, 返回空数组
            XX_LOGW("[ffi] interrupt #{} ended early: {}", id, errmsg);
            co_return false;
        }
    );
    pending_.erase(id);
    {
        std::lock_guard<std::mutex> lock(idsMutex_);
        activeIds_.erase(id);
    }
    co_return std::make_pair(gotResp, std::move(result));
}

void FfiClientAgentIO::emitEvent(AgentxxEventType type, std::string json) {
    if (callbacks_.on_event == nullptr) {
        return; // headless
    }
    try {
        callbacks_.on_event(type, json.c_str(), callbacks_.user_data);
    } catch (const std::exception& e) {
        // 宿主 (C) 回调不应抛异常; C++ 绑定层例外兜底, 避免中断 agent io 线程
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