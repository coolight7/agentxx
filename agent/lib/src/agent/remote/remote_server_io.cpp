#include "agentxx/agent/remote/remote_server_io.h"

#include "agentxx/agent/remote/wire_protocol.h"
#include "agentxx/util/log.h"
#include "asio/cancel_after.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/use_awaitable.hpp"

namespace agentxx {
namespace agent {
namespace remote {

RemoteServerAgentIO::RemoteServerAgentIO(
    asio::any_io_executor             ex,
    std::unique_ptr<MessageTransport> transport,
    Config                            config
) :
    ex_(std::move(ex)),
    transport_(std::move(transport)),
    config_(std::move(config)),
    threadId_(config_.defaultThreadId),
    writeQueue_(std::make_shared<WriteQueue>(ex_, config_.writeQueueCap)),
    joinChannel_(std::make_shared<JoinChannel>(ex_, 2)) {}

RemoteServerAgentIO::~RemoteServerAgentIO() {
    requestStop();
}

// ---------------------------------------------------------------------------
// IConnectionSink
// ---------------------------------------------------------------------------

void RemoteServerAgentIO::pushMessage(neograph::json msg) {
    enqueue(std::move(msg));
}

bool RemoteServerAgentIO::alive() const noexcept {
    return !stopped_.load(std::memory_order_acquire) && transport_ && transport_->isOpen();
}

// ---------------------------------------------------------------------------
// 入队 / 停止
// ---------------------------------------------------------------------------

void RemoteServerAgentIO::enqueue(neograph::json msg) {
    if (stopped_.load(std::memory_order_acquire)) {
        return;
    }
    auto text = msg.dump();
    if (!writeQueue_->try_send(ErrorCode{}, std::move(text))) {
        XX_LOGW("[remote_server] write queue full ({}), dropping connection", threadId_);
        requestStop();
    }
}

void RemoteServerAgentIO::requestStop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true)) {
        return;
    }
    writeQueue_->close();
    if (transport_) {
        transport_->close();
    }
}

void RemoteServerAgentIO::onDisconnected() {
    if (!disconnected_.exchange(true)) {
        XX_LOGW("[remote_server] client disconnected (thread={})", threadId_);
        if (controller_) {
            controller_->detach(this);
        }
    }
    requestStop();
}

// ---------------------------------------------------------------------------
// 读 / 写协程
// ---------------------------------------------------------------------------

asio::awaitable<void> RemoteServerAgentIO::writeLoop() {
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

asio::awaitable<void> RemoteServerAgentIO::readLoop() {
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
            enqueue(makeError(400, "invalid json"));
            continue;
        }
        auto t = remote::msgType(j);

        // ----- 鉴权握手 (绑定到 SessionController, 含增量重放) -----
        if (!controller_) {
            if (t == MsgType::Hello) {
                auto tok      = j.value("token", std::string{});
                bool tokenOk  = config_.token.empty() ? true : (tok == config_.token);
                auto thread   = j.value("thread", config_.defaultThreadId);
                std::shared_ptr<SessionController> ctrl;
                std::string                        curTail;
                if (tokenOk && authHandler_) {
                    auto lastSeq  = j.value("last_seq", uint64_t{0});
                    auto tailHash = j.value("tail_hash", std::string{});
                    ctrl          = authHandler_(thread, lastSeq, tailHash, curTail);
                }
                bool ok = tokenOk && (ctrl != nullptr);
                if (ok) {
                    controller_ = ctrl;
                    threadId_   = thread;
                } else if (!tokenOk) {
                    XX_LOGW("[remote_server] auth failed (thread={})", thread);
                }
                enqueue(makeHelloAck(ok, thread, curTail, config_.models));
                if (!ok) {
                    requestStop();
                    break;
                }
            } else {
                enqueue(makeError(401, "not authenticated"));
            }
            continue;
        }

        // ----- 已鉴权: 分发到 SessionController -----
        if (t == MsgType::UserInput) {
            controller_->onUserInput(j.value("text", std::string{}));
        } else if (t == MsgType::InterruptResponse) {
            controller_->resolveInterrupt(
                j.value("id", int64_t{0}),
                j.value("result", neograph::json{})
            );
        } else if (t == MsgType::Cancel) {
            controller_->onCancel();
        } else if (t == MsgType::SelectModel) {
            if (onSelectModel_) {
                onSelectModel_(j.value("model", std::string{}));
            }
        } else if (t == MsgType::Ping) {
            enqueue(makePong(j.value("t", int64_t{0})));
        }
    }
}

// ---------------------------------------------------------------------------
// 连接主流程
// ---------------------------------------------------------------------------

asio::awaitable<void> RemoteServerAgentIO::run() {
    auto join = joinChannel_;
    asio::co_spawn(ex_, writeLoop(), [join](std::exception_ptr ep) {
        if (ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                XX_LOGE("[remote_server] writeLoop error: {}", e.what());
            }
        }
        join->try_send(ErrorCode{});
    });
    asio::co_spawn(ex_, readLoop(), [join](std::exception_ptr ep) {
        if (ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                XX_LOGE("[remote_server] readLoop error: {}", e.what());
            }
        }
        join->try_send(ErrorCode{});
    });

    // 等待读/写协程退出 (断线后); 各 10s 安全上限
    for (int i = 0; i < 2; ++i) {
        bool done = false;
        try {
            co_await joinChannel_->async_receive(
                asio::cancel_after(std::chrono::seconds{10}, asio::use_awaitable)
            );
            done = true;
        } catch (const boost::system::system_error&) {
        }
        if (!done) {
            break;
        }
    }
}

} // namespace remote
} // namespace agent
} // namespace agentxx
