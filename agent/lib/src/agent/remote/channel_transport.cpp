#include "agentxx/agent/remote/channel_transport.h"

#include "agentxx/util/exception.h"
#include "asio/redirect_error.hpp"
#include "asio/use_awaitable.hpp"

namespace agentxx {
namespace agent {
namespace remote {

ChannelTransport::ChannelTransport(std::shared_ptr<Chan> outgoing, std::shared_ptr<Chan> incoming) :
    outgoing_(std::move(outgoing)),
    incoming_(std::move(incoming)) {}

std::pair<std::unique_ptr<ChannelTransport>, std::unique_ptr<ChannelTransport>>
    ChannelTransport::makePair(
        asio::any_io_executor clientEx,
        asio::any_io_executor serverEx,
        size_t                cap
    ) {
    // client->server 通道由 server 端接收 (serverEx); server->client 通道由 client 端接收
    // (clientEx)
    auto c2s    = std::make_shared<Chan>(serverEx, cap);
    auto s2c    = std::make_shared<Chan>(clientEx, cap);
    auto client = std::make_unique<ChannelTransport>(c2s, s2c);
    auto server = std::make_unique<ChannelTransport>(s2c, c2s);
    return {std::move(client), std::move(server)};
}

asio::awaitable<std::expected<void, std::string>> ChannelTransport::send(std::string_view jsonText
) {
    if (closed_.load(std::memory_order_acquire) || !outgoing_) {
        co_return std::unexpected<std::string>("channel transport closed");
    }

    co_return co_await agentxx::util::catchErrorToUnexpectedAsync<void>(
        [&]() -> asio::awaitable<std::expected<void, std::string>> {
            co_await outgoing_->async_send(
                boost::system::error_code{},
                std::string(jsonText),
                asio::use_awaitable
            );
            co_return std::expected<void, std::string>{};
        }
    );
}

asio::awaitable<std::expected<util::WsMessage, std::string>> ChannelTransport::recv() {
    if (!incoming_) {
        co_return std::unexpected<std::string>("channel transport closed");
    }

    co_return co_await agentxx::util::catchErrorToUnexpectedAsync<util::WsMessage>(
        [&]() -> asio::awaitable<std::expected<util::WsMessage, std::string>> {
            auto            text = co_await incoming_->async_receive(asio::use_awaitable);
            util::WsMessage msg;
            msg.type    = util::WsMessage::Type::Text;
            msg.payload = std::move(text);
            co_return std::expected<util::WsMessage, std::string>{std::move(msg)};
        }
    );
}

void ChannelTransport::close() {
    if (closed_.exchange(true)) {
        return;
    }
    // 关闭 outgoing 使对端 recv 感知断开; 关闭 incoming 唤醒本端挂起的 recv
    if (outgoing_) {
        outgoing_->close();
    }
    if (incoming_) {
        incoming_->close();
    }
}

bool ChannelTransport::isOpen() const noexcept {
    return !closed_.load(std::memory_order_acquire);
}

} // namespace remote
} // namespace agent
} // namespace agentxx
