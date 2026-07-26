#include "agentxx/agent/channel_io_transport.h"

#include "asio/use_awaitable.hpp"

namespace agentxx {
namespace agent {

ChannelAgentIOTransport::ChannelAgentIOTransport(
    std::shared_ptr<Chan> outgoing,
    std::shared_ptr<Chan> incoming
) :
    outgoing_(std::move(outgoing)),
    incoming_(std::move(incoming)) {}

std::pair<std::unique_ptr<ChannelAgentIOTransport>, std::unique_ptr<ChannelAgentIOTransport>>
    ChannelAgentIOTransport::makePair(
        asio::any_io_executor clientEx,
        asio::any_io_executor serverEx,
        size_t                cap
    ) {
    auto clientToServer = std::make_shared<Chan>(clientEx, cap);
    auto serverToClient = std::make_shared<Chan>(serverEx, cap);

    auto clientEnd = std::make_unique<ChannelAgentIOTransport>(clientToServer, serverToClient);
    auto serverEnd = std::make_unique<ChannelAgentIOTransport>(serverToClient, clientToServer);

    return {std::move(clientEnd), std::move(serverEnd)};
}

void ChannelAgentIOTransport::send(WireMessage msg) {
    if (closed_.load(std::memory_order_acquire)) {
        return;
    }
    outgoing_->try_send(boost::system::error_code{}, std::move(msg));
}

asio::awaitable<std::optional<WireMessage>> ChannelAgentIOTransport::recv() {
    try {
        auto msg = co_await incoming_->async_receive(asio::use_awaitable);
        co_return std::move(msg);
    } catch (const boost::system::system_error&) {
        co_return std::nullopt;
    }
}

void ChannelAgentIOTransport::close() {
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    outgoing_->close();
    incoming_->close();
}

bool ChannelAgentIOTransport::alive() const noexcept {
    return !closed_.load(std::memory_order_acquire);
}

} // namespace agent
} // namespace agentxx
