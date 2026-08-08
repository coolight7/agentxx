#pragma once

#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "agentxx/util/router.h"
#include "agentxx/util/string_util.h"
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
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <ctime>
#include <functional>
#include <list>
#include <memory>
#include <neograph/api.h>
#include <neograph/json.h>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace agentxx {
namespace util {

/// HTTP method index for the router (0-8 maps to standard methods)
int httpMethodIndex(boost::beast::http::verb v) noexcept;

/// HTTP method name by router index (0-8); returns "UNKNOWN" for invalid index
std::string_view httpMethodName(int methodIdx) noexcept;

/// Strip query string from a request target, returning just the path portion
std::string_view requestPath(std::string_view target) noexcept;

/// Format a time_point as an RFC 7231 IMF-fixdate string
/// (e.g. "Sun, 06 Nov 1994 08:49:37 GMT")
std::string formatHttpDate(std::time_t t) noexcept;

// ---------------------------------------------------------------------------
// HttpServer
// ---------------------------------------------------------------------------

class HttpServer {
public:

    using Request  = boost::beast::http::request<boost::beast::http::string_body>;
    using Response = boost::beast::http::response<boost::beast::http::string_body>;
    using Handler
        = std::function<asio::awaitable<void>(Request&, Response&, std::string_view matched_path)>;
    using Router = XXRouter<Handler, 9>;

    using WsStream  = boost::beast::websocket::stream<boost::beast::tcp_stream>;
    using WsHandler = std::function<asio::awaitable<void>(WsStream&)>;

    using WssStream
        = boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>;
    using WssHandler = std::function<asio::awaitable<void>(WssStream&)>;

    /// Streaming SSE connection — the handler can hold onto this to push
    /// events after the initial response header has been sent.
    class SseWriter {
    public:

        virtual ~SseWriter() = default;
        /// Write an SSE event (formatted as "event: ...\ndata: ...\n\n").
        virtual asio::awaitable<bool> writeEvent(std::string_view event, std::string_view data) = 0;
        /// Write a raw chunk (must be valid SSE framing).
        virtual asio::awaitable<bool> writeChunk(std::string_view chunk) = 0;
        /// Write a complete (non-SSE) HTTP response — used to return a plain
        /// JSON-RPC error/result from a streaming route (e.g. invalid
        /// subscriptions/listen params). After this the writer must not be
        /// used again.
        virtual asio::awaitable<bool> writeResponse(
            boost::beast::http::status status,
            std::string_view           contentType,
            std::string_view           body
        ) = 0;
        /// Close the SSE stream gracefully.
        virtual asio::awaitable<void> close() = 0;
    };

    struct Config {
        std::string          address   = "0.0.0.0";
        uint16_t             port      = 8080;
        unsigned             ioThreads = 0; // 0 = hardware_concurrency
        std::chrono::seconds requestTimeout{30};

        /// SSE 写入超时: 两次 SSE event 推送之间的最大间隔 (默认 120s, 适应 LLM 长思考)
        std::chrono::seconds sseWriteTimeout{120};

        // SSL (optional – set both to enable)
        std::string sslCertFile;
        std::string sslKeyFile;

        size_t maxConnections = 8192;

        /// 单个 keep-alive 连接的最大请求数 (0 = 无限制); 防止恶意客户端永久占用连接
        size_t maxRequestsPerConnection = 1024;

        // DoS protection: reject requests with oversized headers or bodies
        uint32_t maxHeaderSize  = 8192;            // default 8 KB header limit
        uint64_t maxRequestBody = 8 * 1024 * 1024; // default 8 MB body limit

        // Per-request access log (false = silent in release for performance)
        bool accessLogEnabled = true;
    };

    explicit HttpServer(Config config);

    HttpServer(const HttpServer&)            = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    ~HttpServer();

    Router& router() {
        return *router_;
    }

    const Config& config() const noexcept {
        return config_;
    }

    uint16_t port() const noexcept;

