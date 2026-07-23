#include "agentxx/util/http_server.h"

namespace agentxx {
namespace util {

int httpMethodIndex(boost::beast::http::verb v) noexcept {
    switch (v) {
        case boost::beast::http::verb::get:
            return 0;
        case boost::beast::http::verb::head:
            return 1;
        case boost::beast::http::verb::post:
            return 2;
        case boost::beast::http::verb::put:
            return 3;
        case boost::beast::http::verb::delete_:
            return 4;
        case boost::beast::http::verb::connect:
            return 5;
        case boost::beast::http::verb::options:
            return 6;
        case boost::beast::http::verb::trace:
            return 7;
        case boost::beast::http::verb::patch:
            return 8;
        default:
            return -1;
    }
}

std::string_view requestPath(std::string_view target) noexcept {
    auto q = target.find('?');
    return q == std::string_view::npos ? target : target.substr(0, q);
}

std::string formatHttpDate(std::time_t t) noexcept {
    char buf[64];
#if defined(_WIN32)
    std::tm tm;
    gmtime_s(&tm, &t);
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
#else
    std::tm tm;
    gmtime_r(&t, &tm);
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
#endif
    return buf;
}

HttpServer::HttpServer(Config config) :
    config_(std::move(config)) {
    router_ = std::make_unique<Router>();
}

HttpServer::~HttpServer() {
    stop();
}

uint16_t HttpServer::port() const noexcept {
    if (acceptor_ && acceptor_->is_open()) {
        boost::system::error_code ec;
        auto                      ep = acceptor_->local_endpoint(ec);
        if (!ec) {
            return ep.port();
        }
    }
    return 0;
}

void HttpServer::start() {
    if (stopped_) {
        return;
    }
    stopped_ = false;

    using tcp = asio::ip::tcp;

    // Create workers (each has its own io_context — zero-lock isolation)
    unsigned threadCount = config_.ioThreads;
    if (threadCount == 0) {
        threadCount = std::thread::hardware_concurrency();
    }
    if (threadCount == 0) {
        threadCount = 1;
    }

    workers_.reserve(threadCount);
    for (unsigned i = 0; i < threadCount; ++i) {
        auto w = std::make_unique<Worker>();
        w->workGuard.emplace(asio::make_work_guard(w->ioCtx));
        workers_.push_back(std::move(w));
    }

    auto& mainCtx = workers_[0]->ioCtx;

    // Acceptor — runs on the first worker's io_context
    auto const    address = asio::ip::make_address(config_.address);
    tcp::endpoint endpoint(address, config_.port);
    acceptor_ = std::make_unique<tcp::acceptor>(mainCtx);
    acceptor_->open(endpoint.protocol());
    acceptor_->set_option(tcp::acceptor::reuse_address(true));
    acceptor_->bind(endpoint);
    acceptor_->listen(asio::socket_base::max_listen_connections);

    // Setup SSL context if configured
    if (!config_.sslCertFile.empty() && !config_.sslKeyFile.empty()) {
        sslCtx_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv12_server);
        sslCtx_->set_options(
            asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2
            | asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1
            | asio::ssl::context::no_tlsv1_1 | asio::ssl::context::single_dh_use
        );
        sslCtx_->use_certificate_chain_file(config_.sslCertFile);
        sslCtx_->use_private_key_file(config_.sslKeyFile, asio::ssl::context::pem);
    }

    // Spawn listener coroutine on first worker's io_context
    asio::co_spawn(mainCtx, acceptLoop(), asio::detached);

    XX_OUT("[server] Listening on {}:{}{}", config_.address, port(), sslCtx_ ? " (HTTPS)" : "");

    // Start worker threads — each runs its own io_context
    for (unsigned i = 0; i < threadCount; ++i) {
        workers_[i]->thread = std::thread([this, i]() {
            workers_[i]->ioCtx.run();
        });
    }

