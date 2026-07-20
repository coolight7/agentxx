#pragma once

#include "agentxx/util/log.h"
#include "agentxx/util/router.h"
#include "asio/awaitable.hpp"
#include "asio/cancel_after.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/redirect_error.hpp"
#include "asio/signal_set.hpp"
#include "asio/use_awaitable.hpp"
#include <atomic>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <chrono>
#include <ctime>
#include <functional>
#include <list>
#include <memory>
#include <neograph/api.h>
#include <neograph/json.h>
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
  using Handler = std::function<asio::awaitable<void>(
      Request &, Response &, const std::string &matched_path)>;
  using Router = XXRouter<Handler, 9>;

  /// Streaming SSE connection — the handler can hold onto this to push
  /// events after the initial response header has been sent.
  class SseWriter {
  public:
    virtual ~SseWriter() = default;
    /// Write an SSE event (formatted as "event: ...\ndata: ...\n\n").
    virtual asio::awaitable<bool>
    writeEvent(std::string_view event, std::string_view data) = 0;
    /// Write a raw chunk (must be valid SSE framing).
    virtual asio::awaitable<bool> writeChunk(std::string_view chunk) = 0;
    /// Close the SSE stream gracefully.
    virtual asio::awaitable<void> close() = 0;
  };

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
    uint32_t maxHeaderSize = 8192;             // default 8 KB header limit
    uint64_t maxRequestBody = 8 * 1024 * 1024; // default 8 MB body limit

    // Per-request access log (false = silent in release for performance)
    bool accessLogEnabled = true;
  };

  explicit HttpServer(Config config) : config_(std::move(config)) {
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

    using tcp = asio::ip::tcp;

    // Create workers (each has its own io_context — zero-lock isolation)
    unsigned threadCount = config_.ioThreads;
    if (threadCount == 0)
      threadCount = std::thread::hardware_concurrency();
    if (threadCount == 0)
      threadCount = 1;

    workers_.reserve(threadCount);
    for (unsigned i = 0; i < threadCount; ++i)
      workers_.push_back(std::make_unique<Worker>());

    auto &mainCtx = workers_[0]->ioCtx;

    // Acceptor — runs on the first worker's io_context
    auto const address = asio::ip::make_address(config_.address);
    tcp::endpoint endpoint(address, config_.port);
    acceptor_ = std::make_unique<tcp::acceptor>(mainCtx);
    acceptor_->open(endpoint.protocol());
    acceptor_->set_option(tcp::acceptor::reuse_address(true));
    acceptor_->bind(endpoint);
    acceptor_->listen(asio::socket_base::max_listen_connections);

    // Setup SSL context if configured
    if (!config_.sslCertFile.empty() && !config_.sslKeyFile.empty()) {
      sslCtx_ = std::make_unique<asio::ssl::context>(
          asio::ssl::context::tlsv12_server);
      sslCtx_->set_options(
          asio::ssl::context::default_workarounds |
          asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 |
          asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1 |
          asio::ssl::context::single_dh_use);
      sslCtx_->use_certificate_chain_file(config_.sslCertFile);
      sslCtx_->use_private_key_file(config_.sslKeyFile,
                                    asio::ssl::context::pem);
    }

    // Spawn listener coroutine on first worker's io_context
    asio::co_spawn(mainCtx, acceptLoop(), asio::detached);

    XX_OUT("[server] Listening on {}:{}{}", config_.address, port(),
           sslCtx_ ? " (HTTPS)" : "");

    // Start worker threads — each runs its own io_context
    for (unsigned i = 0; i < threadCount; ++i) {
      workers_[i]->thread =
          std::thread([this, i]() { workers_[i]->ioCtx.run(); });
    }

    // Block until all threads finish
    for (auto &w : workers_) {
      if (w->thread.joinable())
        w->thread.join();
    }
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
    // Stop all worker io_contexts — pending async operations are cancelled,
    // serve() loops catch operation_aborted and exit cleanly.
    for (auto &w : workers_) {
      w->ioCtx.stop();
    }
  }

  bool isStopped() const noexcept { return stopped_; }

  /// Register a streaming SSE handler. Unlike normal handlers, the SSE
  /// handler receives an SseWriter to push events incrementally.
  void addSseRoute(const std::string &path,
                   std::function<asio::awaitable<void>(
                       Request &, std::shared_ptr<SseWriter>)> handler) {
    sseRoutes_[path] = std::move(handler);
  }

