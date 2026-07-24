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

void RemoteServerAgentIO::enqueueCloseSentinel() {
    if (stopped_.load(std::memory_order_acquire)) {
        return;
    }
    // 空串作为关闭哨兵 (合法 JSON 消息均非空)
    writeQueue_->try_send(ErrorCode{}, std::string{});
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
    std::optional<std::string> leftover;
    for (;;) {
        std::string text;
        if (leftover.has_value()) {
            text = std::move(leftover.value());
            leftover.reset();
        } else {
            try {
                text = co_await writeQueue_->async_receive(asio::use_awaitable);
            } catch (const boost::system::system_error&) {
                break;
            }
        }
        // 空串关闭哨兵: 此前的消息已发出, 现在关闭传输
        if (text.empty()) {
            transport_->close();
            break;
        }
        // 机会性合并相邻同类 token delta 降帧
        auto merged = coalesceTokenDeltas(std::move(text));
        leftover    = std::move(merged.second);
        auto res    = co_await transport_->send(merged.first);
        if (!res) {
            onDisconnected();
            break;
        }
    }
}

std::pair<std::string, std::optional<std::string>>
    RemoteServerAgentIO::coalesceTokenDeltas(std::string first) {
    neograph::json j;
    try {
        j = neograph::json::parse(first);
    } catch (const std::exception&) {
        return {std::move(first), std::nullopt};
    }
    if (j.value("type", std::string{}) != std::string(MsgType::DeltaMsg)) {
        return {std::move(first), std::nullopt};
    }
    auto kind = j.value("kind", std::string{});
    if (kind != "text_token" && kind != "thinking_token") {
        return {std::move(first), std::nullopt};
    }

    // 是 token delta: 非阻塞 drain 后续同类 token delta 并合并文本
    std::string                accText = j.value("text", std::string{});
    std::optional<std::string> leftover;
    for (;;) {
        std::string next;
        bool        got = writeQueue_->try_receive([&](ErrorCode ec, std::string m) {
            if (!ec) {
                next = std::move(m);
            }
        });
        if (!got || next.empty()) {
            break;
        }
        neograph::json nj;
        bool           parsed = true;
        try {
            nj = neograph::json::parse(next);
        } catch (const std::exception&) {
            parsed = false;
        }
        if (parsed && nj.value("type", std::string{}) == std::string(MsgType::DeltaMsg)
            && nj.value("kind", std::string{}) == kind) {
            accText += nj.value("text", std::string{});
            if (nj.contains("seq")) {
                j["seq"] = nj["seq"]; // 保留最新 seq (重连增量基线)
            }
        } else {
            // 不可合并: 留待下次发送
            leftover = std::move(next);
            break;
        }
    }
    j["text"] = accText;
    return {j.dump(), std::move(leftover)};
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
                auto tok     = j.value("token", std::string{});
                bool tokenOk = config_.token.empty() ? true : (tok == config_.token);
                auto thread  = j.value("thread", config_.defaultThreadId);
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
                    // 冲刷 hello_ack 后再关闭: 入队关闭哨兵, 写协程发完即关闭传输
                    enqueueCloseSentinel();
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
    auto self = shared_from_this();
    // 完成回调捕获 self: 保证协程退出前对象不被析构 (避免 join 超时后 UAF)
    auto guard = [join, self](const char* name) {
        return [join, self, name](std::exception_ptr ep) {
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    XX_LOGE("[remote_server] {} error: {}", name, e.what());
                }
            }
            join->try_send(ErrorCode{});
        };
    };
    asio::co_spawn(ex_, writeLoop(), guard("writeLoop"));
    asio::co_spawn(ex_, readLoop(), guard("readLoop"));

    // 等待读/写协程退出 (断线后); 各 10s 安全上限
    int received = 0;
    for (int i = 0; i < 2; ++i) {
        try {
            co_await joinChannel_->async_receive(
                asio::cancel_after(std::chrono::seconds{10}, asio::use_awaitable)
            );
            received++;
        } catch (const boost::system::system_error&) {
            break;
        }
    }
    if (received < 2) {
        // 有协程未退出 (如 send 被慢客户端阻塞): 强制关闭传输使其退出, 再等待
        requestStop();
        for (int i = received; i < 2; ++i) {
            try {
                co_await joinChannel_->async_receive(
                    asio::cancel_after(std::chrono::seconds{5}, asio::use_awaitable)
                );
            } catch (const boost::system::system_error&) {
                break;
            }
        }
    }
}

} // namespace remote
} // namespace agent
} // namespace agentxx
