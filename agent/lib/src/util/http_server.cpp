#include "agentxx/util/http_server.h"
#include "agentxx/util/http_client.h"
#include <array>

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

std::string_view httpMethodName(int methodIdx) noexcept {
    static constexpr std::array<std::string_view, 9>
        names{"GET", "HEAD", "POST", "PUT", "DELETE", "CONNECT", "OPTIONS", "TRACE", "PATCH"};
    if (methodIdx < 0 || methodIdx >= static_cast<int>(names.size())) {
        return "UNKNOWN";
    }
    return names[static_cast<size_t>(methodIdx)];
}

std::string_view requestPath(std::string_view target) noexcept {
    // 兼容 absolute-form 请求 (代理风格, RFC 7230 §5.3.2):
    // "GET http://host:port/path?q=1 HTTP/1.1" → "/path"
    if (target.starts_with("http://") || target.starts_with("https://")) {
        auto schemeEnd = target.find("://");
        auto pathStart = target.find('/', schemeEnd + 3);
        if (pathStart == std::string_view::npos) {
            return "/";
        }
        target = target.substr(pathStart);
    }
    // 去除 query string; 容忍非法携带的 fragment (请求目标中不应出现 '#', 但防御性处理)
    auto q = target.find_first_of("?#");
    return q == std::string_view::npos ? target : target.substr(0, q);
}

