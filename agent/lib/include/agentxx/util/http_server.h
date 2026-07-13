#pragma once

#include "agentxx/util/log.h"
#include "agentxx/util/router.h"
#include "hical/core/Coroutine.h"
#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <atomic>
#include <chrono>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace agentxx {
namespace util {

/// HTTP method index for the router (0-8 maps to standard methods)
inline int httpMethodIndex(boost::beast::http::verb v) noexcept {
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

/// Strip query string from a request target, returning just the path portion
inline std::string_view requestPath(std::string_view target) noexcept {
  auto q = target.find('?');
  return q == std::string_view::npos ? target : target.substr(0, q);
}

/// Format a time_point as an RFC 7231 IMF-fixdate string
/// (e.g. "Sun, 06 Nov 1994 08:49:37 GMT")
inline std::string formatHttpDate(std::time_t t) noexcept {
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

// ---------------------------------------------------------------------------
// HttpServer
// ---------------------------------------------------------------------------

class HttpServer {
public:
  using Request = boost::beast::http::request<boost::beast::http::string_body>;
  using Response =
      boost::beast::http::response<boost::beast::http::string_body>;
  using Handler = std::function<hical::Awaitable<void>(
      Request &, Response &, const std::string &matched_path)>;
  using Router = XXRouter<Handler, 9>;

  struct Config {
    std::string address = "0.0.0.0";
    uint16_t port = 8080;
    unsigned ioThreads = 0; // 0 = hardware_concurrency
    std::chrono::seconds requestTimeout{30};

    // SSL (optional – set both to enable)
    std::string sslCertFile;
    std::string sslKeyFile;

    size_t maxConnections = 8192;

    // DoS protection: reject requests with oversized headers or bodies
    uint32_t maxHeaderSize = 8192;       // default 8 KB header limit
    uint64_t maxRequestBody = 8 * 1024 * 1024; // default 8 MB body limit

    // Per-request access log (false = silent in release for performance)
    bool accessLogEnabled = true;
  };

  explicit HttpServer(Config config)
      : config_(std::move(config)), signals_(ioContext_, SIGINT, SIGTERM) {
    router_ = std::make_unique<Router>();
  }

  HttpServer(const HttpServer &) = delete;
  HttpServer &operator=(const HttpServer &) = delete;

  ~HttpServer() { stop(); }

  Router &router() { return *router_; }
  const Config &config() const noexcept { return config_; }

  uint16_t port() const noexcept {
    if (acceptor_ && acceptor_->is_open()) {
      boost::system::error_code ec;
      auto ep = acceptor_->local_endpoint(ec);
      if (!ec)
        return ep.port();
    }
    return 0;
  }

  size_t activeConnections() const noexcept {
    return activeConnections_.load(std::memory_order_relaxed);
  }

  // -----------------------------------------------------------------------
  // Start / Stop
  // -----------------------------------------------------------------------

  /// Block the calling thread until the server stops.
  void start() {
    if (stopped_)
      return;
    stopped_ = false;

    using tcp = boost::asio::ip::tcp;
    auto const address = boost::asio::ip::make_address(config_.address);
    tcp::endpoint endpoint(address, config_.port);

    // Acceptor
    acceptor_ = std::make_unique<tcp::acceptor>(ioContext_);
    acceptor_->open(endpoint.protocol());
    acceptor_->set_option(tcp::acceptor::reuse_address(true));
    acceptor_->bind(endpoint);
    // Use max_listen_connections for the backlog to handle connection bursts
    acceptor_->listen(boost::asio::socket_base::max_listen_connections);

    // Setup SSL context if configured
    if (!config_.sslCertFile.empty() && !config_.sslKeyFile.empty()) {
      sslCtx_ = std::make_unique<boost::asio::ssl::context>(
          boost::asio::ssl::context::tlsv12_server);
      // Disable deprecated protocols (SSLv2, SSLv3, TLS 1.0, TLS 1.1)
      sslCtx_->set_options(boost::asio::ssl::context::default_workarounds |
                           boost::asio::ssl::context::no_sslv2 |
                           boost::asio::ssl::context::no_sslv3 |
                           boost::asio::ssl::context::no_tlsv1 |
                           boost::asio::ssl::context::no_tlsv1_1 |
                           boost::asio::ssl::context::single_dh_use);
      sslCtx_->use_certificate_chain_file(config_.sslCertFile);
      sslCtx_->use_private_key_file(config_.sslKeyFile,
                                     boost::asio::ssl::context::pem);
    }

    // Signal handlers for graceful shutdown
    signals_.async_wait([this](boost::system::error_code ec, int sig) {
      if (ec)
        return;
      XX_OUT("[server] Signal {} received, shutting down...", sig);
      stop();
    });

    // Spawn listener coroutine
    hical::coSpawn(ioContext_, [this]() -> hical::Awaitable<void> {
      return acceptLoop();
    });

    // Start IO threads
    unsigned threadCount = config_.ioThreads;
    if (threadCount == 0)
      threadCount = std::thread::hardware_concurrency();
    if (threadCount == 0)
      threadCount = 1;

    threads_.reserve(threadCount);
    for (unsigned i = 0; i < threadCount; ++i) {
      threads_.emplace_back([this] { ioContext_.run(); });
    }

    XX_OUT("[server] Listening on {}:{}{}", config_.address, port(),
           sslCtx_ ? " (HTTPS)" : "");

    // Block until all threads finish
    for (auto &t : threads_) {
      if (t.joinable())
        t.join();
    }
    threads_.clear();
  }

  /// Signal the server to stop. Safe to call from any thread.
  void stop() {
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true))
      return;
    boost::system::error_code ec;
    if (acceptor_ && acceptor_->is_open()) {
      acceptor_->cancel(ec);
      acceptor_->close(ec);
    }
    signals_.cancel(ec);
    // Stop the io_context — pending async operations are cancelled,
    // serve() loops catch operation_aborted and exit cleanly.
    ioContext_.stop();
  }

  bool isStopped() const noexcept { return stopped_; }

private:
  // -----------------------------------------------------------------------
  // RAII guard for active connection counting — ensures decrement even
  // if the session coroutine throws or is cancelled.
  // -----------------------------------------------------------------------
  struct ConnectionGuard {
    std::atomic<size_t> &counter;
    explicit ConnectionGuard(std::atomic<size_t> &c) : counter(c) {}
    ~ConnectionGuard() { counter.fetch_sub(1, std::memory_order_relaxed); }
    ConnectionGuard(const ConnectionGuard &) = delete;
    ConnectionGuard &operator=(const ConnectionGuard &) = delete;
  };

  // -----------------------------------------------------------------------
  // Accept loop
  // -----------------------------------------------------------------------

  hical::Awaitable<void> acceptLoop() {
    using tcp = boost::asio::ip::tcp;
    auto executor = co_await boost::asio::this_coro::executor;

    while (!stopped_) {
      boost::system::error_code ec;
      tcp::socket socket = co_await acceptor_->async_accept(
          boost::asio::redirect_error(boost::asio::use_awaitable, ec));
      if (ec) {
        if (ec == boost::asio::error::operation_aborted ||
            ec == boost::asio::error::connection_aborted)
          co_return;
        // EMFILE (too many open files): wait briefly and retry rather
        // than killing the accept loop — prevents fd exhaustion DoS.
        if (ec == boost::asio::error::no_descriptors) {
          XX_LOGW("[server] Accept: too many open files, retrying in 100ms");
          boost::asio::steady_timer timer(executor,
                                          std::chrono::milliseconds(100));
          co_await timer.async_wait(
              boost::asio::redirect_error(boost::asio::use_awaitable, ec));
          continue;
        }
        XX_LOGE("[server] Accept error: {}", ec.message());
        // Brief backoff on other errors to avoid tight error loop
        boost::asio::steady_timer timer(executor, std::chrono::milliseconds(10));
        co_await timer.async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (stopped_)
          co_return;
        continue;
      }

      // Set TCP no_delay immediately for lowest latency (before SSL handshake)
      boost::system::error_code tcpEc;
      socket.set_option(boost::asio::ip::tcp::no_delay(true), tcpEc);

      // Enforce max connections
      if (activeConnections_.load(std::memory_order_relaxed) >=
          config_.maxConnections) {
        boost::system::error_code closeEc;
        socket.shutdown(tcp::socket::shutdown_both, closeEc);
        socket.close(closeEc);
        XX_LOGW("[server] Max connections reached, dropping client");
        continue;
      }

      // Increment on accept loop thread (no race with the check above).
      // The ConnectionGuard inside the coroutine ensures the decrement
      // runs even if serve() throws or the coroutine is cancelled.
      activeConnections_.fetch_add(1, std::memory_order_relaxed);

      if (sslCtx_) {
        // SSL session
        auto sslStream = std::make_shared<
            boost::beast::ssl_stream<boost::beast::tcp_stream>>(
            boost::beast::tcp_stream(std::move(socket)), *sslCtx_);
        hical::coSpawn(
            executor, [this, sslStream]() -> hical::Awaitable<void> {
              ConnectionGuard guard{activeConnections_};
              co_await sslHandshakeAndServe(sslStream);
            });
      } else {
        // Plain session
        auto stream =
            std::make_shared<boost::beast::tcp_stream>(std::move(socket));
        hical::coSpawn(
            executor, [this, stream]() -> hical::Awaitable<void> {
              ConnectionGuard guard{activeConnections_};
              co_await serve(std::move(*stream));
            });
      }
    }
    co_return;
  }

  // -----------------------------------------------------------------------
  // SSL helper
  // -----------------------------------------------------------------------

  hical::Awaitable<void> sslHandshakeAndServe(
      std::shared_ptr<boost::beast::ssl_stream<boost::beast::tcp_stream>>
          stream) {
    try {
      co_await stream->async_handshake(
          boost::asio::ssl::stream_base::server,
          boost::asio::cancel_after(config_.requestTimeout,
                                    boost::asio::use_awaitable));
      co_await serve(std::move(*stream));
    } catch (const boost::system::system_error &e) {
      if (e.code() != boost::asio::error::operation_aborted &&
          e.code() != boost::asio::ssl::error::stream_truncated) {
        XX_LOGE("[server] SSL error: {}", e.what());
      }
    } catch (const std::exception &e) {
      XX_LOGE("[server] SSL handshake error: {}", e.what());
    }
    // activeConnections_ decrement handled by ConnectionGuard in caller
  }

  // -----------------------------------------------------------------------
  // Session handler (template – works with tcp_stream & ssl_stream)
  // -----------------------------------------------------------------------

  template <typename Stream>
  hical::Awaitable<void> serve(Stream stream) {
    namespace http = boost::beast::http;
    boost::system::error_code ec;

    boost::beast::flat_buffer buffer;
    bool keepAlive = false;
    bool readError = false;
    std::string readErrorMsg;
    http::status readErrorStatus = http::status::bad_request;

    do {
      // Read one request with body/header size limits for DoS protection
      readError = false;
      Request req;
      try {
        http::request_parser<http::string_body> parser;
        parser.header_limit(config_.maxHeaderSize);
        parser.body_limit(config_.maxRequestBody);
        co_await http::async_read(
            stream, buffer, parser,
            boost::asio::cancel_after(config_.requestTimeout,
                                      boost::asio::use_awaitable));
        req = parser.release();
      } catch (const boost::system::system_error &e) {
        if (e.code() == http::error::end_of_stream ||
            e.code() == boost::asio::error::eof ||
            e.code() == boost::asio::error::operation_aborted)
          break;
        readError = true;
        readErrorMsg = e.what();
        if (e.code() == http::error::body_limit)
          readErrorStatus = http::status::payload_too_large;
        else if (e.code() == http::error::header_limit)
          readErrorStatus = http::status::request_header_fields_too_large;
        else
          readErrorStatus = http::status::bad_request;
      }

      if (readError) {
        Response errResp;
        errResp.version(11);
        errResp.result(readErrorStatus);
        errResp.set(http::field::content_type, "text/plain");
        errResp.body() = "Bad Request: " + readErrorMsg;
        errResp.prepare_payload();
        errResp.keep_alive(false);
        co_await http::async_write(
            stream, errResp,
            boost::asio::cancel_after(std::chrono::seconds(5),
                                      boost::asio::use_awaitable));
        break;
      }

      // Prepare response
      Response resp;
      resp.version(req.version());
      resp.set(http::field::server, "agentxx/1.0");
      // RFC 7231: servers SHOULD include a Date header
      resp.set(http::field::date, formatHttpDate(std::time(nullptr)));

      // Extract path (strip query string) and dispatch
      std::string path(requestPath(req.target()));
      int methodIdx = httpMethodIndex(req.method());
      std::string matchedPath;
      bool handled = false;

      if (methodIdx >= 0) {
        auto handler = router_->get(path, methodIdx, matchedPath);
        if (handler && *handler) {
          try {
            co_await (*handler)(req, resp, matchedPath);
            handled = true;
          } catch (const std::exception &e) {
            XX_LOGE("[server] Handler error [{} {}]: {}",
                    req.method_string(), req.target(), e.what());
            fillError(resp, req.version(),
                      http::status::internal_server_error,
                      "Internal Server Error");
          }
        }
      }

      if (!handled) {
        std::string dummyPath;
        bool hasAnyRoute = false;
        for (int m = 0; m < 9; ++m) {
          if (m == methodIdx)
            continue;
          if (router_->getNocache(path, m, dummyPath)) {
            hasAnyRoute = true;
            break;
          }
        }
        if (hasAnyRoute) {
          fillError(resp, req.version(), http::status::method_not_allowed,
                    "Method Not Allowed");
          resp.set(http::field::allow,
                   "GET, HEAD, POST, PUT, DELETE, PATCH, OPTIONS");
        } else {
          fillError(resp, req.version(), http::status::not_found, "Not Found");
        }
      }

      // Ensure content-length is set
      if (!resp.payload_size().has_value())
        resp.prepare_payload();

      // Determine keep-alive
      keepAlive = resp.keep_alive();
      if (keepAlive && stopped_)
        keepAlive = false;
      resp.set(http::field::connection, keepAlive ? "keep-alive" : "close");

      // Send response
      co_await http::async_write(
          stream, resp,
          boost::asio::cancel_after(config_.requestTimeout,
                                    boost::asio::use_awaitable));

      // Per-request access log (compiled out in release via XX_LOGI)
      if (config_.accessLogEnabled) {
        XX_LOGI("{} {} -> {} ({})", req.method_string(), req.target(),
                resp.result_int(), resp.body().size());
      }

    } while (keepAlive && !stopped_);

    // Graceful close: shutdown send side, ignore errors
    ec = {};
    boost::beast::get_lowest_layer(stream).socket().shutdown(
        boost::asio::ip::tcp::socket::shutdown_send, ec);
  }

  // -----------------------------------------------------------------------
  // Helpers
  // -----------------------------------------------------------------------

  static void fillError(Response &resp, unsigned version,
                        boost::beast::http::status status,
                        std::string_view message) {
    resp.version(version);
    resp.result(status);
    resp.set(boost::beast::http::field::content_type, "text/plain");
    resp.body() = std::string(message);
    resp.prepare_payload();
    resp.keep_alive(false);
  }

  // -----------------------------------------------------------------------
  // Members
  // -----------------------------------------------------------------------

  Config config_;
  boost::asio::io_context ioContext_;
  std::unique_ptr<Router> router_;
  std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
  std::unique_ptr<boost::asio::ssl::context> sslCtx_;
  boost::asio::signal_set signals_;
  std::atomic<bool> stopped_{false};
  std::atomic<size_t> activeConnections_{0};
  std::vector<std::thread> threads_;
};

} // namespace util
} // namespace agentxx
