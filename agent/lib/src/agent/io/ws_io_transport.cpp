#include "agentxx/agent/io/ws_io_transport.h"

#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/redirect_error.hpp"
#include "asio/use_awaitable.hpp"

namespace agentxx {
namespace agent {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

WsAgentIOTransport::WsAgentIOTransport(
    asio::any_io_executor ex,
    std::string           url,
    std::string           token,
    Config                config,
    util::WsClientConfig  wsConfig
) :
    ex_(std::move(ex)),
    config_(std::move(config)),
    url_(std::move(url)),
    token_(std::move(token)),
    wsConfig_(std::move(wsConfig)),
    clientMode_(true) {
    if (wsConfig_.recvTimeout.count() <= 0) {
        wsConfig_.recvTimeout = std::chrono::seconds{60};
    }
}

WsAgentIOTransport::WsAgentIOTransport(
    asio::any_io_executor           ex,
    std::unique_ptr<util::WsClient> client,
    Config                          config
) :
    ex_(std::move(ex)),
    config_(std::move(config)),
    clientMode_(false),
    wsClient_(std::move(client)) {}

WsAgentIOTransport::~WsAgentIOTransport() {
    close();
}

// ---------------------------------------------------------------------------
// AgentIOTransportBase
// ---------------------------------------------------------------------------

void WsAgentIOTransport::send(WireMessage msg) {
    if (stopped_.load(std::memory_order_acquire)) {
        return;
    }
    auto text = serialize(msg);
    if (!writeQueue_ || !writeQueue_->try_send(ErrorCode{}, std::move(text))) {
        // 写队列满或已关闭
    }
}

asio::awaitable<std::optional<WireMessage>> WsAgentIOTransport::recv() {
    if (!recvQueue_) {
        co_return std::nullopt;
    }
    try {
        auto msg = co_await recvQueue_->async_receive(asio::use_awaitable);
        co_return std::move(msg);
    } catch (const boost::system::system_error&) {
        co_return std::nullopt;
    }
}

asio::awaitable<bool> WsAgentIOTransport::connect(const WireHello& hello) {
    if (clientMode_) {
        bool ok = co_await establishConnection();
        if (!ok) {
            co_return false;
        }
    }

    writeQueue_     = std::make_shared<WriteQueue>(ex_, config_.writeQueueCap);
    recvQueue_      = std::make_shared<RecvQueue>(ex_, 256);
    heartbeatTimer_ = std::make_shared<asio::steady_timer>(ex_);

    spawnLoops();

    if (!clientMode_) {
        // 服务端模式: WS 已建立, 不发送 hello; 由 AgentServer 处理握手
        connected_.store(true, std::memory_order_release);
        co_return true;
    }

    // 记录 threadId 供重连时复用
    helloThreadId_ = hello.threadId;

    // 客户端模式：发送 hello 并等待 helloAck
    // 注意：HelloAck 在此处被消费 (仅用于握手判断), 不会传递给 runTransportLoop 的调用方
    auto helloJson = io::makeHello(hello.threadId, hello.token, hello.lastSeq, hello.tailHash);
    writeQueue_->try_send(ErrorCode{}, helloJson.dump());

    try {
        for (;;) {
            auto msg = co_await recvQueue_->async_receive(
                asio::cancel_after(config_.authTimeout, asio::use_awaitable)
            );
            if (std::get_if<WireHelloAck>(&msg)) {
                break;
            }
        }
    } catch (const boost::system::system_error& e) {
        // 超时或 channel 关闭，确保资源清理
        XX_LOGW("[ws_transport] auth handshake timeout or disconnected: {}", e.what());
        stopLoops(); // 确保所有循环和资源被正确关闭
        connected_.store(false, std::memory_order_release); // 明确设置连接状态
    }

    co_return connected_.load(std::memory_order_acquire);
}

void WsAgentIOTransport::close() {
    if (stopped_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    stopLoops();
}

bool WsAgentIOTransport::alive() const noexcept {
    return !stopped_.load(std::memory_order_acquire) && connected_.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Internal loops
// ---------------------------------------------------------------------------

void WsAgentIOTransport::spawnLoops() {
    auto self = shared_from_this();
    asio::co_spawn(
        ex_,
        [self]() -> asio::awaitable<void> {
            co_await self->writeLoop();
        },
        asio::detached
    );
    asio::co_spawn(
        ex_,
        [self]() -> asio::awaitable<void> {
            co_await self->readLoop();
        },
        asio::detached
    );
    asio::co_spawn(
        ex_,
        [self]() -> asio::awaitable<void> {
            co_await self->heartbeatLoop();
        },
        asio::detached
    );
}

void WsAgentIOTransport::stopLoops() {
    if (heartbeatTimer_) {
        heartbeatTimer_->cancel();
    }
    if (writeQueue_) {
        writeQueue_->close();
    }
    if (recvQueue_) {
        recvQueue_->close();
    }
    if (wsClient_) {
        wsClient_->abort();
    }
}

asio::awaitable<void> WsAgentIOTransport::writeLoop() {
    auto queue  = writeQueue_;
    auto client = wsClient_;
    if (!queue || !client) {
        co_return;
    }
    for (;;) {
        std::string text;
        try {
            text = co_await queue->async_receive(asio::use_awaitable);
        } catch (const boost::system::system_error&) {
            break;
        }
        auto res = co_await client->sendText(text);
        if (!res) {
            connected_.store(false, std::memory_order_release);
            break;
        }
    }
}

asio::awaitable<void> WsAgentIOTransport::readLoop() {
    auto client = wsClient_;
    if (!client) {
        co_return;
    }

    for (;;) {
        bool disconnected = false;
        for (;;) {
            auto msgRes = co_await client->recv();
            if (!msgRes) {
                disconnected = true;
                break;
            }
            const auto& wsMsg = msgRes.value();
            if (wsMsg.type == util::WsMessage::Type::Close) {
                disconnected = true;
                break;
            }
            if (wsMsg.type != util::WsMessage::Type::Text) {
                continue;
            }

            auto wireMsg = deserialize(wsMsg.payload);
            if (!wireMsg.has_value()) {
                continue;
            }

            if (auto* delta = std::get_if<Delta>(&wireMsg.value())) {
                uint64_t seq = delta->seq;
                uint64_t cur = lastDeltaSeq_.load(std::memory_order_acquire);
                // 重连重放可能重复投递已交付的 delta; 丢弃已见序号, 避免 UI 重复渲染。
                // (seq==0 表示无序号, 不参与去重)
                if (seq > 0 && seq <= cur) {
                    continue;
                }
                while (seq > cur
                       && !lastDeltaSeq_.compare_exchange_weak(cur, seq, std::memory_order_acq_rel)
                ) {
                }
            } else if (auto* sync = std::get_if<SyncPayload>(&wireMsg.value())) {
                lastTailHash_ = sync->tailHash;
            } else if (auto* ack = std::get_if<WireHelloAck>(&wireMsg.value())) {
                if (ack->ok) {
                    connected_.store(true, std::memory_order_release);
                }
            }

            if (recvQueue_) {
                recvQueue_->try_send(ErrorCode{}, std::move(wireMsg.value()));
            }
        }

        if (!disconnected || !clientMode_ || stopped_.load(std::memory_order_acquire)) {
            break;
        }

        // 客户端模式: 自动重连
        connected_.store(false, std::memory_order_release);
        int  attempts    = 0;
        bool reconnected = false;
        while (!stopped_.load(std::memory_order_acquire)) {
            ++attempts;
            if (config_.maxReconnectAttempts > 0 && attempts > config_.maxReconnectAttempts) {
                break;
            }
            asio::steady_timer backoff(ex_);
            backoff.expires_after(config_.reconnectBackoff);
            ErrorCode ec;
            co_await backoff.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if (stopped_.load(std::memory_order_acquire)) {
                break;
            }

            auto newClient = co_await util::wsConnect(ex_, url_, {}, wsConfig_);
            if (!newClient) {
                continue;
            }

            client    = std::shared_ptr<util::WsClient>(std::move(newClient.value()));
            wsClient_ = client;

            // 重建 writeQueue 并重启 writeLoop (旧 writeLoop 因旧 queue 关闭而退出)
            if (writeQueue_) {
                writeQueue_->close();
            }
            writeQueue_ = std::make_shared<WriteQueue>(ex_, config_.writeQueueCap);
            auto self   = shared_from_this();
            asio::co_spawn(
                ex_,
                [self]() -> asio::awaitable<void> {
                    co_await self->writeLoop();
                },
                asio::detached
            );

            // 重连后发送 hello (携带 token/threadId/lastSeq 供鉴权与增量重放)
            auto helloJson = io::makeHello(
                helloThreadId_,
                token_,
                lastDeltaSeq_.load(std::memory_order_acquire),
                lastTailHash_
            );
            writeQueue_->try_send(ErrorCode{}, helloJson.dump());
            reconnected = true;
            break;
        }
        if (!reconnected) {
            break;
        }
    }

    if (recvQueue_) {
        recvQueue_->close();
    }
}

asio::awaitable<void> WsAgentIOTransport::heartbeatLoop() {
    auto timer = heartbeatTimer_;
    if (!timer) {
        co_return;
    }
    for (;;) {
        timer->expires_after(config_.heartbeatInterval);
        ErrorCode ec;
        co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
        if (ec || stopped_.load(std::memory_order_acquire)) {
            break;
        }
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()
        )
                       .count();
        if (writeQueue_) {
            writeQueue_->try_send(ErrorCode{}, io::makePing(now).dump());
        }
    }
}

// ---------------------------------------------------------------------------
// Connection establishment (client mode with reconnection)
// ---------------------------------------------------------------------------

asio::awaitable<bool> WsAgentIOTransport::establishConnection() {
    int attempts = 0;
    for (;;) {
        if (stopped_.load(std::memory_order_acquire)) {
            co_return false;
        }
        auto client = co_await util::wsConnect(ex_, url_, {}, wsConfig_);
        if (!client) {
            if (stopped_.load(std::memory_order_acquire)) {
                co_return false;
            }
            ++attempts;
            if (config_.maxReconnectAttempts > 0 && attempts > config_.maxReconnectAttempts) {
                XX_LOGE("[ws_transport] reconnect attempts exhausted ({})", attempts);
                co_return false;
            }
            XX_LOGW("[ws_transport] connect failed, retry #{}", attempts);
            asio::steady_timer backoff(ex_);
            backoff.expires_after(config_.reconnectBackoff);
            ErrorCode ec;
            co_await backoff.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            continue;
        }
        wsClient_ = std::shared_ptr<util::WsClient>(std::move(client.value()));
        co_return true;
    }
}

// ---------------------------------------------------------------------------
// Serialization: WireMessage <-> JSON
// ---------------------------------------------------------------------------

std::string WsAgentIOTransport::serialize(const WireMessage& msg) {
    return std::visit(
        [](const auto& m) -> std::string {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, WireHello>) {
                return io::makeHello(m.threadId, m.token, m.lastSeq, m.tailHash).dump();
            } else if constexpr (std::is_same_v<T, WireHelloAck>) {
                return io::makeHelloAck(m.ok, m.threadId, m.tailHash, m.models).dump();
            } else if constexpr (std::is_same_v<T, WireUserInput>) {
                return io::makeUserInput(m.threadId, m.text).dump();
            } else if constexpr (std::is_same_v<T, WireCancel>) {
                return io::makeCancel(m.threadId).dump();
            } else if constexpr (std::is_same_v<T, WireSelectModel>) {
                return io::makeSelectModel(m.threadId, m.model).dump();
            } else if constexpr (std::is_same_v<T, WireInterruptRequest>) {
                return io::makeInterruptRequest(m.id, m.threadId, m.node, m.value, m.argJson)
                    .dump();
            } else if constexpr (std::is_same_v<T, WireInterruptResponse>) {
                return io::makeInterruptResponse(m.id, m.result).dump();
            } else if constexpr (std::is_same_v<T, Delta>) {
                return io::makeDeltaMsg(m).dump();
            } else if constexpr (std::is_same_v<T, SyncPayload>) {
                return io::makeSyncMsg(m).dump();
            } else if constexpr (std::is_same_v<T, WireTurnResult>) {
                return io::makeTurnResult(
                           m.threadId,
                           m.hasError,
                           m.errorMessage,
                           m.interrupted,
                           m.startTimeMs,
                           m.durationMs
                )
                    .dump();
            } else if constexpr (std::is_same_v<T, WireContextStats>) {
                return io::makeContextStats(m.contextTokens, m.maxContextTokens).dump();
            } else if constexpr (std::is_same_v<T, WireError>) {
                return io::makeError(m.code, m.message).dump();
            } else if constexpr (std::is_same_v<T, WireLog>) {
                return io::makeLog(m.level, m.message).dump();
            } else if constexpr (std::is_same_v<T, WireGetModel>) {
                return io::makeGetModel(m.threadId).dump();
            } else if constexpr (std::is_same_v<T, WireModelInfo>) {
                return io::makeModelInfo(m.currentModel, m.models).dump();
            } else if constexpr (std::is_same_v<T, WireGetAppendComponentInfo>) {
                return io::makeGetAppendComponentInfo(m.threadId).dump();
            } else if constexpr (std::is_same_v<T, WireAppendComponentInfo>) {
                return io::makeAppendComponentInfo(m.notifications).dump();
            } else if constexpr (std::is_same_v<T, WireGetContext>) {
                return io::makeGetContext(m.threadId).dump();
            } else if constexpr (std::is_same_v<T, WireContextMessages>) {
                return io::makeContextMessages(m.messages).dump();
            } else {
                return "{}";
            }
        },
        msg
    );
}

std::optional<WireMessage> WsAgentIOTransport::deserialize(std::string_view jsonText) {
    neograph::json j;
    try {
        j = neograph::json::parse(jsonText);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!j.is_object()) {
        return std::nullopt;
    }

    auto t = io::msgType(j);

    if (t == io::MsgType::DeltaMsg) {
        auto d = io::deltaMsgFromJson(j);
        if (d.has_value()) {
            return WireMessage{std::move(d.value())};
        }
    } else if (t == io::MsgType::SyncMsg) {
        auto s = io::syncMsgFromJson(j);
        if (s.has_value()) {
            return WireMessage{std::move(s.value())};
        }
    } else if (t == io::MsgType::InterruptRequest) {
        WireInterruptRequest req;
        req.id       = j.value("id", int64_t{0});
        req.threadId = j.value("thread", std::string{});
        req.node     = j.value("node", std::string{});
        req.value    = j.value("value", std::string{});
        req.argJson  = j.value("arg_json", std::string{});
        return WireMessage{std::move(req)};
    } else if (t == io::MsgType::InterruptResponse) {
        WireInterruptResponse resp;
        resp.id     = j.value("id", int64_t{0});
        resp.result = j.value("result", neograph::json{});
        return WireMessage{std::move(resp)};
    } else if (t == io::MsgType::TurnResult) {
        WireTurnResult r;
        r.threadId     = j.value("thread", std::string{});
        r.hasError     = j.value("has_error", false);
        r.errorMessage = j.value("error_message", std::string{});
        r.interrupted  = j.value("interrupted", false);
        return WireMessage{std::move(r)};
    } else if (t == io::MsgType::HelloAck) {
        WireHelloAck ack;
        ack.ok       = j.value("ok", false);
        ack.threadId = j.value("thread", std::string{});
        ack.tailHash = j.value("tail_hash", std::string{});
        if (j.contains("models") && j["models"].is_array()) {
            for (const auto& m : j["models"]) {
                if (m.is_string()) {
                    ack.models.push_back(m.get<std::string>());
                }
            }
        }
        return WireMessage{std::move(ack)};
    } else if (t == io::MsgType::Hello) {
        WireHello hello;
        hello.threadId = j.value("thread", std::string{});
        hello.token    = j.value("token", std::string{});
        hello.lastSeq  = j.value("last_seq", uint64_t{0});
        hello.tailHash = j.value("tail_hash", std::string{});
        return WireMessage{std::move(hello)};
    } else if (t == io::MsgType::UserInput) {
        WireUserInput input;
        input.threadId = j.value("thread", std::string{});
        input.text     = j.value("text", std::string{});
        return WireMessage{std::move(input)};
    } else if (t == io::MsgType::Cancel) {
        WireCancel cancel;
        cancel.threadId = j.value("thread", std::string{});
        return WireMessage{std::move(cancel)};
    } else if (t == io::MsgType::SelectModel) {
        WireSelectModel sm;
        sm.threadId = j.value("thread", std::string{});
        sm.model    = j.value("model", std::string{});
        return WireMessage{std::move(sm)};
    } else if (t == io::MsgType::ContextStats) {
        WireContextStats stats;
        stats.contextTokens    = j.value("context_tokens", uint64_t{0});
        stats.maxContextTokens = j.value("max_context_tokens", uint64_t{0});
        return WireMessage{std::move(stats)};
    } else if (t == io::MsgType::ErrorMsg) {
        WireError err;
        err.code    = j.value("code", 0);
        err.message = j.value("message", std::string{});
        return WireMessage{std::move(err)};
    } else if (t == io::MsgType::LogMsg) {
        WireLog log;
        log.level   = j.value("level", 0);
        log.message = j.value("message", std::string{});
        return WireMessage{std::move(log)};
    } else if (t == io::MsgType::GetModel) {
        WireGetModel req;
        req.threadId = j.value("thread", std::string{});
        return WireMessage{std::move(req)};
    } else if (t == io::MsgType::ModelInfo) {
        WireModelInfo info;
        info.currentModel = j.value("current_model", std::string{});
        if (j.contains("models") && j["models"].is_array()) {
            for (const auto& m : j["models"]) {
                if (m.is_string()) {
                    info.models.push_back(m.get<std::string>());
                }
            }
        }
        return WireMessage{std::move(info)};
    } else if (t == io::MsgType::GetAppendComponentInfo) {
        WireGetAppendComponentInfo req;
        req.threadId = j.value("thread", std::string{});
        return WireMessage{std::move(req)};
    } else if (t == io::MsgType::AppendComponentInfo) {
        WireAppendComponentInfo info;
        info.notifications = io::appendComponentInfoFromJson(j);
        return WireMessage{std::move(info)};
    } else if (t == io::MsgType::GetContext) {
        WireGetContext req;
        req.threadId = j.value("thread", std::string{});
        return WireMessage{std::move(req)};
    } else if (t == io::MsgType::ContextMessages) {
        WireContextMessages resp;
        resp.messages = j.value("messages", neograph::json::array());
        return WireMessage{std::move(resp)};
    }
    // Pong / Ping: 心跳内部处理, 不转发给调用方
    return std::nullopt;
}

} // namespace agent
} // namespace agentxx
