#include <asio/detail/socket_option.hpp>
// 仅提供 TCP_KEEPIDLE/TCP_KEEPINTVL/TCP_KEEPCNT 选项号常量, 设置经由 asio 接口完成
#if XX_IS_WIN_D
#include <windows.h>
// ---
#include <mstcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

#include "agentxx/util/ws_client.h"

#include "agentxx/util/exception.h"
#include "agentxx/util/http_client.h"
#include <algorithm>
#include <cctype>
#include <openssl/ssl.h>

namespace agentxx {
namespace util {

namespace {

/// 启用 TCP keepalive 并缩短探测间隔: WebSocket 长连接在业务空窗期极易被
/// NAT/网关静默丢弃, keepalive 探测包可维持中间设备状态 (best-effort)
template<typename Socket>
void enableWsKeepalive(Socket& sock) noexcept {
    neograph_asio_error_code ec;
    sock.set_option(asio::socket_base::keep_alive(true), ec);
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
    asio::detail::socket_option::integer<IPPROTO_TCP, TCP_KEEPIDLE>  idle{30};
    asio::detail::socket_option::integer<IPPROTO_TCP, TCP_KEEPINTVL> interval{10};
    asio::detail::socket_option::integer<IPPROTO_TCP, TCP_KEEPCNT>   count{3};
    sock.set_option(idle, ec);
    sock.set_option(interval, ec);
    sock.set_option(count, ec);
#endif
}

} // namespace

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
        // 注: 请求头 decorator (UA + 自定义头) 由 wsConnect 统一设置, 避免二次 set_option
        // 覆盖导致自定义头/UA 丢失
        if (isSsl) {
            wss->set_option(boost::beast::websocket::stream_base::timeout{
                .handshake_timeout = std::chrono::seconds{30},
                .idle_timeout      = boost::beast::websocket::stream_base::none(),
                .keep_alive_pings  = false,
            });
            wss->read_message_max(config.maxMessageSize);
        } else {
            ws->set_option(boost::beast::websocket::stream_base::timeout{
                .handshake_timeout = std::chrono::seconds{30},
                .idle_timeout      = boost::beast::websocket::stream_base::none(),
                .keep_alive_pings  = false,
            });
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
    co_return co_await agentxx::util::catchErrorAsync<std::expected<void, std::string>>(
        [&]() -> asio::awaitable<std::expected<void, std::string>> {
            size_t offset = 0;
            if (impl_->isSsl) {
                impl_->wss->text(true);
                do {
                    size_t len
                        = std::min(WsClientConfig::kTimeoutChunkBytes, payload.size() - offset);
                    bool fin = (offset + len >= payload.size());
                    auto n   = co_await impl_->wss->async_write_some(
                        fin,
                        asio::buffer(payload.data() + offset, len),
                        asio::cancel_after(impl_->config.sendTimeout, asio::use_awaitable)
                    );
                    offset += n;
                } while (offset < payload.size());
            } else {
                impl_->ws->text(true);
                do {
                    size_t len
                        = std::min(WsClientConfig::kTimeoutChunkBytes, payload.size() - offset);
                    bool fin = (offset + len >= payload.size());
                    auto n   = co_await impl_->ws->async_write_some(
                        fin,
                        asio::buffer(payload.data() + offset, len),
                        asio::cancel_after(impl_->config.sendTimeout, asio::use_awaitable)
                    );
                    offset += n;
                } while (offset < payload.size());
            }
            co_return std::expected<void, std::string>{};
        },
        [&](std::string errinfo) -> asio::awaitable<std::expected<void, std::string>> {
            impl_->closed_ = true;
            co_return std::unexpected{std::move(errinfo)};
        }
    );
}

asio::awaitable<std::expected<void, std::string>> WsClient::sendBinary(std::string_view payload) {
    if (!impl_ || impl_->closed_) {
        co_return std::unexpected{std::string{"connection closed"}};
    }
    co_return co_await agentxx::util::catchErrorAsync<std::expected<void, std::string>>(
        [&]() -> asio::awaitable<std::expected<void, std::string>> {
            size_t offset = 0;
            if (impl_->isSsl) {
                impl_->wss->binary(true);
                do {
                    size_t len
                        = std::min(WsClientConfig::kTimeoutChunkBytes, payload.size() - offset);
                    bool fin = (offset + len >= payload.size());
                    auto n   = co_await impl_->wss->async_write_some(
                        fin,
                        asio::buffer(payload.data() + offset, len),
                        asio::cancel_after(impl_->config.sendTimeout, asio::use_awaitable)
                    );
                    offset += n;
                } while (offset < payload.size());
            } else {
                impl_->ws->binary(true);
                do {
                    size_t len
                        = std::min(WsClientConfig::kTimeoutChunkBytes, payload.size() - offset);
                    bool fin = (offset + len >= payload.size());
                    auto n   = co_await impl_->ws->async_write_some(
                        fin,
                        asio::buffer(payload.data() + offset, len),
                        asio::cancel_after(impl_->config.sendTimeout, asio::use_awaitable)
                    );
                    offset += n;
                } while (offset < payload.size());
            }
            co_return std::expected<void, std::string>{};
        },
        [&](std::string errinfo) -> asio::awaitable<std::expected<void, std::string>> {
            impl_->closed_ = true;
            co_return std::unexpected{std::move(errinfo)};
        }
    );
}

asio::awaitable<std::expected<void, std::string>> WsClient::sendPing(std::string_view payload) {
    if (!impl_ || impl_->closed_) {
        co_return std::unexpected{std::string{"connection closed"}};
    }
    // RFC 6455 §5.5: 控制帧 payload 最大 125 字节, 超限直接报错避免协议错误
    if (payload.size() > 125) {
        co_return std::unexpected{std::string{"ping payload too large (max 125 bytes)"}};
    }
    co_return co_await agentxx::util::catchErrorAsync<std::expected<void, std::string>>(
        [&]() -> asio::awaitable<std::expected<void, std::string>> {
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
        },
        [&](std::string errinfo) -> asio::awaitable<std::expected<void, std::string>> {
            impl_->closed_ = true;
            co_return std::unexpected{std::move(errinfo)};
        }
    );
}

asio::awaitable<std::expected<void, std::string>>
    WsClient::sendClose(uint16_t code, std::string_view reason) {
    if (!impl_ || impl_->closed_) {
        co_return std::expected<void, std::string>{};
    }
    impl_->closed_ = true;
    co_return co_await agentxx::util::catchErrorAsync<std::expected<void, std::string>>(
        [&]() -> asio::awaitable<std::expected<void, std::string>> {
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
        },
        // close 握手失败 (如对端已断开) 不影响关闭语义, 仍视为成功
        [](std::string) -> asio::awaitable<std::expected<void, std::string>> {
            co_return std::expected<void, std::string>{};
        }
    );
}

asio::awaitable<std::expected<WsMessage, std::string>> WsClient::recv() {
    if (!impl_ || impl_->closed_) {
        co_return std::unexpected{std::string{"connection closed"}};
    }
    co_return co_await agentxx::util::catchErrorAsync<std::expected<WsMessage, std::string>>(
        [&]() -> asio::awaitable<std::expected<WsMessage, std::string>> {
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
                // close 帧是协议正常结束而非错误: 转为 Close 消息返回。
                // 需要错误码以读取对端 close code/reason, 因此在此特判,
                // 其余错误交由 catchErrorAsync 统一处理
                if (e.code() == boost::beast::websocket::error::closed) {
                    impl_->closed_ = true;
                    // 透传对端 close 帧携带的 code/reason (reason() 在 close 处理后可用)
                    auto      cr = impl_->isSsl ? impl_->wss->reason() : impl_->ws->reason();
                    WsMessage msg;
                    msg.type        = WsMessage::Type::Close;
                    msg.closeCode   = (cr.code != 0) ? cr.code : 1000;
                    msg.closeReason = std::string(cr.reason);
                    co_return std::expected<WsMessage, std::string>{std::move(msg)};
                }
                // 接收超时 (空闲超过 recvTimeout): 以专用消息报错。
                // Beast websocket stream 在 async_read_some 被 cancel 后内部状态机
                // (帧缓冲/解压上下文) 不可恢复, 后续操作必然失败, 必须标记 closed,
                // 调用方应重建连接
                if (e.code() == asio::error::operation_aborted) {
                    impl_->closed_ = true;
                    co_return std::unexpected{std::string{"recv timeout"}};
                }
                throw;
            }
        },
        [&](std::string errinfo) -> asio::awaitable<std::expected<WsMessage, std::string>> {
            // 其它传输错误: 连接状态同样不可恢复, 标记 closed
            impl_->closed_ = true;
            co_return std::unexpected{std::move(errinfo)};
        }
    );
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

