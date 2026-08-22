#include "agentxx/agent/io/ws_io_transport.h"

#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/dispatch.hpp"
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
    // 优先返回握手期间缓存的消息 (connect() 等待 HelloAck 时先于 HelloAck
    // 到达的非 HelloAck 消息; 仅 ex_ 线程访问, 无锁)
    if (!helloPending_.empty()) {
        auto msg = std::move(helloPending_.front());
        helloPending_.pop_front();
        co_return std::move(msg);
    }
    // channel 关闭时 async_receive 抛 system_error, 按"无消息"处理返回 nullopt;
    // 取消类异常 (CancelledException/NodeInterrupt) 由 catchErrorAsync 原样抛出
    co_return co_await agentxx::util::catchErrorAsync<std::optional<WireMessage>>(
        [&]() -> asio::awaitable<std::optional<WireMessage>> {
            auto msg = co_await recvQueue_->async_receive(asio::use_awaitable);
            co_return std::move(msg);
        },
        [](std::string) -> asio::awaitable<std::optional<WireMessage>> {
            co_return std::nullopt;
        }
    );
}

asio::awaitable<bool> WsAgentIOTransport::connect(const WireHello& hello) {
    reconnectTimer_ = std::make_shared<asio::steady_timer>(ex_);
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

    // 记录 sessionId 供重连时复用
    helloSessionId_ = hello.sessionId;

    // 客户端模式：发送 hello 并等待 helloAck
    // 注意：HelloAck 在此处被消费 (仅用于握手判断), 不会传递给 runTransportLoop 的调用方
    auto helloJson = io::makeHello(hello.sessionId, hello.token, hello.lastSeq, hello.tailHash);
    writeQueue_->try_send(ErrorCode{}, helloJson.dump());

    // 等待 HelloAck; 超时或 channel 关闭时按连接失败处理
    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
            for (;;) {
                auto msg = co_await recvQueue_->async_receive(
                    asio::cancel_after(config_.authTimeout, asio::use_awaitable)
                );
                if (std::get_if<WireHelloAck>(&msg)) {
                    break;
                }
                // 防御: 先于 HelloAck 到达的其余消息 (如 Log/ContextStats) 缓存
                // 起来供 recv() 消费, 避免被握手循环丢弃 (协议上服务端先发
                // HelloAck 再重放, 正常路径此列表为空)
                helloPending_.push_back(std::move(msg));
            }
            co_return true;
        },
        [&](std::string errmsg) -> asio::awaitable<bool> {
            // 超时或 channel 关闭，确保资源清理
            XX_LOGW("[ws_transport] auth handshake timeout or disconnected: {}", errmsg);
            // 握手失败视为连接终止: close() 置 stopped_ 并关闭队列/取消定时器/abort ws,
            // 使已启动的 readLoop 退出且不进入自动重连循环 —— 否则调用方在 connect()
            // 返回 false 后已放弃连接, readLoop 仍会每 reconnectBackoff 无限重连
            // (maxReconnectAttempts=0 表示无限), 泄漏协程与持续的连接尝试
            close();
            connected_.store(false, std::memory_order_release); // 明确设置连接状态
            co_return false;
        }
    );

    co_return connected_.load(std::memory_order_acquire);
}

