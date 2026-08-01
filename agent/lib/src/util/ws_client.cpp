#include "agentxx/util/ws_client.h"

#include "agentxx/util/http_client.h"
#include <algorithm>
#include <asio/redirect_error.hpp>
#include <openssl/ssl.h>

namespace agentxx {
namespace util {

struct WsClient::Impl {
    asio::any_io_executor executor;
    WsClientConfig        config;

    using WsStream = boost::beast::websocket::stream<boost::beast::tcp_stream>;
    using WssStream
        = boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>;

    std::unique_ptr<WsStream>  ws;
    std::unique_ptr<WssStream> wss;
    bool                       isSsl   = false;
    bool                       closed_ = false;

    Impl(asio::any_io_executor ex, WsClientConfig cfg) :
        executor(std::move(ex)),
        config(std::move(cfg)) {}

    void configureStream() {
        // 禁用 Beast 内部 idle_timeout: 由外部 cancel_after(recvTimeout/sendTimeout) 统一控制,
        // 避免 Beast suggested timeout (默认 30s) 与用户配置的 recvTimeout 冲突
        if (isSsl) {
            wss->set_option(boost::beast::websocket::stream_base::timeout{
                .handshake_timeout = std::chrono::seconds{30},
                .idle_timeout      = boost::beast::websocket::stream_base::none(),
                .keep_alive_pings  = false,
            });
            wss->set_option(boost::beast::websocket::stream_base::decorator(
                [](boost::beast::websocket::request_type& req) {
                    req.set(boost::beast::http::field::user_agent, "agentxx-ws/1.0");
                }
            ));
            wss->read_message_max(config.maxMessageSize);
        } else {
            ws->set_option(boost::beast::websocket::stream_base::timeout{
                .handshake_timeout = std::chrono::seconds{30},
                .idle_timeout      = boost::beast::websocket::stream_base::none(),
                .keep_alive_pings  = false,
            });
            ws->set_option(boost::beast::websocket::stream_base::decorator(
                [](boost::beast::websocket::request_type& req) {
                    req.set(boost::beast::http::field::user_agent, "agentxx-ws/1.0");
                }
            ));
            ws->read_message_max(config.maxMessageSize);
        }
    }
};

WsClient::WsClient(std::unique_ptr<Impl> impl) :
    impl_(std::move(impl)) {}

WsClient::~WsClient() = default;

WsClient::WsClient(WsClient&&) noexcept            = default;
WsClient& WsClient::operator=(WsClient&&) noexcept = default;

bool WsClient::isOpen() const noexcept {
    return impl_ && !impl_->closed_;
}

void WsClient::setRecvTimeout(std::chrono::seconds timeout) noexcept {
    if (impl_) {
        impl_->config.recvTimeout = timeout;
    }
}

void WsClient::abort() noexcept {
    if (!impl_) {
        return;
    }
    impl_->closed_ = true;
    neograph_asio_error_code ec;
    if (impl_->isSsl && impl_->wss) {
        auto& lowest = boost::beast::get_lowest_layer(*impl_->wss);
        lowest.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        lowest.socket().close(ec);
    } else if (impl_->ws) {
        auto& lowest = boost::beast::get_lowest_layer(*impl_->ws);
        lowest.socket().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        lowest.socket().close(ec);
    }
}

asio::awaitable<std::expected<void, std::string>> WsClient::sendText(std::string_view payload) {
    if (!impl_ || impl_->closed_) {
        co_return std::unexpected{std::string{"connection closed"}};
    }
    try {
        size_t offset = 0;
        if (impl_->isSsl) {
            impl_->wss->text(true);
            do {
                size_t len = std::min(WsClientConfig::kTimeoutChunkBytes, payload.size() - offset);
                bool   fin = (offset + len >= payload.size());
                auto   n   = co_await impl_->wss->async_write_some(
                    fin,
                    asio::buffer(payload.data() + offset, len),
                    asio::cancel_after(impl_->config.sendTimeout, asio::use_awaitable)
                );
                offset += n;
            } while (offset < payload.size());
        } else {
            impl_->ws->text(true);
            do {
                size_t len = std::min(WsClientConfig::kTimeoutChunkBytes, payload.size() - offset);
                bool   fin = (offset + len >= payload.size());
                auto   n   = co_await impl_->ws->async_write_some(
                    fin,
                    asio::buffer(payload.data() + offset, len),
                    asio::cancel_after(impl_->config.sendTimeout, asio::use_awaitable)
                );
                offset += n;
            } while (offset < payload.size());
        }
        co_return std::expected<void, std::string>{};
    } catch (const boost::system::system_error& e) {
        impl_->closed_ = true;
        co_return std::unexpected{std::string{e.what()}};
    }
}

asio::awaitable<std::expected<void, std::string>> WsClient::sendBinary(std::string_view payload) {
    if (!impl_ || impl_->closed_) {
        co_return std::unexpected{std::string{"connection closed"}};
    }
    try {
        size_t offset = 0;
        if (impl_->isSsl) {
            impl_->wss->binary(true);
            do {
                size_t len = std::min(WsClientConfig::kTimeoutChunkBytes, payload.size() - offset);
                bool   fin = (offset + len >= payload.size());
                auto   n   = co_await impl_->wss->async_write_some(
                    fin,
                    asio::buffer(payload.data() + offset, len),
                    asio::cancel_after(impl_->config.sendTimeout, asio::use_awaitable)
                );
                offset += n;
            } while (offset < payload.size());
        } else {
            impl_->ws->binary(true);
            do {
                size_t len = std::min(WsClientConfig::kTimeoutChunkBytes, payload.size() - offset);
                bool   fin = (offset + len >= payload.size());
                auto   n   = co_await impl_->ws->async_write_some(
                    fin,
                    asio::buffer(payload.data() + offset, len),
                    asio::cancel_after(impl_->config.sendTimeout, asio::use_awaitable)
                );
                offset += n;
            } while (offset < payload.size());
        }
        co_return std::expected<void, std::string>{};
    } catch (const boost::system::system_error& e) {
        impl_->closed_ = true;
        co_return std::unexpected{std::string{e.what()}};
    }
}

asio::awaitable<std::expected<void, std::string>> WsClient::sendPing(std::string_view payload) {
    if (!impl_ || impl_->closed_) {
        co_return std::unexpected{std::string{"connection closed"}};
    }
    try {
        // ping payload 协议限制最大 125 字节, 直接使用配置的 sendTimeout
        if (impl_->isSsl) {
            co_await impl_->wss->async_ping(
                boost::beast::websocket::ping_data{payload},
                asio::cancel_after(impl_->config.sendTimeout, asio::use_awaitable)
            );
        } else {
            co_await impl_->ws->async_ping(
                boost::beast::websocket::ping_data{payload},
                asio::cancel_after(impl_->config.sendTimeout, asio::use_awaitable)
            );
        }
        co_return std::expected<void, std::string>{};
    } catch (const boost::system::system_error& e) {
        impl_->closed_ = true;
        co_return std::unexpected{std::string{e.what()}};
    }
}

asio::awaitable<std::expected<void, std::string>>
    WsClient::sendClose(uint16_t code, std::string_view reason) {
    if (!impl_ || impl_->closed_) {
        co_return std::expected<void, std::string>{};
    }
    impl_->closed_ = true;
    try {
        boost::beast::websocket::close_reason cr;
        cr.code   = code;
        cr.reason = std::string(reason);
        if (impl_->isSsl) {
            co_await impl_->wss->async_close(
                cr,
                asio::cancel_after(std::chrono::seconds{5}, asio::use_awaitable)
            );
        } else {
            co_await impl_->ws->async_close(
                cr,
                asio::cancel_after(std::chrono::seconds{5}, asio::use_awaitable)
            );
        }
        co_return std::expected<void, std::string>{};
    } catch (const boost::system::system_error&) {
        co_return std::expected<void, std::string>{};
    }
}

asio::awaitable<std::expected<WsMessage, std::string>> WsClient::recv() {
    if (!impl_ || impl_->closed_) {
        co_return std::unexpected{std::string{"connection closed"}};
    }
    try {
        std::string payload;
        std::string chunk;
        chunk.resize(4096);

        if (impl_->isSsl) {
            do {
                auto n = co_await impl_->wss->async_read_some(
                    asio::buffer(chunk, chunk.size()),
                    asio::cancel_after(impl_->config.recvTimeout, asio::use_awaitable)
                );
                payload.append(chunk.data(), n);
            } while (!impl_->wss->is_message_done());
        } else {
            do {
                auto n = co_await impl_->ws->async_read_some(
                    asio::buffer(chunk, chunk.size()),
                    asio::cancel_after(impl_->config.recvTimeout, asio::use_awaitable)
                );
                payload.append(chunk.data(), n);
            } while (!impl_->ws->is_message_done());
        }

        WsMessage msg;
        bool      isText = impl_->isSsl ? impl_->wss->got_text() : impl_->ws->got_text();
        msg.type         = isText ? WsMessage::Type::Text : WsMessage::Type::Binary;
        msg.payload      = std::move(payload);
        co_return std::expected<WsMessage, std::string>{std::move(msg)};
    } catch (const boost::system::system_error& e) {
        if (e.code() == boost::beast::websocket::error::closed) {
            impl_->closed_ = true;
            WsMessage msg;
            msg.type      = WsMessage::Type::Close;
            msg.closeCode = 1000;
            co_return std::expected<WsMessage, std::string>{std::move(msg)};
        }
        if (e.code() == asio::error::operation_aborted) {
            // 接收超时 (空闲超过 recvTimeout): Beast websocket stream 在 async_read_some
            // 被 cancel 后内部状态机 (帧缓冲/解压上下文) 不可恢复, 后续操作必然失败,
            // 因此必须标记 closed。调用方应重建连接。
            impl_->closed_ = true;
            co_return std::unexpected{std::string{"recv timeout"}};
        }
        impl_->closed_ = true;
        co_return std::unexpected{std::string{e.what()}};
    }
}

asio::awaitable<std::expected<std::unique_ptr<WsClient>, std::string>> wsConnect(
    asio::any_io_executor                            executor,
    std::string_view                                 url,
    std::vector<std::pair<std::string, std::string>> headers,
    WsClientConfig                                   config
) {
    namespace beast = boost::beast;
    namespace ws    = beast::websocket;
    using tcp       = asio::ip::tcp;

    std::string urlStr(url);
    bool        isSsl = false;
    std::string host;
    std::string port;
    std::string path;

    if (urlStr.starts_with("wss://")) {
        isSsl  = true;
        urlStr = urlStr.substr(6);
    } else if (urlStr.starts_with("ws://")) {
        urlStr = urlStr.substr(5);
    } else {
        co_return std::unexpected<std::string>{"invalid ws url, must start with ws:// or wss://"};
    }

    auto        pathStart = urlStr.find('/');
    std::string hostPort;
    if (pathStart == std::string::npos) {
        hostPort = urlStr;
        path     = "/";
    } else {
        hostPort = urlStr.substr(0, pathStart);
        path     = urlStr.substr(pathStart);
    }

    auto colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
        host = hostPort.substr(0, colon);
        port = hostPort.substr(colon + 1);
    } else {
        host = hostPort;
        port = isSsl ? "443" : "80";
    }

