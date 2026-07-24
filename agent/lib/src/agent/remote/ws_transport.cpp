#include "agentxx/agent/remote/ws_transport.h"

namespace agentxx {
namespace agent {
namespace remote {

// ---------------------------------------------------------------------------
// ClientWsTransport
// ---------------------------------------------------------------------------

ClientWsTransport::ClientWsTransport(std::unique_ptr<util::WsClient> client) :
    client_(std::move(client)) {}

asio::awaitable<std::expected<void, std::string>>
ClientWsTransport::send(std::string_view jsonText) {
    if (!client_) {
        co_return std::unexpected<std::string>("client transport closed");
    }
    co_return co_await client_->sendText(jsonText);
}

asio::awaitable<std::expected<util::WsMessage, std::string>> ClientWsTransport::recv() {
    if (!client_) {
        co_return std::unexpected<std::string>("client transport closed");
    }
    co_return co_await client_->recv();
}

void ClientWsTransport::close() {
    // 关闭底层 socket, 唤醒挂起的 recv (readLoop 退出后方可安全析构 transport)
    if (client_) {
        client_->abort();
    }
}

bool ClientWsTransport::isOpen() const noexcept {
    return client_ && client_->isOpen();
}

} // namespace remote
} // namespace agent
} // namespace agentxx