void WsAgentIOTransport::close() {
    if (stopped_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    stopLoops();
}

void WsAgentIOTransport::updateReconnectSessionId(std::string newSessionId) {
    if (!clientMode_) {
        // 服务端模式不存在重连握手, 无需处理
        return;
    }
    // helloSessionId_/lastDeltaSeq_/lastTailHash_ 仅由 ex_ 线程访问
    // (connect() 与 readLoop 重连路径), 投递回 ex_ 线程更新避免数据竞争。
    // 会话切换后新会话的 delta seq 独立编号, 旧的 seq/tailHash 不再适用,
    // 一并复位: 重连时 lastSeq=0 使服务端回退全量 sync
    auto self = shared_from_this();
    asio::dispatch(ex_, [self, tid = std::move(newSessionId)]() {
        if (self->stopped_.load(std::memory_order_acquire)) {
            return;
        }
        self->helloSessionId_ = tid;
        self->lastDeltaSeq_.store(0, std::memory_order_release);
        self->lastTailHash_.clear();
    });
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
    if (reconnectTimer_) {
        // 取消重连退避等待, 使 establishConnection/readLoop 重连循环立即检查 stopped_ 退出
        reconnectTimer_->cancel();
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
        // 队列关闭 (transport 停止/重连) 时 async_receive 抛 system_error;
        // 经 catchErrorAsync 转换为 nullopt 退出写循环
        auto received = co_await agentxx::util::catchErrorAsync<std::optional<std::string>>(
            [&]() -> asio::awaitable<std::optional<std::string>> {
                co_return co_await queue->async_receive(asio::use_awaitable);
            },
            [](std::string) -> asio::awaitable<std::optional<std::string>> {
                co_return std::nullopt;
            }
        );
        if (!received.has_value()) {
            break;
        }
        auto res = co_await client->sendText(std::move(*received));
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
            if (!reconnectTimer_) {
                reconnectTimer_ = std::make_shared<asio::steady_timer>(ex_);
            }
            reconnectTimer_->expires_after(config_.reconnectBackoff);
            ErrorCode ec;
            co_await reconnectTimer_->async_wait(asio::redirect_error(asio::use_awaitable, ec));
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

            // 重连后发送 hello (携带 token/sessionId/lastSeq 供鉴权与增量重放)
            auto helloJson = io::makeHello(
                helloSessionId_,
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
            // 使用成员 reconnectTimer_ (而非局部 timer), 便于 close() 取消退避立即退出
            if (!reconnectTimer_) {
                reconnectTimer_ = std::make_shared<asio::steady_timer>(ex_);
            }
            reconnectTimer_->expires_after(config_.reconnectBackoff);
            ErrorCode ec;
            co_await reconnectTimer_->async_wait(asio::redirect_error(asio::use_awaitable, ec));
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
                return io::makeHello(m.sessionId, m.token, m.lastSeq, m.tailHash).dump();
            } else if constexpr (std::is_same_v<T, WireHelloAck>) {
                return io::makeHelloAck(m.ok, m.sessionId, m.tailHash, m.models).dump();
            } else if constexpr (std::is_same_v<T, WireUserInput>) {
                return io::makeUserInput(m.sessionId, m.text, m.model).dump();
            } else if constexpr (std::is_same_v<T, WireCancel>) {
                return io::makeCancel(m.sessionId).dump();
            } else if constexpr (std::is_same_v<T, WireSelectModel>) {
                return io::makeSelectModel(m.sessionId, m.model).dump();
            } else if constexpr (std::is_same_v<T, WireInterruptRequest>) {
                return io::makeInterruptRequest(m.id, m.sessionId, m.node, m.value, m.argJson)
                    .dump();
            } else if constexpr (std::is_same_v<T, WireInterruptResponse>) {
                return io::makeInterruptResponse(m.id, m.result).dump();
            } else if constexpr (std::is_same_v<T, WireInterruptExpired>) {
                return io::makeInterruptExpired(m.id, m.sessionId).dump();
            } else if constexpr (std::is_same_v<T, Delta>) {
                return io::makeDeltaMsg(m).dump();
            } else if constexpr (std::is_same_v<T, SyncPayload>) {
                return io::makeSyncMsg(m).dump();
            } else if constexpr (std::is_same_v<T, WireTurnResult>) {
                return io::makeTurnResult(
                           m.sessionId,
                           m.hasError,
                           m.errorMessage,
                           m.interrupted,
                           m.startTimeMs,
                           m.durationMs
                )
                    .dump();
            } else if constexpr (std::is_same_v<T, WireContextStats>) {
                return io::makeContextStats(m.contextTokens, m.maxContextTokens, m.tps).dump();
            } else if constexpr (std::is_same_v<T, WireError>) {
                return io::makeError(m.code, m.message).dump();
            } else if constexpr (std::is_same_v<T, WireLog>) {
                return io::makeLog(m.level, m.message).dump();
            } else if constexpr (std::is_same_v<T, WireGetModel>) {
                return io::makeGetModel(m.sessionId).dump();
            } else if constexpr (std::is_same_v<T, WireModelInfo>) {
                return io::makeModelInfo(m.currentModel, m.models).dump();
            } else if constexpr (std::is_same_v<T, WireGetAppendComponentInfo>) {
                return io::makeGetAppendComponentInfo(m.sessionId).dump();
            } else if constexpr (std::is_same_v<T, WireAppendComponentInfo>) {
                return io::makeAppendComponentInfo(m.notifications).dump();
            } else if constexpr (std::is_same_v<T, WireGetContext>) {
                return io::makeGetContext(m.sessionId).dump();
            } else if constexpr (std::is_same_v<T, WireContextMessages>) {
                return io::makeContextMessages(m.messages).dump();
            } else if constexpr (std::is_same_v<T, WireListSessions>) {
                return io::makeListSessions().dump();
            } else if constexpr (std::is_same_v<T, WireSessionList>) {
                return io::makeSessionList(m.sessions).dump();
            } else if constexpr (std::is_same_v<T, WireSwitchSession>) {
                return io::makeSwitchSession(m.sessionId).dump();
            } else if constexpr (std::is_same_v<T, WireSetPermission>) {
                return io::makeSetPermission(m.sessionId, m.path, m.allow, m.index).dump();
            } else if constexpr (std::is_same_v<T, WirePluginData>) {
                return io::makePluginData(m).dump();
            } else if constexpr (std::is_same_v<T, WirePluginDataUp>) {
                return io::makePluginDataUp(m).dump();
            } else if constexpr (std::is_same_v<T, WireMessageQueueUpdate>) {
                return io::makeMessageQueueUpdate(m.sessionId, m.items).dump();
            } else if constexpr (std::is_same_v<T, WireClearMessageQueue>) {
                return io::makeClearMessageQueue(m.sessionId).dump();
            } else if constexpr (std::is_same_v<T, WireRemoveQueueItem>) {
                return io::makeRemoveQueueItem(m.sessionId, m.itemId).dump();
            } else if constexpr (std::is_same_v<T, WireInterruptAndRunNext>) {
                return io::makeInterruptAndRunNext(m.sessionId).dump();
            } else if constexpr (std::is_same_v<T, WireGetViewMessages>) {
                return io::makeGetViewMessages(m.sessionId, m.beforeIndex, m.count).dump();
            } else if constexpr (std::is_same_v<T, WireViewMessagesPage>) {
                return io::makeViewMessagesPage(
                           m.sessionId,
                           m.startIndex,
                           m.totalCount,
                           m.messages
                )
                    .dump();
            } else {
                return "{}";
            }
        },
        msg
    );
}