std::string formatHttpDate(std::time_t t) noexcept {
    char buf[64];
#if XX_IS_WIN_D
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

namespace {

/// 解析监听地址: 优先按 IP 字面量解析; 主机名 (如 "localhost") 走 DNS 解析取首个结果,
/// 避免 make_address 直接抛异常导致服务无法启动
asio::ip::address resolveBindAddress(const std::string& address) {
    neograph_asio_error_code ec;
    auto                     addr = asio::ip::make_address(address, ec);
    if (!ec) {
        return addr;
    }
    asio::io_context         ctx;
    asio::ip::tcp::resolver  resolver(ctx);
    neograph_asio_error_code resolveEc;
    auto                     results = resolver.resolve(address, "0", resolveEc);
    if (resolveEc || results.empty()) {
        throw std::runtime_error{
            fmt::format("[server] invalid bind address: {} ({})", address, resolveEc.message())
        };
    }
    return results.begin()->endpoint().address();
}

} // namespace

HttpServer::HttpServer(Config config) :
    config_(std::move(config)) {
    router_ = std::make_unique<Router>();
}

HttpServer::~HttpServer() {
    stop();
}

uint16_t HttpServer::port() const noexcept {
    if (acceptor_ && acceptor_->is_open()) {
        neograph_asio_error_code ec;
        auto                     ep = acceptor_->local_endpoint(ec);
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
    auto const    address = resolveBindAddress(config_.address);
    tcp::endpoint endpoint(address, config_.port);
    acceptor_ = std::make_unique<tcp::acceptor>(mainCtx);
    acceptor_->open(endpoint.protocol());
    acceptor_->set_option(tcp::acceptor::reuse_address(true));
    acceptor_->bind(endpoint);
    acceptor_->listen(asio::socket_base::max_listen_connections);

    // Spawn listener coroutine on first worker's io_context
    asio::co_spawn(mainCtx, acceptLoop(), asio::detached);

    XX_OUT("[server] Listening on {}:{}", config_.address, port());

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
    neograph_asio_error_code ec;
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
    std::string_view                                                           path,
    std::function<asio::awaitable<void>(Request&, std::shared_ptr<SseWriter>)> handler
) {
    sseRoutes_[std::string{path}] = std::move(handler);
}

void HttpServer::addSsePostRoute(
    std::string_view                                                           path,
    std::function<asio::awaitable<void>(Request&, std::shared_ptr<SseWriter>)> handler
) {
    ssePostRoutes_[std::string{path}] = std::move(handler);
}

void HttpServer::enableWebSocket(std::string_view path, WsHandler handler) {
    wsRoutes_[std::string{path}] = std::move(handler);
}

void HttpServer::startAsync(asio::any_io_executor executor) {
    if (stopped_) {
        return;
    }
    stopped_   = false;
    asyncMode_ = true;

    using tcp = asio::ip::tcp;

    auto const    address = resolveBindAddress(config_.address);
    tcp::endpoint endpoint(address, config_.port);
    acceptor_ = std::make_unique<tcp::acceptor>(executor);
    acceptor_->open(endpoint.protocol());
    acceptor_->set_option(tcp::acceptor::reuse_address(true));
    acceptor_->bind(endpoint);
    acceptor_->listen(asio::socket_base::max_listen_connections);

    asio::co_spawn(executor, acceptLoopAsync(), asio::detached);

    XX_OUT("[server] Listening on {}:{} (async mode)", config_.address, port());
}

asio::awaitable<void> HttpServer::serveTcp(std::shared_ptr<boost::beast::tcp_stream> stream) {
    ConnectionGuard guard{activeConnections_};
    // 捕获异常, 避免客户端在读写期间断开时异常逃逸 detached 协程 → terminate;
    // 取消/中断 (CancelledException/NodeInterrupt, 经 handler 传播) 是预期行为, 静默返回
    try {
        co_await serve(std::move(*stream));
    } catch (const neograph::graph::CancelledException&) {
        // 取消: 连接终止是预期行为, 静默返回
    } catch (const neograph::graph::NodeInterrupt&) {
        // 中断: 同上
    } catch (const neograph_asio_system_error& e) {
        if (e.code() != asio::error::operation_aborted) {
            XX_LOGE("[server] TCP error: {}", agentxx::util::autoTryConvertToUtf8(e.what()));
        }
    } catch (const std::exception& e) {
        XX_LOGE("[server] TCP serve error: {}", e.what());
    }
}

asio::awaitable<void> HttpServer::acceptLoop() {
    using tcp     = asio::ip::tcp;
    auto executor = co_await asio::this_coro::executor;
    while (!stopped_) {
        neograph_asio_error_code ec;
        tcp::socket              socket
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

        neograph_asio_error_code tcpEc;
        socket.set_option(asio::ip::tcp::no_delay(true), tcpEc);

        if (activeConnections_.load(std::memory_order_relaxed) >= config_.maxConnections) {
            neograph_asio_error_code closeEc;
            socket.shutdown(tcp::socket::shutdown_both, closeEc);
            socket.close(closeEc);
            XX_LOGW("[server] Max connections reached, dropping client");
            continue;
        }

        activeConnections_.fetch_add(1, std::memory_order_relaxed);

        size_t idx          = nextWorker_++ % workers_.size();
        auto&  targetWorker = *workers_[idx];

        // 用 ec 重载: stop() 与本处存在竞态, acceptor 可能已被关闭,
        // 抛异常版本会使异常逃逸 detached 协程导致 terminate
        neograph_asio_error_code epEc;
        auto                     localEp = acceptor_->local_endpoint(epEc);
        if (epEc) {
            activeConnections_.fetch_sub(1, std::memory_order_relaxed);
            neograph_asio_error_code closeEc;
            socket.shutdown(tcp::socket::shutdown_both, closeEc);
            socket.close(closeEc);
            if (stopped_) {
                co_return;
            }
            continue;
        }
        tcp::socket workerSocket(targetWorker.ioCtx);
        workerSocket.assign(localEp.protocol(), socket.release());

        auto stream = std::make_shared<boost::beast::tcp_stream>(std::move(workerSocket));
        asio::co_spawn(targetWorker.ioCtx, serveTcp(std::move(stream)), asio::detached);
    }
    co_return;
}

asio::awaitable<void> HttpServer::acceptLoopAsync() {
    using tcp     = asio::ip::tcp;
    auto executor = co_await asio::this_coro::executor;
    while (!stopped_) {
        neograph_asio_error_code ec;
        tcp::socket              socket
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

        neograph_asio_error_code tcpEc;
        socket.set_option(asio::ip::tcp::no_delay(true), tcpEc);

        if (activeConnections_.load(std::memory_order_relaxed) >= config_.maxConnections) {
            neograph_asio_error_code closeEc;
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
