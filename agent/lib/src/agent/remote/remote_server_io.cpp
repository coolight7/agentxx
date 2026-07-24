#include "agentxx/agent/remote/remote_server_io.h"

#include "agentxx/agent/deepagent.h"
#include "agentxx/agent/remote/wire_protocol.h"
#include "agentxx/util/log.h"
#include "asio/as_tuple.hpp"
#include "asio/cancel_after.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/redirect_error.hpp"
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
    writeQueue_(std::make_shared<WriteQueue>(ex_, config_.writeQueueCap)),
    inputChannel_(std::make_shared<InputChannel>(ex_, 64)),
    authChannel_(std::make_shared<AuthChannel>(ex_, 1)),
    joinChannel_(std::make_shared<JoinChannel>(ex_, 2)) {}

RemoteServerAgentIO::~RemoteServerAgentIO() {
    requestStop();
}

// ---------------------------------------------------------------------------
// AgentIOBase
// ---------------------------------------------------------------------------

void RemoteServerAgentIO::onDelta(const Delta& delta) {
    enqueue(makeDeltaMsg(delta));
}

void RemoteServerAgentIO::onSync(const SyncPayload& payload) {
    enqueue(makeSyncMsg(payload));
}

asio::awaitable<std::optional<std::string>> RemoteServerAgentIO::getInput() {
    try {
        auto line = co_await inputChannel_->async_receive(asio::use_awaitable);
        co_return std::optional<std::string>(std::move(line));
    } catch (const boost::system::system_error&) {
        // 输入 channel 被关闭 (断线/停止)
        co_return std::nullopt;
    }
}

asio::awaitable<neograph::json> RemoteServerAgentIO::handleInterrupt(
    const std::string& /*threadId*/,
    const std::string& interruptNode,
    const std::string& interruptValue,
    const std::string& interruptArgJson
) {
    auto respCh = std::make_shared<RespChannel>(ex_, 1);
    auto id     = nextReqId_++;
    pending_[id] = respCh;

    enqueue(makeInterruptRequest(id, config_.threadId, interruptNode, interruptValue, interruptArgJson)
    );

    neograph::json result = neograph::json::array();
    try {
        result = co_await respCh->async_receive(
            asio::cancel_after(config_.interruptTimeout, asio::use_awaitable)
        );
    } catch (const boost::system::system_error& e) {
        // 超时 / 断线 (resp channel 被 close)
        XX_LOGW("[remote_server] interrupt #{} ended early: {}", id, e.what());
    }
    pending_.erase(id);
    co_return result;
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
        // 写队列满 -> 慢消费者 -> 断开 (线程安全; 读协程会检测并 onDisconnected)
        XX_LOGW("[remote_server] write queue full ({}), dropping connection", config_.threadId);
        requestStop();
    }
}

void RemoteServerAgentIO::requestStop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true)) {
        return;
    }
    writeQueue_->close();
    inputChannel_->close();
    authChannel_->close();
    if (transport_) {
        transport_->close();
    }
}

void RemoteServerAgentIO::failAllPending() {
    for (auto& [id, ch] : pending_) {
        ch->close();
    }
    pending_.clear();
}

void RemoteServerAgentIO::onDisconnected() {
    if (!disconnected_.exchange(true)) {
        XX_LOGW("[remote_server] client disconnected (thread={})", config_.threadId);
        failAllPending();
        if (onCancel_) {
            onCancel_();
        }
    }
    requestStop();
}

void RemoteServerAgentIO::cancelCurrentTurn() {
    if (onCancel_) {
        onCancel_();
    }
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
            break; // 写队列关闭
        }
        // TODO(perf): 合并相邻 token delta 以降低 WS 帧数 (Phase 2)
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
            continue; // 忽略二进制帧
        }

        neograph::json j;
        try {
            j = neograph::json::parse(msg.payload);
        } catch (const std::exception&) {
            enqueue(makeError(400, "invalid json"));
            continue;
        }
        auto t = remote::msgType(j);

        // ----- 鉴权握手 -----
        if (!authed_) {
            if (t == MsgType::Hello) {
                auto tok   = j.value("token", std::string{});
                bool ok    = config_.token.empty() ? true : (tok == config_.token);
                config_.threadId = j.value("thread", config_.threadId);
                authed_    = ok;
                if (!ok) {
                    XX_LOGW("[remote_server] auth failed (thread={})", config_.threadId);
                }
                // v1: 全量 sync 重连, tailHash 留空
                enqueue(makeHelloAck(ok, config_.threadId, "", config_.models));
                authChannel_->try_send(ErrorCode{}, ok);
                if (!ok) {
                    requestStop();
                    break;
                }
            } else {
                enqueue(makeError(401, "not authenticated"));
            }
            continue;
        }

        // ----- 已鉴权 -----
        if (t == MsgType::UserInput) {
            auto text = j.value("text", std::string{});
            if (!inputChannel_->try_send(ErrorCode{}, std::move(text))) {
                XX_LOGW("[remote_server] input channel full, dropping input");
            }
        } else if (t == MsgType::InterruptResponse) {
            auto id = j.value("id", int64_t{0});
            auto it = pending_.find(id);
            if (it != pending_.end()) {
                it->second->try_send(ErrorCode{}, j.value("result", neograph::json{}));
                pending_.erase(it);
            }
        } else if (t == MsgType::Cancel) {
            cancelCurrentTurn();
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

asio::awaitable<void> RemoteServerAgentIO::shutdown() {
    requestStop();
    onDisconnected();
    // 等待读/写协程退出 (各 5s 安全上限)
    for (int i = 0; i < 2; ++i) {
        try {
            co_await joinChannel_->async_receive(
                asio::cancel_after(std::chrono::seconds{5}, asio::use_awaitable)
            );
        } catch (const boost::system::system_error&) {
            break;
        }
    }
}

asio::awaitable<void> RemoteServerAgentIO::run(const std::shared_ptr<DeepAgent>& agent) {
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

    // 等待鉴权 (co_await 不能置于 catch 块内, 用标志位延后处理)
    bool ok        = false;
    bool authError = false;
    try {
        ok = co_await authChannel_->async_receive(
            asio::cancel_after(config_.authTimeout, asio::use_awaitable)
        );
    } catch (const boost::system::system_error&) {
        XX_LOGW("[remote_server] auth timeout");
        authError = true;
    }
    if (authError || !ok) {
        co_await shutdown();
        co_return;
    }

    XX_OUT("[remote_server] client connected (thread={})", config_.threadId);

    // 驱动对话轮次
    bool first = true;
    for (;;) {
        auto input = co_await getInput();
        if (!input.has_value()) {
            break; // 断线/停止
        }
        if (input->empty()) {
            continue;
        }
        try {
            auto result = co_await agent->runConversationTurnAsync(
                config_.threadId,
                *input,
                first,
                shared_from_this()
            );
            first = false;
            enqueue(makeTurnResult(
                config_.threadId,
                result.hasError,
                result.errorMessage,
                result.interrupted
            ));
        } catch (const std::exception& e) {
            XX_LOGE("[remote_server] turn error: {}", e.what());
            enqueue(makeTurnResult(config_.threadId, true, e.what(), false));
        }
    }

    co_await shutdown();
}

} // namespace remote
} // namespace agent
} // namespace agentxx