std::optional<WireMessage> WsAgentIOTransport::deserialize(std::string_view jsonText) {
    neograph::json j;
    bool           parsed = agentxx::util::catchError<bool>(
        [&]() -> bool {
            j = neograph::json::parse(jsonText);
            return true;
        },
        [](std::string) -> bool {
            return false;
        }
    );
    if (!parsed) {
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
        req.id        = j.value("id", int64_t{0});
        req.sessionId = j.value("sessionId", std::string{});
        req.node      = j.value("node", std::string{});
        req.value     = j.value("value", std::string{});
        req.argJson   = j.value("argJson", std::string{});
        return WireMessage{std::move(req)};
    } else if (t == io::MsgType::InterruptResponse) {
        WireInterruptResponse resp;
        resp.id     = j.value("id", int64_t{0});
        resp.result = j.value("result", neograph::json{});
        return WireMessage{std::move(resp)};
    } else if (t == io::MsgType::InterruptExpired) {
        WireInterruptExpired expired;
        expired.id        = j.value("id", int64_t{0});
        expired.sessionId = j.value("sessionId", std::string{});
        return WireMessage{std::move(expired)};
    } else if (t == io::MsgType::TurnResult) {
        WireTurnResult r;
        r.sessionId    = j.value("sessionId", std::string{});
        r.hasError     = j.value("hasError", false);
        r.errorMessage = j.value("errorMessage", std::string{});
        r.interrupted  = j.value("interrupted", false);
        return WireMessage{std::move(r)};
    } else if (t == io::MsgType::HelloAck) {
        WireHelloAck ack;
        ack.ok        = j.value("ok", false);
        ack.sessionId = j.value("sessionId", std::string{});
        ack.tailHash  = j.value("tailHash", std::string{});
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
        hello.sessionId = j.value("sessionId", std::string{});
        hello.token     = j.value("token", std::string{});
        hello.lastSeq   = j.value("lastSeq", uint64_t{0});
        hello.tailHash  = j.value("tailHash", std::string{});
        return WireMessage{std::move(hello)};
    } else if (t == io::MsgType::UserInput) {
        WireUserInput input;
        input.sessionId = j.value("sessionId", std::string{});
        input.text      = j.value("text", std::string{});
        input.model     = j.value("model", std::string{});
        return WireMessage{std::move(input)};
    } else if (t == io::MsgType::Cancel) {
        WireCancel cancel;
        cancel.sessionId = j.value("sessionId", std::string{});
        return WireMessage{std::move(cancel)};
    } else if (t == io::MsgType::SelectModel) {
        WireSelectModel sm;
        sm.sessionId = j.value("sessionId", std::string{});
        sm.model     = j.value("model", std::string{});
        return WireMessage{std::move(sm)};
    } else if (t == io::MsgType::ContextStats) {
        WireContextStats stats;
        stats.contextTokens    = j.value("contextTokens", uint64_t{0});
        stats.maxContextTokens = j.value("maxContextTokens", uint64_t{0});
        stats.tps              = j.value("tps", 0.0);
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
        req.sessionId = j.value("sessionId", std::string{});
        return WireMessage{std::move(req)};
    } else if (t == io::MsgType::ModelInfo) {
        WireModelInfo info;
        info.currentModel = j.value("currentModel", std::string{});
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
        req.sessionId = j.value("sessionId", std::string{});
        return WireMessage{std::move(req)};
    } else if (t == io::MsgType::AppendComponentInfo) {
        WireAppendComponentInfo info;
        info.notifications = io::appendComponentInfoFromJson(j);
        return WireMessage{std::move(info)};
    } else if (t == io::MsgType::GetContext) {
        WireGetContext req;
        req.sessionId = j.value("sessionId", std::string{});
        return WireMessage{std::move(req)};
    } else if (t == io::MsgType::ContextMessages) {
        WireContextMessages resp;
        resp.messages = j.value("messages", neograph::json::array());
        return WireMessage{std::move(resp)};
    } else if (t == io::MsgType::ListSessions) {
        return WireMessage{WireListSessions{}};
    } else if (t == io::MsgType::SessionList) {
        WireSessionList resp;
        resp.sessions = io::sessionListFromJson(j);
        return WireMessage{std::move(resp)};
    } else if (t == io::MsgType::SwitchSession) {
        return WireMessage{io::switchSessionFromJson(j)};
    } else if (t == io::MsgType::SetPermission) {
        return WireMessage{io::setPermissionFromJson(j)};
    } else if (t == io::MsgType::PluginData) {
        return WireMessage{io::pluginDataFromJson(j)};
    } else if (t == io::MsgType::PluginDataUp) {
        return WireMessage{io::pluginDataUpFromJson(j)};
    } else if (t == io::MsgType::MessageQueueUpdate) {
        return WireMessage{io::messageQueueUpdateFromJson(j)};
    } else if (t == io::MsgType::ClearMessageQueue) {
        return WireMessage{io::clearMessageQueueFromJson(j)};
    } else if (t == io::MsgType::RemoveQueueItem) {
        return WireMessage{io::removeQueueItemFromJson(j)};
    } else if (t == io::MsgType::InterruptAndRunNext) {
        return WireMessage{io::interruptAndRunNextFromJson(j)};
    } else if (t == io::MsgType::GetViewMessages) {
        return WireMessage{io::getViewMessagesFromJson(j)};
    } else if (t == io::MsgType::ViewMessagesPage) {
        auto page = io::viewMessagesPageFromJson(j);
        if (page.has_value()) {
            return WireMessage{std::move(page.value())};
        }
    }
    // Pong / Ping: 心跳内部处理, 不转发给调用方
    return std::nullopt;
}

} // namespace agent
} // namespace agentxx