    size_t activeConnections() const noexcept {
        return activeConnections_.load(std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // Start / Stop
    // -----------------------------------------------------------------------

    /// Block the calling thread until the server stops.
    void start();

    /// Signal the server to stop. Safe to call from any thread.
    void stop();

    bool isStopped() const noexcept {
        return stopped_;
    }

    /// Register a streaming SSE handler. Unlike normal handlers, the SSE
    /// handler receives an SseWriter to push events incrementally.
    void addSseRoute(
        std::string_view                                                           path,
        std::function<asio::awaitable<void>(Request&, std::shared_ptr<SseWriter>)> handler
    );

    /// Register a streaming SSE handler for POST requests (e.g. MCP
    /// `subscriptions/listen`). The response is a long-lived SSE stream
    /// scoped to that request. Takes precedence over the normal router
    /// handler registered on the same path.
    void addSsePostRoute(
        std::string_view                                                           path,
        std::function<asio::awaitable<void>(Request&, std::shared_ptr<SseWriter>)> handler
    );

    /// Register a WebSocket endpoint. When a client sends an HTTP GET with
    /// Upgrade: websocket to the given path, the connection is upgraded and
    /// the handler is invoked with the websocket stream.
    void enableWebSocket(std::string_view path, WsHandler handler);

    /// Register a WebSocket endpoint for SSL (wss://) connections.
    void enableWebSocketSsl(std::string_view path, WssHandler handler);

    /// Start the accept loop on the given executor without creating threads.
    /// The caller is responsible for running the io_context.
    /// Suitable for single-threaded server mode (e.g. agent service).
    void startAsync(asio::any_io_executor executor);

private:

    // -----------------------------------------------------------------------
    // SseWriter implementation backed by a TCP/SSL stream
    // -----------------------------------------------------------------------
    template<typename Stream>
    class SseWriterImpl : public SseWriter {
        Stream&              stream_;
        std::chrono::seconds timeout_;
        bool                 headerSent_   = false;
        bool                 responseSent_ = false; // writeResponse 已发送完整响应

    public:

        explicit SseWriterImpl(Stream& s, std::chrono::seconds t) :
            stream_(s),
            timeout_(t) {}

        asio::awaitable<bool> writeEvent(std::string_view event, std::string_view data) override {
            std::string chunk;
            if (!event.empty()) {
                chunk += fmt::format("event: {}\n", event);
            }
            chunk += fmt::format("data: {}\n\n", data);
            co_return co_await doWrite(chunk);
        }

        asio::awaitable<bool> writeChunk(std::string_view chunk) override {
            co_return co_await doWrite(chunk);
        }

        asio::awaitable<bool> writeResponse(
            boost::beast::http::status status,
            std::string_view           contentType,
            std::string_view           body
        ) override {
            namespace http = boost::beast::http;
            co_return co_await agentxx::util::catchErrorAsync<bool>(
                [&]() -> asio::awaitable<bool> {
                    responseSent_               = true;
                    http::response<http::string_body> resp;
                    resp.version(11);
                    resp.result(status);
                    resp.set(http::field::content_type, contentType);
                    resp.set(http::field::connection, "close");
                    resp.body() = std::string(body);
                    resp.prepare_payload();
                    co_await http::async_write(
                        stream_,
                        resp,
                        asio::cancel_after(timeout_, asio::use_awaitable)
                    );
                    co_return true;
                },
                [&](std::string errinfo) -> asio::awaitable<bool> {
                    XX_LOGE("[sse] write response error: {}", errinfo);
                    co_return false;
                },
                [](std::string& errinfo) -> std::optional<bool> {
                    XX_LOGE("[sse] write response error: {}", errinfo);
                    return false;
                }
            );
        }

        asio::awaitable<void> close() override {
            // SSE 流已开始 (header 已发) 且未发送完整响应时, 写 chunked 终止帧,
            // 客户端解析器才能正确结束 (否则 keep-alive 连接会一直挂起)
            if (headerSent_ && !responseSent_) {
                neograph_asio_error_code ec;
                co_await asio::async_write(
                    stream_,
                    asio::buffer(std::string_view{"0\r\n\r\n"}),
                    asio::cancel_after(timeout_, asio::redirect_error(asio::use_awaitable, ec))
                );
            }
            neograph_asio_error_code ec;
            boost::beast::get_lowest_layer(stream_).socket().shutdown(
                asio::ip::tcp::socket::shutdown_send,
                ec
            );
            co_return;
        }

    private:

        asio::awaitable<bool> doWrite(std::string_view data) {
            co_return co_await agentxx::util::catchErrorAsync<bool>(
                [&]() -> asio::awaitable<bool> {
                    if (!headerSent_) {
                        co_return co_await writeWithHeader(data);
                    }
                    // 已声明 Transfer-Encoding: chunked, 后续数据必须按 chunk 帧格式写:
                    // <hex-size>\r\n<data>\r\n, 否则客户端解析器无法识别
                    std::string frame = fmt::format("{:x}\r\n{}\r\n", data.size(), data);
                    co_await asio::async_write(
                        stream_,
                        asio::buffer(frame),
                        asio::cancel_after(timeout_, asio::use_awaitable)
                    );
                    co_return true;
                },
                [&](std::string errinfo) -> asio::awaitable<bool> {
                    XX_LOGE("[sse] write error: {}", errinfo);
                    co_return false;
                },
                [](std::string& errinfo) -> std::optional<bool> {
                    XX_LOGE("[sse] write error: {}", errinfo);
                    return false;
                }
            );
        }

        asio::awaitable<bool> writeWithHeader(std::string_view data) {
            namespace http = boost::beast::http;
            headerSent_    = true;
            // 只写响应头 (声明 chunked), 不能使用 http::async_write 写完整响应:
            // 它对 chunked 响应会自动追加终止帧 (0\r\n\r\n), 导致首个事件后流即结束,
            // 后续 writeEvent 无法送达客户端
            http::response<http::empty_body> resp;
            resp.version(11);
            resp.result(http::status::ok);
            resp.set(http::field::content_type, "text/event-stream");
            resp.set(http::field::cache_control, "no-cache");
            resp.set(http::field::connection, "keep-alive");
            resp.set("X-Accel-Buffering", "no");
            resp.chunked(true);
            http::response_serializer<http::empty_body> sr{resp};
            co_await http::async_write_header(
                stream_,
                sr,
                asio::cancel_after(timeout_, asio::use_awaitable)
            );
            // 第一个 chunk 帧 (与 doWrite 保持一致)
            std::string frame = fmt::format("{:x}\r\n{}\r\n", data.size(), data);
            co_await asio::async_write(
                stream_,
                asio::buffer(frame),
                asio::cancel_after(timeout_, asio::use_awaitable)
            );
            co_return true;
        }
    };

    // -----------------------------------------------------------------------
    // RAII guard for active connection counting — ensures decrement even
    // if the session coroutine throws or is cancelled.
    // -----------------------------------------------------------------------
    struct ConnectionGuard {
        std::atomic<size_t>& counter;

        explicit ConnectionGuard(std::atomic<size_t>& c) :
            counter(c) {}

        ~ConnectionGuard() {
            counter.fetch_sub(1, std::memory_order_relaxed);
        }

        ConnectionGuard(const ConnectionGuard&)            = delete;
        ConnectionGuard& operator=(const ConnectionGuard&) = delete;
    };

    // -----------------------------------------------------------------------
    // Accept loop
    // -----------------------------------------------------------------------

    asio::awaitable<void> acceptLoop();

    /// Accept loop for startAsync mode — all connections served on the same executor
    asio::awaitable<void> acceptLoopAsync();

    /// Serve a TCP connection (member coroutine — avoids dangling lambda captures)
    asio::awaitable<void> serveTcp(std::shared_ptr<boost::beast::tcp_stream> stream);

    /// Serve an SSL connection (member coroutine)
    asio::awaitable<void>
        serveSsl(std::shared_ptr<boost::beast::ssl_stream<boost::beast::tcp_stream>> stream);

    // -----------------------------------------------------------------------
    // SSL helper
    // -----------------------------------------------------------------------

    asio::awaitable<void> sslHandshakeAndServe(
        std::shared_ptr<boost::beast::ssl_stream<boost::beast::tcp_stream>> stream
    );

    // -----------------------------------------------------------------------
    // Session handler (template – works with tcp_stream & ssl_stream)
    // -----------------------------------------------------------------------

    template<typename Stream>
    asio::awaitable<void> serve(Stream stream) {
        namespace http = boost::beast::http;
        neograph_asio_error_code ec;

        boost::beast::flat_buffer buffer;
        bool                      keepAlive = false;
        bool                      readError = false;
        std::string               readErrorMsg;
        http::status              readErrorStatus = http::status::bad_request;
        size_t                    requestCount    = 0; // 连接级请求计数 (防永久占用)
        // WebSocket 升级后 stream 已被移入 ws stream, 收尾 shutdown 必须跳过
        bool streamMoved = false;

        do {
            // 连接级请求数限制: 防止恶意客户端通过 keep-alive 永久占用连接资源
            if (config_.maxRequestsPerConnection > 0
                && requestCount >= config_.maxRequestsPerConnection) {
                break;
            }
            ++requestCount;

            // Read one request with body/header size limits for DoS protection
            readError = false;
            Request req;
            try {
                http::request_parser<http::string_body> parser;
                parser.header_limit(config_.maxHeaderSize);
                parser.body_limit(config_.maxRequestBody);
                // 先只读头部: 携带 Expect: 100-continue 的客户端 (curl/Go net/http 等)
                // 会等待服务端回复 100 Continue 后才发送 body, 若直接整体读取会阻塞到
                // 客户端超时回退 (甚至永久挂起)
                co_await http::async_read_header(
                    stream,
                    buffer,
                    parser,
                    asio::cancel_after(config_.requestTimeout, asio::use_awaitable)
                );
                auto expectIt = parser.get().find(http::field::expect);
                if (expectIt != parser.get().end()
                    && boost::beast::iequals(expectIt->value(), "100-continue")) {
                    http::response<http::empty_body> continueResp;
                    continueResp.version(parser.get().version());
                    continueResp.result(http::status::continue_);
                    co_await http::async_write(
                        stream,
                        continueResp,
                        asio::cancel_after(std::chrono::seconds{5}, asio::use_awaitable)
                    );
                }
                co_await http::async_read(
                    stream,
                    buffer,
                    parser,
                    asio::cancel_after(config_.requestTimeout, asio::use_awaitable)
                );
                req = parser.release();
            } catch (const neograph_asio_system_error& e) {
                if (e.code() == http::error::end_of_stream || e.code() == asio::error::eof
                    || e.code() == asio::error::operation_aborted) {
                    break;
                }
                readError    = true;
                readErrorMsg = e.what();
                agentxx::util::autoConvertToUtf8(readErrorMsg);
                if (e.code() == http::error::body_limit) {
                    readErrorStatus = http::status::payload_too_large;
                } else if (e.code() == http::error::header_limit) {
                    readErrorStatus = http::status::request_header_fields_too_large;
                } else {
                    readErrorStatus = http::status::bad_request;
                }
            }

            if (readError) {
                Response errResp;
                errResp.version(11);
                errResp.result(readErrorStatus);
                errResp.set(http::field::content_type, "text/plain");
                errResp.body() = fmt::format("Bad Request: {}", readErrorMsg);
                errResp.prepare_payload();
                errResp.keep_alive(false);
                co_await http::async_write(
                    stream,
                    errResp,
                    asio::cancel_after(std::chrono::seconds(5), asio::use_awaitable)
                );
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
            int         methodIdx = httpMethodIndex(req.method());
            std::string matchedPath;
            bool        handled = false;

            // Check for SSE streaming route first (GET only)
            if (methodIdx == 0) { // GET
                auto sseIt = sseRoutes_.find(path);
                if (sseIt != sseRoutes_.end()) {
                    // handler 异常统一经 catchErrorAsync 转为 500 (错误消息含 UTF-8
                    // 转换); CancelledException/NodeInterrupt 保持抛出以终止本连接
                    auto sseOk = co_await agentxx::util::catchErrorAsync<bool>(
                        [&]() -> asio::awaitable<bool> {
                            auto writer = std::make_shared<SseWriterImpl<Stream>>(
                                stream,
                                config_.sseWriteTimeout
                            );
                            co_await sseIt->second(req, writer);
                            co_return true;
                        },
                        [&](std::string errInfo) -> asio::awaitable<bool> {
                            XX_LOGE(
                                "[server] SSE handler error [{} {}]: {}",
                                req.method_string(),
                                req.target(),
                                errInfo
                            );
                            fillError(
                                resp,
                                req.version(),
                                http::status::internal_server_error,
                                "Internal Server Error"
                            );
                            co_return false;
                        }
                    );
                    if (sseOk) {
                        handled = true;
                        // SSE streaming handled, skip normal response write
                        // The connection is kept alive by the SseWriter
                        // But we still need to let serve() know not to continue
                        // We'll set a flag and break out
                        break;
                    }
                }
            }

            // POST SSE streaming route (e.g. MCP subscriptions/listen) —
            // 长连接响应流, 优先于普通 router handler
            if (methodIdx == 2) { // POST
                auto sseIt = ssePostRoutes_.find(path);
                if (sseIt != ssePostRoutes_.end()) {
                    auto sseOk = co_await agentxx::util::catchErrorAsync<bool>(
                        [&]() -> asio::awaitable<bool> {
                            auto writer = std::make_shared<SseWriterImpl<Stream>>(
                                stream,
                                config_.sseWriteTimeout
                            );
                            co_await sseIt->second(req, writer);
                            co_return true;
                        },
                        [&](std::string errInfo) -> asio::awaitable<bool> {
                            XX_LOGE(
                                "[server] POST SSE handler error [{} {}]: {}",
                                req.method_string(),
                                req.target(),
                                errInfo
                            );
                            fillError(
                                resp,
                                req.version(),
                                http::status::internal_server_error,
                                "Internal Server Error"
                            );
                            co_return false;
                        }
                    );
                    if (sseOk) {
                        handled = true;
                        break;
                    }
                }
            }

            // Check for WebSocket upgrade (GET + Upgrade: websocket)
            if (methodIdx == 0 && !handled && (!wsRoutes_.empty() || !wsSslRoutes_.empty())) {
                auto upgradeIt = req.find(http::field::upgrade);
                if (upgradeIt != req.end()
                    && boost::beast::iequals(upgradeIt->value(), "websocket")) {
                    // WS 升级/处理异常统一经 catchErrorAsync 转换;
                    // CancelledException/NodeInterrupt 保持抛出
                    auto wsOk = co_await agentxx::util::catchErrorAsync<bool>(
                        [&]() -> asio::awaitable<bool> {
                            if constexpr (std::is_same_v<Stream, boost::beast::tcp_stream>) {
                                auto wsIt = wsRoutes_.find(path);
                                if (wsIt != wsRoutes_.end()) {
                                    streamMoved = true;
                                    boost::beast::websocket::stream<boost::beast::tcp_stream> ws(
                                        std::move(stream)
                                    );
                                    ws.set_option(
                                        boost::beast::websocket::stream_base::timeout::suggested(
                                            boost::beast::role_type::server
                                        )
                                    );
                                    co_await ws.async_accept(
                                        req,
                                        asio::cancel_after(
                                            std::chrono::seconds{10},
                                            asio::use_awaitable
                                        )
                                    );
                                    co_await wsIt->second(ws);
                                    co_return true;
                                }
                            } else {
                                auto wsIt = wsSslRoutes_.find(path);
                                if (wsIt != wsSslRoutes_.end()) {
                                    streamMoved = true;
                                    boost::beast::websocket::stream<
                                        boost::beast::ssl_stream<boost::beast::tcp_stream>>
                                        ws(std::move(stream));
                                    ws.set_option(
                                        boost::beast::websocket::stream_base::timeout::suggested(
                                            boost::beast::role_type::server
                                        )
                                    );
                                    co_await ws.async_accept(
                                        req,
                                        asio::cancel_after(
                                            std::chrono::seconds{10},
                                            asio::use_awaitable
                                        )
                                    );
                                    co_await wsIt->second(ws);
                                    co_return true;
                                }
                            }
                            co_return false;
                        },
                        [&](std::string errInfo) -> asio::awaitable<bool> {
                            XX_LOGE("[server] WS upgrade error [{}]: {}", req.target(), errInfo);
                            fillError(
                                resp,
                                req.version(),
                                http::status::internal_server_error,
                                "WebSocket Error"
                            );
                            co_return false;
                        }
                    );
                    if (wsOk) {
                        handled = true;
                        break;
                    }
                }
            }

            if (methodIdx >= 0 && !handled) {
                auto handler = router_->get(path, methodIdx, matchedPath);
                // HEAD 兼容: 未注册专用 HEAD handler 时回退到 GET handler
                // (RFC 7231 §4.3.2: HEAD 与 GET 语义相同, 响应 body 在写出时剥离)
                if (!handler && methodIdx == 1) {
                    handler = router_->get(path, 0, matchedPath);
                }
                if (handler && *handler) {
                    // handler 异常统一经 catchErrorAsync 转为 500;
                    // CancelledException/NodeInterrupt 保持抛出
                    handled = co_await agentxx::util::catchErrorAsync<bool>(
                        [&]() -> asio::awaitable<bool> {
                            co_await (*handler)(req, resp, matchedPath);
                            co_return true;
                        },
                        [&](std::string errInfo) -> asio::awaitable<bool> {
                            XX_LOGE(
                                "[server] Handler error [{} {}]: {}",
                                req.method_string(),
                                req.target(),
                                errInfo
                            );
                            fillError(
                                resp,
                                req.version(),
                                http::status::internal_server_error,
                                "Internal Server Error"
                            );
                            co_return false;
                        }
                    );
                }
            }

            if (!handled) {
                std::string dummyPath;
                // RFC 7231 §6.5.5: 405 响应必须携带 Allow 头, 列出该资源实际支持的方法
                std::string allowMethods;
                for (int m = 0; m < 9; ++m) {
                    if (router_->getNocache(path, m, dummyPath)) {
                        if (!allowMethods.empty()) {
                            allowMethods += ", ";
                        }
                        allowMethods += httpMethodName(m);
                    }
                }
                if (!allowMethods.empty()) {
                    fillError(
                        resp,
                        req.version(),
                        http::status::method_not_allowed,
                        "Method Not Allowed"
                    );
                    resp.set(http::field::allow, allowMethods);
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
            bool clientClose = req.find(http::field::connection) != req.end()
                               && boost::beast::iequals(req[http::field::connection], "close");
            keepAlive = resp.keep_alive();
            if (keepAlive && (stopped_ || clientClose)) {
                keepAlive = false;
            }
            resp.set(http::field::connection, keepAlive ? "keep-alive" : "close");

            // Send response
            if (req.method() == http::verb::head) {
                // HEAD 响应只发送头部 (含 Content-Length), 不发送 body (RFC 7231 §4.3.2);
                // 发送 body 会使 keep-alive 连接上的后续请求解析错位
                http::response<http::empty_body> headResp;
                headResp.version(resp.version());
                headResp.result(resp.result());
                for (auto const& f : resp) {
                    headResp.insert(f.name_string(), f.value());
                }
                co_await http::async_write(
                    stream,
                    headResp,
                    asio::cancel_after(config_.requestTimeout, asio::use_awaitable)
                );
            } else {
                co_await http::async_write(
                    stream,
                    resp,
                    asio::cancel_after(config_.requestTimeout, asio::use_awaitable)
                );
            }

            // Per-request access log (compiled out in release via XX_LOGI)
            if (config_.accessLogEnabled) {
                XX_LOGI(
                    "{} {} -> {} ({})",
                    req.method_string(),
                    req.target(),
                    resp.result_int(),
                    resp.body().size()
                );
            }

        } while (keepAlive && !stopped_);

        // Graceful close: shutdown send side, ignore errors
        // (WebSocket 升级后 stream 已被移走, 由 ws stream 负责关闭)
        if (!streamMoved) {
            ec = {};
            boost::beast::get_lowest_layer(stream).socket().shutdown(
                asio::ip::tcp::socket::shutdown_send,
                ec
            );
        }
    }

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    static void fillError(
        Response&                  resp,
        unsigned                   version,
        boost::beast::http::status status,
        std::string_view           message
    );

    // -----------------------------------------------------------------------
    // Per-thread worker: each owns a private io_context — zero-lock isolation
    // -----------------------------------------------------------------------
    struct Worker {
        asio::io_context                                                          ioCtx;
        std::optional<asio::executor_work_guard<asio::io_context::executor_type>> workGuard;
        std::thread                                                               thread;
    };

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    Config                                   config_;
    std::vector<std::unique_ptr<Worker>>     workers_;
    std::unique_ptr<Router>                  router_;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::unique_ptr<asio::ssl::context>      sslCtx_;
    std::atomic<bool>                        stopped_{false};
    std::atomic<size_t>                      activeConnections_{0};
    std::atomic<size_t>                      nextWorker_{0};

    /// SSE streaming routes (GET only) — keyed by path
    std::unordered_map<
        std::string,
        std::function<asio::awaitable<void>(Request&, std::shared_ptr<SseWriter>)>>
        sseRoutes_;

    /// SSE streaming routes (POST only) — 长连接响应流 (如 MCP subscriptions/listen)
    std::unordered_map<
        std::string,
        std::function<asio::awaitable<void>(Request&, std::shared_ptr<SseWriter>)>>
        ssePostRoutes_;

    /// WebSocket routes (GET + Upgrade) — keyed by path
    std::unordered_map<std::string, WsHandler> wsRoutes_;

    /// WebSocket SSL routes (GET + Upgrade over TLS) — keyed by path
    std::unordered_map<std::string, WssHandler> wsSslRoutes_;

    /// Whether startAsync mode is active (single executor, no worker threads)
    bool asyncMode_ = false;
};

} // namespace util
} // namespace agentxx