    if (host.empty()) {
        co_return std::unexpected<std::string>{"empty host in ws url"};
    }

    auto impl   = std::make_unique<WsClient::Impl>(executor, config);
    impl->isSsl = isSsl;

    try {
        tcp::resolver resolver(executor);
        auto          endpoints = co_await resolver.async_resolve(
            host,
            port,
            asio::cancel_after(config.connectTimeout, asio::use_awaitable)
        );

        if (isSsl) {
            auto& sslCtx    = HttpClient::sharedSslCtx(config.sslVerify);
            auto  sslStream = beast::ssl_stream<beast::tcp_stream>(executor, sslCtx);
            if (!config.sslVerify) {
                ::SSL_set_verify(sslStream.native_handle(), SSL_VERIFY_NONE, nullptr);
            } else {
                ::SSL_set_tlsext_host_name(sslStream.native_handle(), host.c_str());
            }

            co_await beast::get_lowest_layer(sslStream).async_connect(
                endpoints,
                asio::cancel_after(config.connectTimeout, asio::use_awaitable)
            );

            neograph_asio_error_code tcpEc;
            beast::get_lowest_layer(sslStream).socket().set_option(tcp::no_delay(true), tcpEc);

            co_await sslStream.async_handshake(
                asio::ssl::stream_base::client,
                asio::cancel_after(config.connectTimeout, asio::use_awaitable)
            );

            impl->wss = std::make_unique<WsClient::Impl::WssStream>(std::move(sslStream));
            impl->configureStream();

            std::string hostHeader = host;
            if ((isSsl && port != "443") || (!isSsl && port != "80")) {
                hostHeader += ":" + port;
            }

            impl->wss->set_option(
                beast::websocket::stream_base::decorator([&headers](ws::request_type& req) {
                    for (const auto& [k, v] : headers) {
                        req.set(k, v);
                    }
                })
            );

            co_await impl->wss->async_handshake(
                hostHeader,
                path,
                asio::cancel_after(config.connectTimeout, asio::use_awaitable)
            );
        } else {
            beast::tcp_stream tcpStream(executor);

            co_await tcpStream.async_connect(
                endpoints,
                asio::cancel_after(config.connectTimeout, asio::use_awaitable)
            );

            neograph_asio_error_code tcpEc;
            tcpStream.socket().set_option(tcp::no_delay(true), tcpEc);

            impl->ws = std::make_unique<WsClient::Impl::WsStream>(std::move(tcpStream));
            impl->configureStream();

            std::string hostHeader = host;
            if (port != "80") {
                hostHeader += ":" + port;
            }

            impl->ws->set_option(
                beast::websocket::stream_base::decorator([&headers](ws::request_type& req) {
                    for (const auto& [k, v] : headers) {
                        req.set(k, v);
                    }
                })
            );

            co_await impl->ws->async_handshake(
                hostHeader,
                path,
                asio::cancel_after(config.connectTimeout, asio::use_awaitable)
            );
        }
    } catch (const boost::system::system_error& e) {
        if (e.code() == asio::error::operation_aborted) {
            co_return std::unexpected<std::string>{"ws connect timeout"};
        }
        auto errInfo = std::string{e.what()};
        agentxx::util::autoConvertToUtf8(errInfo);
        co_return std::unexpected<std::string>{errInfo};
    } catch (const std::exception& e) {
        auto errInfo = std::string{e.what()};
        agentxx::util::autoConvertToUtf8(errInfo);
        co_return std::unexpected<std::string>{errInfo};
    }

