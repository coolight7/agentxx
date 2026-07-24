#pragma once

#include "agentxx/agent/remote/message_transport.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "agentxx/util/ws_client.h"
#include "asio/buffer.hpp"
#include "asio/awaitable.hpp"
#include "asio/redirect_error.hpp"
#include "asio/use_awaitable.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace agentxx {
namespace agent {
namespace remote {

/// 服务端 WS 传输 (模板: 支持明文 tcp_stream 与 TLS ssl_stream)
/// - 构造时配置 keepalive ping 用于检测 client 断线
/// - recv/send 分别由读/写协程独占调用 (beast 允许 1 读 + 1 写 并发)
template<typename WsStream>
class ServerWsTransportT : public MessageTransport {
public:

    explicit ServerWsTransportT(
        WsStream&            ws,
        std::chrono::seconds keepAliveInterval = std::chrono::seconds{30}
    ) :
        ws_(&ws) {
        // 断线检测: 空闲 keepAliveInterval*2 无数据 -> 自动发 ping; 再无响应 -> 关闭
        boost::beast::websocket::stream_base::timeout opt{};
        opt.handshake_timeout = std::chrono::seconds(30);
        opt.idle_timeout      = keepAliveInterval * 2;
        opt.keep_alive_pings  = true;
        ws_->set_option(opt);
    }

    asio::awaitable<std::expected<void, std::string>> send(std::string_view jsonText) override {
        ws_->text(true);
        boost::system::error_code ec;
        co_await ws_->async_write(
            asio::buffer(jsonText),
            asio::redirect_error(asio::use_awaitable, ec)
        );
        if (ec) {
            co_return std::unexpected<std::string>(
                agentxx::util::autoTryConvertToUtf8(ec.message())
            );
        }
        co_return std::expected<void, std::string>{};
    }

    asio::awaitable<std::expected<util::WsMessage, std::string>> recv() override {
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
            co_return std::unexpected<std::string>(
                agentxx::util::autoTryConvertToUtf8(ec.message())
            );
        }
        util::WsMessage msg;
        msg.type    = ws_->got_text() ? util::WsMessage::Type::Text : util::WsMessage::Type::Binary;
        msg.payload = boost::beast::buffers_to_string(buffer.data());
        co_return msg;
    }

    void close() override {
        if (!ws_) {
            return;
        }
        boost::system::error_code ec;
        auto&                     lowest = boost::beast::get_lowest_layer(*ws_);
        lowest.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        lowest.socket().close(ec);
    }

    bool isOpen() const noexcept override {
        return ws_ && ws_->is_open();
    }

private:

    WsStream* ws_;
};

/// 明文 WS (ws://)
using ServerWsTransport = ServerWsTransportT<
    boost::beast::websocket::stream<boost::beast::tcp_stream>>;

/// TLS WS (wss://)
using ServerWssTransport = ServerWsTransportT<
    boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>>;

/// 客户端 WS 传输: 包裹 util::WsClient (ws/wss 由 url scheme 决定)
class ClientWsTransport : public MessageTransport {
public:

    explicit ClientWsTransport(std::unique_ptr<util::WsClient> client);

    asio::awaitable<std::expected<void, std::string>> send(std::string_view jsonText) override;

    asio::awaitable<std::expected<util::WsMessage, std::string>> recv() override;

    void close() override;

    bool isOpen() const noexcept override;

private:

    std::unique_ptr<util::WsClient> client_;
};

} // namespace remote
} // namespace agent
} // namespace agentxx