    // Block until all threads finish
    for (auto& w : workers_) {
        if (w->thread.joinable()) {
            w->thread.join();
        }
    }
}

void HttpServer::stop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true)) {
        return;
    }
    boost::system::error_code ec;
    if (acceptor_ && acceptor_->is_open()) {
        acceptor_->cancel(ec);
        acceptor_->close(ec);
    }
    // Release work guards so io_context::run() can return
    for (auto& w : workers_) {
        if (w->workGuard) {
            w->workGuard->reset();
        }
    }
    // Stop all worker io_contexts — pending async operations are cancelled,
    // serve() loops catch operation_aborted and exit cleanly.
    for (auto& w : workers_) {
        w->ioCtx.stop();
    }
    // Wait for all active connections to drain (coroutines finishing cleanup)
    for (int i = 0; i < 500 && activeConnections_.load(std::memory_order_relaxed) > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void HttpServer::addSseRoute(
    const std::string&                                                         path,
    std::function<asio::awaitable<void>(Request&, std::shared_ptr<SseWriter>)> handler
) {
    sseRoutes_[path] = std::move(handler);
}

void HttpServer::enableWebSocket(const std::string& path, WsHandler handler) {
    wsRoutes_[path] = std::move(handler);
}

void HttpServer::enableWebSocketSsl(const std::string& path, WssHandler handler) {
    wsSslRoutes_[path] = std::move(handler);
}

void HttpServer::startAsync(asio::any_io_executor executor) {
    if (stopped_) {
        return;
    }
    stopped_   = false;
    asyncMode_ = true;

    using tcp = asio::ip::tcp;

    auto const    address = asio::ip::make_address(config_.address);
    tcp::endpoint endpoint(address, config_.port);
    acceptor_ = std::make_unique<tcp::acceptor>(executor);
    acceptor_->open(endpoint.protocol());
    acceptor_->set_option(tcp::acceptor::reuse_address(true));
    acceptor_->bind(endpoint);
    acceptor_->listen(asio::socket_base::max_listen_connections);

    if (!config_.sslCertFile.empty() && !config_.sslKeyFile.empty()) {
        sslCtx_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv12_server);
        sslCtx_->set_options(
            asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2
            | asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1
            | asio::ssl::context::no_tlsv1_1 | asio::ssl::context::single_dh_use
        );
        sslCtx_->use_certificate_chain_file(config_.sslCertFile);
        sslCtx_->use_private_key_file(config_.sslKeyFile, asio::ssl::context::pem);
    }

    asio::co_spawn(executor, acceptLoopAsync(), asio::detached);

    XX_OUT("[server] Listening on {}:{} (async mode)", config_.address, port());
}

asio::awaitable<void> HttpServer::serveTcp(
    std::shared_ptr<boost::beast::tcp_stream> stream
) {
    ConnectionGuard guard{activeConnections_};
    co_await serve(std::move(*stream));
}

asio::awaitable<void> HttpServer::serveSsl(
    std::shared_ptr<boost::beast::ssl_stream<boost::beast::tcp_stream>> stream
) {
    ConnectionGuard guard{activeConnections_};
    co_await sslHandshakeAndServe(stream);
}

