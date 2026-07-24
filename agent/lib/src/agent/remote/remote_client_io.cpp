#include "agentxx/agent/remote/remote_client_io.h"

#include "agentxx/agent/remote/wire_protocol.h"
#include "agentxx/util/log.h"
#include "asio/cancel_after.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
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
    transport_(std::move(transport)),
    inner_(std::move(inner)),
    config_(config),
    writeQueue_(std::make_shared<WriteQueue>(ex_, config_.writeQueueCap)),
    authChannel_(std::make_shared<AuthChannel>(ex_, 1)),
    turnChannel_(std::make_shared<TurnChannel>(ex_, 1)),
    joinChannel_(std::make_shared<JoinChannel>(ex_, 3)) {}

RemoteClientAgentIO::~RemoteClientAgentIO() {
    requestStop();
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
    if (!writeQueue_->try_send(ErrorCode{}, std::move(text))) {
        XX_LOGW("[remote_client] write queue full, dropping connection");
        requestStop();
    }
}

void RemoteClientAgentIO::requestStop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true)) {
        return;
    }
    writeQueue_->close();
    authChannel_->close();
    turnChannel_->close();
    if (transport_) {
        transport_->close();
    }
}

void RemoteClientAgentIO::onDisconnected() {
    if (!disconnected_.exchange(true)) {
        XX_LOGW("[remote_client] server disconnected");
    }
    requestStop();
}

// ---------------------------------------------------------------------------
// 读 / 写 / 心跳协程
// ---------------------------------------------------------------------------

asio::awaitable<void> RemoteClientAgentIO::writeLoop() {
    for (;;) {
        std::string text;
        try {
            text = co_await writeQueue_->async_receive(asio::use_awaitable);
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
                onDelta(d.value());
            }
        } else if (t == MsgType::SyncMsg) {
            auto s = syncMsgFromJson(j);
            if (s.has_value()) {
                onSync(s.value());
            }
        } else if (t == MsgType::InterruptRequest) {
            auto id      = j.value("id", int64_t{0});
            auto thread  = j.value("thread", std::string{});
            auto node    = j.value("node", std::string{});
            auto value   = j.value("value", std::string{});
            auto argJson = j.value("arg_json", std::string{});
            // 独立协程处理 (经 inner_ 收集用户输入); 持有 self 防止提前析构
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
            r.hasError    = j.value("has_error", false);
            r.interrupted = j.value("interrupted", false);
            r.errorMessage = j.value("error_message", std::string{});
            turnChannel_->try_send(ErrorCode{}, std::move(r));
        } else if (t == MsgType::HelloAck) {
            authChannel_->try_send(ErrorCode{}, j.value("ok", false));
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
    asio::steady_timer timer(ex_);
    for (;;) {
        timer.expires_after(config_.heartbeatInterval);
        ErrorCode ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        if (ec || stopped_.load(std::memory_order_acquire)) {
            break;
        }
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()
        )
                       .count();
        enqueue(makePing(now));
    }
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------

asio::awaitable<bool> RemoteClientAgentIO::connect(
    const std::string& threadId,
    const std::string& token
) {
    enqueue(makeHello(threadId, token));
    try {
        auto ok = co_await authChannel_->async_receive(
            asio::cancel_after(config_.authTimeout, asio::use_awaitable)
        );
        co_return ok;
    } catch (const boost::system::system_error&) {
        co_return false;
    }
}

asio::awaitable<bool> RemoteClientAgentIO::start(
    const std::string& threadId,
    const std::string& token
) {
    auto join = joinChannel_;
    auto spawnGuard = [join](const char* name) {
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
    asio::co_spawn(ex_, writeLoop(), spawnGuard("writeLoop"));
    asio::co_spawn(ex_, readLoop(), spawnGuard("readLoop"));
    asio::co_spawn(ex_, heartbeat(), spawnGuard("heartbeat"));

    bool ok = co_await connect(threadId, token);
    if (!ok) {
        XX_LOGW("[remote_client] connect/auth failed");
        co_await shutdown();
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
    requestStop();
    onDisconnected();
    for (int i = 0; i < 3; ++i) {
        try {
            co_await joinChannel_->async_receive(
                asio::cancel_after(std::chrono::seconds{5}, asio::use_awaitable)
            );
        } catch (const boost::system::system_error&) {
            break;
        }
    }
}

} // namespace remote
} // namespace agent
} // namespace agentxx
