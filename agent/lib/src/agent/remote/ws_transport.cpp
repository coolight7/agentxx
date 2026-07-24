#include "agentxx/agent/remote/ws_transport.h"

#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/buffer.hpp"
#include "asio/redirect_error.hpp"
#include "asio/use_awaitable.hpp"
#include <boost/beast/core.hpp>

namespace agentxx {
namespace agent {
namespace remote {

// ---------------------------------------------------------------------------
// ServerWsTransport
// ---------------------------------------------------------------------------

ServerWsTransport::ServerWsTransport(WsStream& ws, std::chrono::seconds keepAliveInterval) :
    ws_(&ws) {
    // 断线检测: 空闲 keepAliveInterval*2 无数据 -> 自动发 ping; 再无响应 -> 关闭
    // (read 以 beast::error::timeout 返回). 需保证读协程始终有 outstanding async_read.
    boost::beast::websocket::stream_base::timeout opt{};
    opt.handshake_timeout = std::chrono::seconds(30);
    opt.idle_timeout      = keepAliveInterval * 2;
    opt.keep_alive_pings  = true;
    ws_->set_option(opt);
}

asio::awaitable<std::expected<void, std::string>>
ServerWsTransport::send(std::string_view jsonText) {
    ws_->text(true);
    boost::system::error_code ec;
    co_await ws_->async_write(
        asio::buffer(jsonText),
        asio::redirect_error(asio::use_awaitable, ec)
    );
    if (ec) {
        co_return std::unexpected<std::string>(agentxx::util::autoTryConvertToUtf8(ec.message()));
    }
    co_return std::expected<void, std::string>{};
}

asio::awaitable<std::expected<util::WsMessage, std::string>> ServerWsTransport::recv() {
    boost::beast::flat_buffer buffer;
    boost::system::error_code ec;
    co_await ws_->async_read(buffer, asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
        util::WsMessage msg;
        if (ec == boost::beast::websocket::error::closed) {
            msg.type      = util::WsMessage::Type::Close;
            msg.closeCode = ws_->reason().code;
            co_return msg;
        }
        // timeout / eof / connection reset 等一律视为断线
        co_return std::unexpected<std::string>(agentxx::util::autoTryConvertToUtf8(ec.message()));
    }
    util::WsMessage msg;
    msg.type    = ws_->got_text() ? util::WsMessage::Type::Text : util::WsMessage::Type::Binary;
    msg.payload = boost::beast::buffers_to_string(buffer.data());
    co_return msg;
}

void ServerWsTransport::close() {
    if (!ws_) {
        return;
    }
    boost::system::error_code ec;
    auto&                     lowest = boost::beast::get_lowest_layer(*ws_);
    lowest.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    lowest.socket().close(ec);
}

bool ServerWsTransport::isOpen() const noexcept {
    return ws_ && ws_->is_open();
}

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