    co_return std::expected<std::unique_ptr<WsClient>, std::string>{
        std::unique_ptr<WsClient>(new WsClient(std::move(impl)))
    };
}

std::unique_ptr<WsClient> wrapAcceptedWs(
    asio::any_io_executor                                     ex,
    boost::beast::websocket::stream<boost::beast::tcp_stream> ws,
    WsClientConfig                                            config
) {
    auto impl   = std::make_unique<WsClient::Impl>(std::move(ex), std::move(config));
    impl->isSsl = false;
    impl->ws    = std::make_unique<WsClient::Impl::WsStream>(std::move(ws));
    // 禁用 Beast 内部 idle_timeout, 由外部 cancel_after 统一控制超时
    impl->ws->set_option(boost::beast::websocket::stream_base::timeout{
        .handshake_timeout = std::chrono::seconds{30},
        .idle_timeout      = boost::beast::websocket::stream_base::none(),
        .keep_alive_pings  = false,
    });
    impl->ws->read_message_max(impl->config.maxMessageSize);
    return std::unique_ptr<WsClient>(new WsClient(std::move(impl)));
}

std::unique_ptr<WsClient> wrapAcceptedWss(
    asio::any_io_executor                                                               ex,
    boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>> wss,
    WsClientConfig                                                                      config
) {
    auto impl   = std::make_unique<WsClient::Impl>(std::move(ex), std::move(config));
    impl->isSsl = true;
    impl->wss   = std::make_unique<WsClient::Impl::WssStream>(std::move(wss));
    // 禁用 Beast 内部 idle_timeout, 由外部 cancel_after 统一控制超时
    impl->wss->set_option(boost::beast::websocket::stream_base::timeout{
        .handshake_timeout = std::chrono::seconds{30},
        .idle_timeout      = boost::beast::websocket::stream_base::none(),
        .keep_alive_pings  = false,
    });
    impl->wss->read_message_max(impl->config.maxMessageSize);
    return std::unique_ptr<WsClient>(new WsClient(std::move(impl)));
}

} // namespace util
} // namespace agentxx