    // 解析 host[:port]: IPv6 字面量带方括号 (ws://[::1]:8080/),
    // 直接 rfind(':') 会错误切割 IPv6 地址内的冒号
    if (hostPort.starts_with('[')) {
        auto cb = hostPort.find(']');
        if (cb == std::string::npos) {
            co_return std::unexpected<std::string>{"invalid ws url: unclosed '[' in host"};
        }
        host = hostPort.substr(1, cb - 1);
        if (cb + 1 < hostPort.size()) {
            if (hostPort[cb + 1] != ':') {
                co_return std::unexpected<std::string>{"invalid ws url: expected ':' after ']'"};
            }
            port = hostPort.substr(cb + 2);
        }
    } else {
        auto colon = hostPort.rfind(':');
        if (colon != std::string::npos) {
            host = hostPort.substr(0, colon);
            port = hostPort.substr(colon + 1);
        }
    }
    if (port.empty()) {
        port = isSsl ? "443" : "80";
    }

    if (host.empty()) {
        co_return std::unexpected<std::string>{"empty host in ws url"};
    }
    // 端口必须是纯数字, 否则 resolver 会把 "host:abc" 当服务名解析, 报错不可读
    for (char c : port) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            co_return std::unexpected<std::string>{fmt::format("invalid port in ws url: {}", port)};
        }
    }

    auto impl   = std::make_unique<WsClient::Impl>(executor, config);
    impl->isSsl = isSsl;

    // connectTimeout 是 DNS + TCP + TLS + WS 握手四个阶段的总上限:
    // 共用同一个 deadline, 避免各阶段各用满导致总耗时成倍放大
    auto connectDeadline = std::chrono::steady_clock::now() + config.connectTimeout;
    auto remainingMs     = [&]() -> std::chrono::milliseconds {
        auto now = std::chrono::steady_clock::now();
        if (now >= connectDeadline) {
            return std::chrono::milliseconds{1};
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(connectDeadline - now);
    };

    // Host 头: IPv6 字面量必须带方括号, 非默认端口附加 ":port"
    bool        defaultPort = (isSsl && port == "443") || (!isSsl && port == "80");
    std::string hostForHeader
        = (host.find(':') != std::string::npos) ? fmt::format("[{}]", host) : std::string(host);
    std::string hostHeader
        = defaultPort ? hostForHeader : fmt::format("{}:{}", hostForHeader, port);

    // UA + 自定义头统一在一个 decorator 内设置 (set_option(decorator) 是覆盖语义,
    // 分两次设置会丢失先设置的部分)
    auto decorateRequest = [headers](ws::request_type& req) {
        req.set(boost::beast::http::field::user_agent, "agentxx-ws/1.0");
        for (const auto& [k, v] : headers) {
            req.set(k, v);
        }
    };

    // 连接各阶段错误 (DNS/TCP/TLS/WS 握手超时、系统错误等) 统一经
    // catchErrorToUnexpectedAsync 转为错误字符串 (含 UTF-8 转换);
    // CancelledException/NodeInterrupt 保持抛出, 不阻断 BaseAgent 取消处理
    co_return co_await agentxx::util::catchErrorToUnexpectedAsync<std::unique_ptr<WsClient>>(
        [&]() -> asio::awaitable<std::expected<std::unique_ptr<WsClient>, std::string>> {
            tcp::resolver resolver(executor);
            auto          endpoints = co_await resolver.async_resolve(
                host,
                port,
                asio::cancel_after(remainingMs(), asio::use_awaitable)
            );

            if (isSsl) {
                auto& sslCtx    = HttpClient::sharedSslCtx(config.sslVerify);
                auto  sslStream = beast::ssl_stream<beast::tcp_stream>(executor, sslCtx);
                // SNI 与证书验证相互独立: 即使关闭验证也必须发送 SNI,
                // 否则 CDN/网关按 SNI 路由时握手直接失败; IP 字面量 (含 ':') 不支持 SNI
                if (host.find(':') == std::string::npos) {
                    ::SSL_set_tlsext_host_name(sslStream.native_handle(), host.c_str());
                }
                if (!config.sslVerify) {
                    ::SSL_set_verify(sslStream.native_handle(), SSL_VERIFY_NONE, nullptr);
                }

                co_await beast::get_lowest_layer(sslStream).async_connect(
                    endpoints,
                    asio::cancel_after(remainingMs(), asio::use_awaitable)
                );

                neograph_asio_error_code tcpEc;
                beast::get_lowest_layer(sslStream).socket().set_option(tcp::no_delay(true), tcpEc);
                enableWsKeepalive(beast::get_lowest_layer(sslStream).socket());

                co_await sslStream.async_handshake(
                    asio::ssl::stream_base::client,
                    asio::cancel_after(remainingMs(), asio::use_awaitable)
                );

                impl->wss = std::make_unique<WsClient::Impl::WssStream>(std::move(sslStream));
                impl->configureStream();
                impl->wss->set_option(beast::websocket::stream_base::decorator(decorateRequest));

                co_await impl->wss->async_handshake(
                    hostHeader,
                    path,
                    asio::cancel_after(remainingMs(), asio::use_awaitable)
                );
            } else {
                beast::tcp_stream tcpStream(executor);

                co_await tcpStream.async_connect(
                    endpoints,
                    asio::cancel_after(remainingMs(), asio::use_awaitable)
                );

                neograph_asio_error_code tcpEc;
                tcpStream.socket().set_option(tcp::no_delay(true), tcpEc);
                enableWsKeepalive(tcpStream.socket());

                impl->ws = std::make_unique<WsClient::Impl::WsStream>(std::move(tcpStream));
                impl->configureStream();
                impl->ws->set_option(beast::websocket::stream_base::decorator(decorateRequest));

                co_await impl->ws->async_handshake(
                    hostHeader,
                    path,
                    asio::cancel_after(remainingMs(), asio::use_awaitable)
                );
            }

            co_return std::expected<std::unique_ptr<WsClient>, std::string>{
                std::unique_ptr<WsClient>(new WsClient(std::move(impl)))
            };
        }
    );
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
    enableWsKeepalive(boost::beast::get_lowest_layer(*impl->ws).socket());
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
    enableWsKeepalive(boost::beast::get_lowest_layer(*impl->wss).socket());
    return std::unique_ptr<WsClient>(new WsClient(std::move(impl)));
}

} // namespace util
} // namespace agentxx