private:
  // -----------------------------------------------------------------------
  // SseWriter implementation backed by a TCP/SSL stream
  // -----------------------------------------------------------------------
  template <typename Stream> class SseWriterImpl : public SseWriter {
    Stream &stream_;
    std::chrono::seconds timeout_;
    bool headerSent_ = false;

  public:
    explicit SseWriterImpl(Stream &s, std::chrono::seconds t)
        : stream_(s), timeout_(t) {}

    asio::awaitable<bool> writeEvent(std::string_view event,
                                     std::string_view data) override {
      std::string chunk;
      if (!event.empty())
        chunk += "event: " + std::string(event) + "\n";
      chunk += "data: " + std::string(data) + "\n\n";
      co_return co_await doWrite(chunk);
    }

    asio::awaitable<bool> writeChunk(std::string_view chunk) override {
      co_return co_await doWrite(chunk);
    }

    asio::awaitable<void> close() override {
      boost::system::error_code ec;
      boost::beast::get_lowest_layer(stream_).socket().shutdown(
          asio::ip::tcp::socket::shutdown_send, ec);
      co_return;
    }

  private:
    asio::awaitable<bool> doWrite(std::string_view data) {
      try {
        if (!headerSent_) {
          co_return co_await writeWithHeader(data);
        }
        co_await boost::asio::async_write(
            stream_, boost::asio::buffer(data),
            asio::cancel_after(timeout_, asio::use_awaitable));
        co_return true;
      } catch (const boost::system::system_error &e) {
        if (e.code() != asio::error::operation_aborted)
          XX_LOGE("[sse] write error: {}", e.what());
        co_return false;
      } catch (const std::exception &e) {
        XX_LOGE("[sse] write error: {}", e.what());
        co_return false;
      }
    }

    asio::awaitable<bool> writeWithHeader(std::string_view data) {
      namespace http = boost::beast::http;
      headerSent_ = true;
      http::response<http::string_body> resp;
      resp.version(11);
      resp.result(http::status::ok);
      resp.set(http::field::content_type, "text/event-stream");
      resp.set(http::field::cache_control, "no-cache");
      resp.set(http::field::connection, "keep-alive");
      resp.set("X-Accel-Buffering", "no");
      resp.chunked(true);
      resp.body() = std::string(data);
      resp.prepare_payload();
      co_await http::async_write(
          stream_, resp,
          asio::cancel_after(timeout_, asio::use_awaitable));
      co_return true;
    }
  };

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

  asio::awaitable<void> acceptLoop() {
    using tcp = asio::ip::tcp;
    auto executor = co_await asio::this_coro::executor;
    while (!stopped_) {
      boost::system::error_code ec;
      tcp::socket socket = co_await acceptor_->async_accept(
          asio::redirect_error(asio::use_awaitable, ec));
      if (ec) {
        if (ec == asio::error::operation_aborted ||
            ec == asio::error::connection_aborted)
          co_return;
        if (ec == asio::error::no_descriptors) {
          XX_LOGW("[server] Accept: too many open files, retrying in 100ms");
          asio::steady_timer timer(executor, std::chrono::milliseconds(100));
          co_await timer.async_wait(
              asio::redirect_error(asio::use_awaitable, ec));
          continue;
        }
        XX_LOGE("[server] Accept error: {}", ec.message());
        asio::steady_timer timer(executor, std::chrono::milliseconds(10));
        co_await timer.async_wait(
            asio::redirect_error(asio::use_awaitable, ec));
        if (stopped_)
          co_return;
        continue;
      }

      // Set TCP no_delay immediately for lowest latency
      boost::system::error_code tcpEc;
      socket.set_option(asio::ip::tcp::no_delay(true), tcpEc);

      // Enforce max connections
      if (activeConnections_.load(std::memory_order_relaxed) >=
          config_.maxConnections) {
        boost::system::error_code closeEc;
        socket.shutdown(tcp::socket::shutdown_both, closeEc);
        socket.close(closeEc);
        XX_LOGW("[server] Max connections reached, dropping client");
        continue;
      }

      activeConnections_.fetch_add(1, std::memory_order_relaxed);

      // Round-robin dispatch to a per-thread worker io_context
      size_t idx = nextWorker_++ % workers_.size();
      auto &targetWorker = *workers_[idx];

      // Transfer native handle to target worker's io_context (no locks)
      auto protocol = acceptor_->local_endpoint().protocol();
      tcp::socket workerSocket(targetWorker.ioCtx);
      workerSocket.assign(protocol, socket.release());

      if (sslCtx_) {
        auto sslStream = std::make_shared<
            boost::beast::ssl_stream<boost::beast::tcp_stream>>(
            boost::beast::tcp_stream(std::move(workerSocket)), *sslCtx_);
        asio::co_spawn(
            targetWorker.ioCtx,
            [this, sslStream]() -> asio::awaitable<void> {
              ConnectionGuard guard{activeConnections_};
              co_await sslHandshakeAndServe(sslStream);
            }(),
            asio::detached);
      } else {
        auto stream =
            std::make_shared<boost::beast::tcp_stream>(std::move(workerSocket));
        asio::co_spawn(
            targetWorker.ioCtx,
            [this, stream]() -> asio::awaitable<void> {
              ConnectionGuard guard{activeConnections_};
              co_await serve(std::move(*stream));
            }(),
            asio::detached);
      }
    }
    co_return;
  }

  // -----------------------------------------------------------------------
  // SSL helper
  // -----------------------------------------------------------------------

  asio::awaitable<void> sslHandshakeAndServe(
      std::shared_ptr<boost::beast::ssl_stream<boost::beast::tcp_stream>>
          stream) {
    try {
      co_await stream->async_handshake(
          asio::ssl::stream_base::server,
          asio::cancel_after(config_.requestTimeout, asio::use_awaitable));
      co_await serve(std::move(*stream));
    } catch (const boost::system::system_error &e) {
      if (e.code() != asio::error::operation_aborted &&
          e.code() != asio::ssl::error::stream_truncated) {
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

  template <typename Stream> asio::awaitable<void> serve(Stream stream) {
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
            asio::cancel_after(config_.requestTimeout, asio::use_awaitable));
        req = parser.release();
      } catch (const boost::system::system_error &e) {
        if (e.code() == http::error::end_of_stream ||
            e.code() == asio::error::eof ||
            e.code() == asio::error::operation_aborted)
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
            asio::cancel_after(std::chrono::seconds(5), asio::use_awaitable));
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

      // Check for SSE streaming route first (GET only)
      if (methodIdx == 0) { // GET
        auto sseIt = sseRoutes_.find(path);
        if (sseIt != sseRoutes_.end()) {
          try {
            auto writer = std::make_shared<SseWriterImpl<Stream>>(
                stream, std::chrono::seconds{30});
            co_await sseIt->second(req, writer);
            handled = true;
            // SSE streaming handled, skip normal response write
            // The connection is kept alive by the SseWriter
            // But we still need to let serve() know not to continue
            // We'll set a flag and break out
            break;
          } catch (const std::exception &e) {
            XX_LOGE("[server] SSE handler error [{} {}]: {}",
                    req.method_string(), req.target(), e.what());
            fillError(resp, req.version(), http::status::internal_server_error,
                      "Internal Server Error");
          }
        }
      }

      if (methodIdx >= 0 && !handled) {
        auto handler = router_->get(path, methodIdx, matchedPath);
        if (handler && *handler) {
          try {
            co_await (*handler)(req, resp, matchedPath);
            handled = true;
          } catch (const std::exception &e) {
            XX_LOGE("[server] Handler error [{} {}]: {}", req.method_string(),
                    req.target(), e.what());
            fillError(resp, req.version(), http::status::internal_server_error,
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

      // Ensure Content-Length is set. Always call prepare_payload() —
      // payload_size() returns body_.size() for string_body (0 for empty),
      // so the conditional check would skip it and leave no Content-Length.
      resp.prepare_payload();

      // Respect the client's Connection header: if client sent "close",
      // don't keep the connection alive.
      bool clientClose =
          req.find(http::field::connection) != req.end() &&
          boost::beast::iequals(req[http::field::connection], "close");
      keepAlive = resp.keep_alive();
      if (keepAlive && (stopped_ || clientClose))
        keepAlive = false;
      resp.set(http::field::connection, keepAlive ? "keep-alive" : "close");

      // Send response
      co_await http::async_write(
          stream, resp,
          asio::cancel_after(config_.requestTimeout, asio::use_awaitable));

      // Per-request access log (compiled out in release via XX_LOGI)
      if (config_.accessLogEnabled) {
        XX_LOGI("{} {} -> {} ({})", req.method_string(), req.target(),
                resp.result_int(), resp.body().size());
      }

    } while (keepAlive && !stopped_);

    // Graceful close: shutdown send side, ignore errors
    ec = {};
    boost::beast::get_lowest_layer(stream).socket().shutdown(
        asio::ip::tcp::socket::shutdown_send, ec);
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
  // Per-thread worker: each owns a private io_context — zero-lock isolation
  // -----------------------------------------------------------------------
  struct Worker {
    asio::io_context ioCtx;
    std::thread thread;
  };

  // -----------------------------------------------------------------------
  // Members
  // -----------------------------------------------------------------------

  Config config_;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::unique_ptr<Router> router_;
  std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
  std::unique_ptr<asio::ssl::context> sslCtx_;
  std::atomic<bool> stopped_{false};
  std::atomic<size_t> activeConnections_{0};
  std::atomic<size_t> nextWorker_{0};

  /// SSE streaming routes (GET only) — keyed by path
  std::unordered_map<std::string,
                     std::function<asio::awaitable<void>(
                         Request &, std::shared_ptr<SseWriter>)>>
      sseRoutes_;
};

} // namespace util
} // namespace agentxx