asio::awaitable<void> HttpServer::acceptLoop() {
    using tcp     = asio::ip::tcp;
    auto executor = co_await asio::this_coro::executor;
    while (!stopped_) {
        boost::system::error_code ec;
        tcp::socket               socket
            = co_await acceptor_->async_accept(asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            if (ec == asio::error::operation_aborted || ec == asio::error::connection_aborted) {
                co_return;
            }
            if (ec == asio::error::no_descriptors) {
                XX_LOGW("[server] Accept: too many open files, retrying in 100ms");
                asio::steady_timer timer(executor, std::chrono::milliseconds(100));
                co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                continue;
            }
            XX_LOGE("[server] Accept error: {}", ec.message());
            asio::steady_timer timer(executor, std::chrono::milliseconds(10));
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if (stopped_) {
                co_return;
            }
            continue;
        }

        boost::system::error_code tcpEc;
        socket.set_option(asio::ip::tcp::no_delay(true), tcpEc);

        if (activeConnections_.load(std::memory_order_relaxed) >= config_.maxConnections) {
            boost::system::error_code closeEc;
            socket.shutdown(tcp::socket::shutdown_both, closeEc);
            socket.close(closeEc);
            XX_LOGW("[server] Max connections reached, dropping client");
            continue;
        }

        activeConnections_.fetch_add(1, std::memory_order_relaxed);

        size_t idx          = nextWorker_++ % workers_.size();
        auto&  targetWorker = *workers_[idx];

        auto        protocol = acceptor_->local_endpoint().protocol();
        tcp::socket workerSocket(targetWorker.ioCtx);
        workerSocket.assign(protocol, socket.release());

        if (sslCtx_) {
            auto sslStream = std::make_shared<boost::beast::ssl_stream<boost::beast::tcp_stream>>(
                boost::beast::tcp_stream(std::move(workerSocket)),
                *sslCtx_
            );
            asio::co_spawn(targetWorker.ioCtx, serveSsl(std::move(sslStream)), asio::detached);
        } else {
            auto stream = std::make_shared<boost::beast::tcp_stream>(std::move(workerSocket));
            asio::co_spawn(targetWorker.ioCtx, serveTcp(std::move(stream)), asio::detached);
        }
    }
    co_return;
}

asio::awaitable<void> HttpServer::acceptLoopAsync() {
    using tcp     = asio::ip::tcp;
    auto executor = co_await asio::this_coro::executor;
    while (!stopped_) {
        boost::system::error_code ec;
        tcp::socket               socket
            = co_await acceptor_->async_accept(asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            if (ec == asio::error::operation_aborted || ec == asio::error::connection_aborted) {
                co_return;
            }
            if (ec == asio::error::no_descriptors) {
                XX_LOGW("[server] Accept: too many open files, retrying in 100ms");
                asio::steady_timer timer(executor, std::chrono::milliseconds(100));
                co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                continue;
            }
            XX_LOGE("[server] Accept error: {}", ec.message());
            asio::steady_timer timer(executor, std::chrono::milliseconds(10));
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if (stopped_) {
                co_return;
            }
            continue;
        }

        boost::system::error_code tcpEc;
        socket.set_option(asio::ip::tcp::no_delay(true), tcpEc);

        if (activeConnections_.load(std::memory_order_relaxed) >= config_.maxConnections) {
            boost::system::error_code closeEc;
            socket.shutdown(tcp::socket::shutdown_both, closeEc);
            socket.close(closeEc);
            XX_LOGW("[server] Max connections reached, dropping client");
            continue;
        }

        activeConnections_.fetch_add(1, std::memory_order_relaxed);

        auto stream = std::make_shared<boost::beast::tcp_stream>(std::move(socket));
        asio::co_spawn(executor, serveTcp(std::move(stream)), asio::detached);
    }
    co_return;
}

asio::awaitable<void> HttpServer::sslHandshakeAndServe(
    std::shared_ptr<boost::beast::ssl_stream<boost::beast::tcp_stream>> stream
) {
    try {
        co_await stream->async_handshake(
            asio::ssl::stream_base::server,
            asio::cancel_after(config_.requestTimeout, asio::use_awaitable)
        );
        co_await serve(std::move(*stream));
    } catch (const boost::system::system_error& e) {
        if (e.code() != asio::error::operation_aborted
            && e.code() != asio::ssl::error::stream_truncated) {
            XX_LOGE("[server] SSL error: {}", agentxx::util::autoTryConvertToUtf8(e.what()));
        }
    } catch (const std::exception& e) {
        XX_LOGE("[server] SSL handshake error: {}", e.what());
    }
    // activeConnections_ decrement handled by ConnectionGuard in caller
}

void HttpServer::fillError(
    Response&                  resp,
    unsigned                   version,
    boost::beast::http::status status,
    std::string_view           message
) {
    resp.version(version);
    resp.result(status);
    resp.set(boost::beast::http::field::content_type, "text/plain");
    resp.body() = std::string(message);
    resp.prepare_payload();
    resp.keep_alive(false);
}

} // namespace util
} // namespace agentxx
