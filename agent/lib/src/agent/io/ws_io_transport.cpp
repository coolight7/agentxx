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
    helloLanguage_  = hello.language;

    // 客户端模式：发送 hello 并等待 helloAck
    // 注意：HelloAck 在此处被处理 (仅用于握手判断), 不会传递给 runTransportLoop 的调用方
    auto helloJson = io::makeHello(
        hello.sessionId,
        hello.token,
        hello.lastSeq,
        hello.tailHash,
        hello.language
    );
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
                // 起来供 recv() 处理, 避免被握手循环丢弃 (协议上服务端先发
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

            if (auto* delta = std::get_if<WireDelta>(&wireMsg.value())) {
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
            } else if (auto* sync = std::get_if<WireSyncPayload>(&wireMsg.value())) {
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
                lastTailHash_,
                helloLanguage_
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
// Serialization: WireMessage <-> JSON (方案 1: 委托统一 wire_protocol 编解码)
// ---------------------------------------------------------------------------

std::string WsAgentIOTransport::serialize(const WireMessage& msg) {
    return agentxx::agent::io::serialize(msg);
}

std::optional<WireMessage> WsAgentIOTransport::deserialize(std::string_view jsonText) {
    return agentxx::agent::io::deserialize(jsonText);
}

} // namespace agent
} // namespace agentxx
