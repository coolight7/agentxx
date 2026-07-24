#include "agentxx/agent/remote/remote_client_io.h"

#include "agentxx/agent/remote/wire_protocol.h"
#include "agentxx/agent/remote/ws_transport.h"
#include "agentxx/util/log.h"
#include "asio/cancel_after.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/experimental/awaitable_operators.hpp"
#include "asio/redirect_error.hpp"
#include "asio/use_awaitable.hpp"
#include <chrono>

namespace agentxx {
namespace agent {
namespace remote {

RemoteClientAgentIO::RemoteClientAgentIO(
    asio::any_io_executor             ex,
    std::unique_ptr<MessageTransport> transport,
    std::shared_ptr<AgentIOBase>      inner,
    Config                            config
) :
    ex_(std::move(ex)),
    inner_(std::move(inner)),
    config_(config),
    autoReconnect_(false),
    transport_(std::move(transport)) {}

RemoteClientAgentIO::RemoteClientAgentIO(
    asio::any_io_executor        ex,
    std::shared_ptr<AgentIOBase> inner,
    std::string                  url,
    std::string                  token,
    Config                       config,
    util::WsClientConfig         wsConfig
) :
    ex_(std::move(ex)),
    inner_(std::move(inner)),
    config_(config),
    url_(std::move(url)),
    token_(std::move(token)),
    wsConfig_(wsConfig),
    autoReconnect_(true) {
    if (wsConfig_.recvTimeout.count() <= 0) {
        wsConfig_.recvTimeout = std::chrono::seconds{60};
    }
}

RemoteClientAgentIO::~RemoteClientAgentIO() {
    breakConnection();
}

// ---------------------------------------------------------------------------
// AgentIOBase 转发到 inner_
// ---------------------------------------------------------------------------

void RemoteClientAgentIO::onDelta(const Delta& delta) {
    if (inner_) {
        inner_->onDelta(delta);
    }
}

void RemoteClientAgentIO::onSync(const SyncPayload& payload) {
    if (inner_) {
        inner_->onSync(payload);
    }
}

asio::awaitable<std::optional<std::string>> RemoteClientAgentIO::getInput() {
    if (!inner_) {
        co_return std::nullopt;
    }
    co_return co_await inner_->getInput();
}

asio::awaitable<neograph::json> RemoteClientAgentIO::handleInterrupt(
    const std::string& threadId,
    const std::string& interruptNode,
    const std::string& interruptValue,
    const std::string& interruptArgJson
) {
    if (!inner_) {
        co_return neograph::json::array();
    }
    co_return co_await inner_->handleInterrupt(
        threadId,
        interruptNode,
        interruptValue,
        interruptArgJson
    );
}

// ---------------------------------------------------------------------------
// 入队 / 停止
// ---------------------------------------------------------------------------

void RemoteClientAgentIO::enqueue(neograph::json msg) {
    if (stopped_.load(std::memory_order_acquire)) {
        return;
    }
    auto text = msg.dump();
    if (!writeQueue_ || !writeQueue_->try_send(ErrorCode{}, std::move(text))) {
        // 写队列满或已关闭 -> 视为断线 (触发重连, 非用户停止)
        breakConnection();
    }
}

void RemoteClientAgentIO::breakConnection() {
    // 关闭各 channel 与传输 (幂等); 取消心跳定时器使心跳协程 promptly 退出
    // 注意: 不设置 stopped_ (stopped_ 仅表示用户停止/输入耗尽, 用于区分"重连"与"退出")
    if (heartbeatTimer_) {
        heartbeatTimer_->cancel();
    }
    if (writeQueue_) writeQueue_->close();
    if (authChannel_) authChannel_->close();
    if (turnChannel_) turnChannel_->close();
    if (disconnectChannel_) disconnectChannel_->try_send(ErrorCode{}, true);
    if (transport_) transport_->close();
}

void RemoteClientAgentIO::onDisconnected() {
    if (!disconnected_.exchange(true)) {
        XX_LOGW("[remote_client] server disconnected");
    }
    if (turnChannel_) {
        turnChannel_->close();
    }
    if (disconnectChannel_) {
        disconnectChannel_->try_send(ErrorCode{}, true);
    }
    breakConnection();
}

// ---------------------------------------------------------------------------
// 连接状态管理
// ---------------------------------------------------------------------------

void RemoteClientAgentIO::resetConnState() {
    writeQueue_        = std::make_shared<WriteQueue>(ex_, config_.writeQueueCap);
    authChannel_       = std::make_shared<AuthChannel>(ex_, 1);
    turnChannel_       = std::make_shared<TurnChannel>(ex_, 1);
    joinChannel_       = std::make_shared<JoinChannel>(ex_, 3);
    disconnectChannel_ = std::make_shared<DisconnectChannel>(ex_, 1);
    heartbeatTimer_    = std::make_shared<asio::steady_timer>(ex_);
    disconnected_.store(false, std::memory_order_release);
}

void RemoteClientAgentIO::spawnLoops() {
    auto join = joinChannel_;
    auto guard = [join](const char* name) {
        return [join, name](std::exception_ptr ep) {
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    XX_LOGE("[remote_client] {} error: {}", name, e.what());
                }
            }
            join->try_send(ErrorCode{});
        };
    };
    asio::co_spawn(ex_, writeLoop(), guard("writeLoop"));
    asio::co_spawn(ex_, readLoop(), guard("readLoop"));
    asio::co_spawn(ex_, heartbeat(), guard("heartbeat"));
}

asio::awaitable<void> RemoteClientAgentIO::shutdownLoops() {
    breakConnection();
    if (!joinChannel_) {
        co_return;
    }
    for (int i = 0; i < 3; ++i) {
        bool done = false;
        try {
            co_await joinChannel_->async_receive(
                asio::cancel_after(std::chrono::seconds{5}, asio::use_awaitable)
            );
            done = true;
        } catch (const boost::system::system_error&) {
        }
        if (!done) {
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// 读 / 写 / 心跳协程
// ---------------------------------------------------------------------------

asio::awaitable<void> RemoteClientAgentIO::writeLoop() {
    auto queue = writeQueue_;
    for (;;) {
        std::string text;
        try {
            text = co_await queue->async_receive(asio::use_awaitable);
        } catch (const boost::system::system_error&) {
            break;
        }
        auto res = co_await transport_->send(text);
        if (!res) {
            onDisconnected();
            break;
        }
    }
}

asio::awaitable<void> RemoteClientAgentIO::handleInterruptRequest(
    int64_t            id,
    const std::string& threadId,
    const std::string& node,
    const std::string& value,
    const std::string& argJson
) {
    auto result = co_await handleInterrupt(threadId, node, value, argJson);
    enqueue(makeInterruptResponse(id, result));
}

asio::awaitable<void> RemoteClientAgentIO::readLoop() {
    for (;;) {
        auto msgRes = co_await transport_->recv();
        if (!msgRes) {
            onDisconnected();
            break;
        }
        const auto& msg = msgRes.value();
        if (msg.type == util::WsMessage::Type::Close) {
            onDisconnected();
            break;
        }
        if (msg.type != util::WsMessage::Type::Text) {
            continue;
        }

        neograph::json j;
        try {
            j = neograph::json::parse(msg.payload);
        } catch (const std::exception&) {
            continue;
        }
        auto t = remote::msgType(j);

        if (t == MsgType::DeltaMsg) {
            auto d = deltaMsgFromJson(j);
            if (d.has_value()) {
                // 跟踪最大 seq 供重连增量重放
                uint64_t seq = d->seq;
                uint64_t cur = lastDeltaSeq_.load(std::memory_order_acquire);
                while (seq > cur
                       && !lastDeltaSeq_.compare_exchange_weak(cur, seq, std::memory_order_acq_rel)) {
                }
                onDelta(d.value());
            }
        } else if (t == MsgType::SyncMsg) {
            auto s = syncMsgFromJson(j);
            if (s.has_value()) {
                lastTailHash_ = s->tailHash;
                // 全量 sync 后以 server 的 deltaSeq 校准增量基线
                if (j.contains("delta_seq")) {
                    lastDeltaSeq_.store(j.value("delta_seq", uint64_t{0}), std::memory_order_release);
                }
                onSync(s.value());
            }
        } else if (t == MsgType::InterruptRequest) {
            auto id      = j.value("id", int64_t{0});
            auto thread  = j.value("thread", std::string{});
            auto node    = j.value("node", std::string{});
            auto value   = j.value("value", std::string{});
            auto argJson = j.value("arg_json", std::string{});
            asio::co_spawn(
                ex_,
                handleInterruptRequest(id, thread, node, value, argJson),
                [self = shared_from_this()](std::exception_ptr ep) {
                    if (ep) {
                        try {
                            std::rethrow_exception(ep);
                        } catch (const std::exception& e) {
                            XX_LOGE("[remote_client] interrupt handler error: {}", e.what());
                        }
                    }
                }
            );
        } else if (t == MsgType::TurnResult) {
            TurnResult r;
            r.hasError     = j.value("has_error", false);
            r.interrupted  = j.value("interrupted", false);
            r.errorMessage = j.value("error_message", std::string{});
            if (turnChannel_) {
                turnChannel_->try_send(ErrorCode{}, std::move(r));
            }
        } else if (t == MsgType::HelloAck) {
            if (j.contains("tail_hash")) {
                lastTailHash_ = j.value("tail_hash", std::string{});
            }
            if (authChannel_) {
                authChannel_->try_send(ErrorCode{}, j.value("ok", false));
            }
        } else if (t == MsgType::ContextStats) {
            if (contextStatsCallback_) {
                contextStatsCallback_(
                    j.value("context_tokens", uint64_t{0}),
                    j.value("max_context_tokens", uint64_t{0})
                );
            }
        } else if (t == MsgType::Pong) {
            // 心跳回应; recv 已因此重置超时
        } else if (t == MsgType::ErrorMsg) {
            XX_LOGW(
                "[remote_client] server error {}: {}",
                j.value("code", 0),
                j.value("message", std::string{})
            );
        }
    }
}

asio::awaitable<void> RemoteClientAgentIO::heartbeat() {
    auto timer = heartbeatTimer_;
    if (!timer) {
        co_return;
    }
    for (;;) {
        timer->expires_after(config_.heartbeatInterval);
        ErrorCode ec;
        co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
        if (ec || stopped_.load(std::memory_order_acquire)) {
            break; // 被取消 (requestStop) 或已停止
        }
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()
        )
                       .count();
        enqueue(makePing(now));
    }
}

// ---------------------------------------------------------------------------
// 握手
// ---------------------------------------------------------------------------

asio::awaitable<bool> RemoteClientAgentIO::connect(
    const std::string& threadId,
    const std::string& token
) {
    // 重连时携带 lastSeq/tailHash 供 server 增量重放
    enqueue(makeHello(threadId, token, lastDeltaSeq_.load(std::memory_order_acquire), lastTailHash_)
    );
    bool ok        = false;
    bool gotReply  = false;
    try {
        ok = co_await authChannel_->async_receive(
            asio::cancel_after(config_.authTimeout, asio::use_awaitable)
        );
        gotReply = true;
    } catch (const boost::system::system_error&) {
    }
    co_return gotReply && ok;
}

// ---------------------------------------------------------------------------
// 手动模式
// ---------------------------------------------------------------------------

asio::awaitable<bool> RemoteClientAgentIO::start(
    const std::string& threadId,
    const std::string& token
) {
    threadId_ = threadId;
    resetConnState();
    spawnLoops();
    bool ok = co_await connect(threadId, token);
    if (!ok) {
        XX_LOGW("[remote_client] connect/auth failed");
        co_await shutdownLoops();
    }
    co_return ok;
}

void RemoteClientAgentIO::sendUserInput(
    const std::string& threadId,
    const std::string& text,
    bool               isFirstMsg,
    const std::string& model
) {
    enqueue(makeUserInput(threadId, text, isFirstMsg, model));
}

asio::awaitable<RemoteClientAgentIO::TurnResult> RemoteClientAgentIO::awaitTurnResult() {
    co_return co_await turnChannel_->async_receive(asio::use_awaitable);
}

void RemoteClientAgentIO::selectModel(const std::string& threadId, const std::string& model) {
    enqueue(makeSelectModel(threadId, model));
}

void RemoteClientAgentIO::cancel(const std::string& threadId) {
    enqueue(makeCancel(threadId));
}

asio::awaitable<void> RemoteClientAgentIO::shutdown() {
    stopped_.store(true, std::memory_order_release);
    breakConnection();
    // 手动模式: 调用方驱动, 此处 join 协程
    // 自动模式: runSession 自行 join (避免与其竞争 join 信号)
    if (!autoReconnect_) {
        co_await shutdownLoops();
    }
}

// ---------------------------------------------------------------------------
// 自动模式: 重连 + 输入泵
// ---------------------------------------------------------------------------

asio::awaitable<std::optional<std::string>> RemoteClientAgentIO::waitInnerInput() {
    if (!inner_) {
        co_return std::nullopt;
    }
    co_return co_await inner_->getInput();
}

asio::awaitable<bool> RemoteClientAgentIO::waitDisconnect() {
    try {
        co_return co_await disconnectChannel_->async_receive(asio::use_awaitable);
    } catch (const boost::system::system_error&) {
        co_return true;
    }
}

asio::awaitable<void> RemoteClientAgentIO::sleepFor(std::chrono::milliseconds d) {
    asio::steady_timer timer(ex_);
    timer.expires_after(d);
    ErrorCode ec;
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
}

asio::awaitable<bool> RemoteClientAgentIO::runOnce() {
    using asio::experimental::awaitable_operators::operator||;

    resetConnState();
    spawnLoops();

    bool ok = co_await connect(threadId_, token_);
    if (!ok) {
        XX_LOGW("[remote_client] handshake/auth failed");
        co_await shutdownLoops();
        co_return false;
    }
    XX_OUT("[remote_client] connected (thread={})", threadId_);

    // 输入泵: 竞态 本地输入 vs 断线信号
    bool disconnected = false;
    while (!disconnected && !stopped_.load(std::memory_order_acquire)) {
        auto res = co_await (waitInnerInput() || waitDisconnect());
        if (res.index() == 1) {
            disconnected = true; // 断线
            break;
        }
        const auto& inputOpt = std::get<0>(res);
        if (!inputOpt.has_value()) {
            stopped_.store(true, std::memory_order_release); // 本地输入结束 (EOF/退出)
            break;
        }
        if (inputOpt->empty()) {
            continue;
        }
        sendUserInput(threadId_, *inputOpt, first_, model_);
        first_ = false;

        bool turnDisconnected = false;
        try {
            co_await awaitTurnResult();
        } catch (const boost::system::system_error&) {
            turnDisconnected = true;
        }
        if (turnDisconnected) {
            disconnected = true;
            break;
        }
    }

    co_await shutdownLoops();
    co_return true;
}

asio::awaitable<void> RemoteClientAgentIO::runSession(
    const std::string& threadId,
    const std::string& model
) {
    threadId_ = threadId;
    model_    = model;
    first_    = true;

    // 进程内/手动传输模式: 单连接, 不重连
    if (!autoReconnect_) {
        co_await runOnce();
        co_return;
    }

    // 自动重连模式 (WS url)
    int attempts = 0;
    for (;;) {
        if (stopped_.load(std::memory_order_acquire)) {
            co_return;
        }
        auto client = co_await util::wsConnect(ex_, url_, {}, wsConfig_);
        if (!client) {
            if (stopped_.load(std::memory_order_acquire)) {
                co_return;
            }
            ++attempts;
            if (config_.maxReconnectAttempts > 0 && attempts > config_.maxReconnectAttempts) {
                XX_LOGE("[remote_client] reconnect attempts exhausted ({})", attempts);
                co_return;
            }
            XX_LOGW("[remote_client] connect failed, retry #{}", attempts);
            co_await sleepFor(config_.reconnectBackoff);
            continue;
        }
        attempts   = 0;
        transport_ = std::make_unique<ClientWsTransport>(std::move(client.value()));

        bool ok = co_await runOnce();
        if (stopped_.load(std::memory_order_acquire)) {
            co_return;
        }
        if (!ok) {
            // 握手失败 -> 退避重试
            co_await sleepFor(config_.reconnectBackoff);
            continue;
        }
        //  runOnce 因断线返回 -> 重连
        XX_LOGW("[remote_client] disconnected, reconnecting...");
        co_await sleepFor(config_.reconnectBackoff);
    }
}

} // namespace remote
} // namespace agent
} // namespace agentxx
