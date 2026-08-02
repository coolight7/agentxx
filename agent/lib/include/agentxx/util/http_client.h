#pragma once

#include "agentxx/util/http_header.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/awaitable.hpp"
#include "asio/cancel_after.hpp"
#include "asio/redirect_error.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <cctype>
#include <charconv>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <neograph/api.h>
#include <neograph/json.h>
#include <optional>
#include <string>

namespace agentxx {
namespace util {

struct HttpResponse {
    int         status = 0;
    std::string body;
    HeaderMap   headers;

    std::string_view findHeader(std::string_view name) const noexcept;

    bool isSuccess() const noexcept;

    std::string contentType() const noexcept;

    static bool isJsonContentType(std::string_view contentType) noexcept;

    static bool isTextContentType(std::string_view contentType) noexcept;

    std::optional<neograph::json> bodyJson() const;

    std::optional<std::string> bodyText() const;
};

/// Default max response body size (10 MB) to prevent memory exhaustion
inline constexpr uint64_t kDefaultMaxResponseBody = 10 * 1024 * 1024;

/// Per-request configuration for the less frequently customized options.
/// - connectTimeout: total deadline for DNS resolve + TCP connect + TLS handshake
///   combined. The three phases share a single deadline so the worst-case total
///   is exactly connectTimeout (not 3x as with per-phase limits).
/// - sslVerify: enables TLS certificate verification for this request; nullopt
///   falls back to the global default (see HttpClient::setSslVerify).
/// - sendTimeout: bounds writing the request; nullopt auto-derives it from the
///   request body size (see HttpClient::calcTimeoutBySize).
/// - readChunkTimeout: bounds the gap between successive incoming data chunks; if no
///   new data arrives within readChunkTimeout the request is treated as timed out
///   (the timer resets every time new data is received). This is a per-chunk
///   timeout, NOT a total response timeout — as long as data keeps flowing the
///   connection stays alive.
struct RequestConfig {
    std::chrono::milliseconds                connectTimeout   = std::chrono::seconds{30};
    std::optional<std::chrono::milliseconds> sendTimeout      = std::nullopt;
    std::chrono::milliseconds                readChunkTimeout = std::chrono::seconds{30};
    std::optional<bool>                      sslVerify        = std::nullopt;
    size_t                                   followRedirect   = 3;
    bool                                     keepAlive        = false;
    uint64_t                                 maxResponseBody  = kDefaultMaxResponseBody;
};

class HttpClient {
public:

    using RequestConfig = agentxx::util::RequestConfig;

    static std::pair<std::string, std::string> splitUrl(std::string_view url);

    static std::string urlEncode(std::string_view s);

    static bool respIsSucc(const HttpResponse& resp);

    static bool isValidUrl(std::string_view url) noexcept;

public:

    static bool isRedirectStatus(int status) noexcept;

    static bool redirectChangesToGet(int status) noexcept;

    static std::string
        resolveRedirectUrl(std::string_view originalUrl, std::string_view location) noexcept;

    static const HeaderMap& defaultHeaders();

    // -----------------------------------------------------------------------
    // Shared SSL context pool — avoids per-request OpenSSL context creation
    // (SSL_CTX_new is expensive). Two lazily-initialized contexts: one with
    // certificate verification enabled (production default), one without.
    // -----------------------------------------------------------------------
    static asio::ssl::context& sharedSslCtx(bool verify);

private:

    struct ParsedUrl {
        std::string scheme;
        std::string host;
        uint16_t    port;
        std::string path;
    };

    static inline std::atomic<bool> sslVerifyEnabled_{false};

public:

    static std::optional<ParsedUrl> parseUrl(std::string_view url);

    static std::chrono::seconds calcTimeoutBySize(size_t bodyBytes);

    template<typename Stream>
    static asio::awaitable<std::expected<HttpResponse, std::string>> exchange(
        Stream&                                                       stream,
        boost::beast::http::request<boost::beast::http::string_body>& req,
        const RequestConfig&                                          config
    ) {
        namespace http = boost::beast::http;

        auto sendTimeout = config.sendTimeout.value_or(calcTimeoutBySize(req.body().size()));
        co_await http::async_write(
            stream,
            req,
            asio::cancel_after(sendTimeout, asio::use_awaitable)
        );

        boost::beast::flat_buffer                buffer;
        http::response_parser<http::string_body> parser;
        parser.body_limit(config.maxResponseBody);
        while (!parser.is_done()) {
            co_await http::async_read_some(
                stream,
                buffer,
                parser,
                asio::cancel_after(config.readChunkTimeout, asio::use_awaitable)
            );
        }
        // 连接关闭但响应未解析完整 → 视为传输错误, 避免返回截断的 body
        if (!parser.is_done()) {
            co_return std::unexpected{std::string{"connection closed before response complete"}};
        }

        auto         res = parser.release();
        HttpResponse resp;
        resp.status = res.result_int();
        for (auto const& field : res) {
            resp.headers.set(field.name_string(), field.value());
        }
        resp.body = std::move(res.body());
        co_return std::expected<HttpResponse, std::string>{std::move(resp)};
    }

    static asio::awaitable<std::expected<HttpResponse, std::string>> requestAsync(
        std::string_view     method,
        std::string_view     url,
        std::string_view     body,
        std::string_view     contentType,
        const HeaderMap&     extraHeaders,
        const RequestConfig& config = {}
    );

    /// SSE 流式请求: 连接、发送、读取响应头后逐块回调 body 数据。
    /// - 每次收到新数据块时调用 onChunk (分块间隔受 readChunkTimeout 约束)
    /// - HTTP 429 时抛出 neograph::RateLimitError (解析 retry-after)
    /// - 其他非 2xx 时抛出 std::runtime_error
    /// - 网络/超时错误抛出 boost::system::system_error
    static asio::awaitable<void> requestSseAsync(
        std::string_view                      method,
        std::string_view                      url,
        std::string_view                      body,
        std::string_view                      contentType,
        const HeaderMap&                      extraHeaders,
        const RequestConfig&                  config,
        std::function<void(std::string_view)> onChunk
    );

    /// Enable/disable SSL certificate verification (default: enabled).
    /// Disable only for testing with self-signed certificates.
    static void setSslVerify(bool enable) noexcept;

    static bool getSslVerify() noexcept;

    static asio::awaitable<std::expected<HttpResponse, std::string>> getAsync(
        std::string_view     url,
        const HeaderMap&     extraHeaders = {},
        const RequestConfig& config       = {}
    );

    static asio::awaitable<std::expected<HttpResponse, std::string>> headAsync(
        std::string_view     url,
        const HeaderMap&     extraHeaders = {},
        const RequestConfig& config       = {}
    );

    static asio::awaitable<std::expected<HttpResponse, std::string>> postAsync(
        std::string_view      url,
        const neograph::json& body,
        const HeaderMap&      extraHeaders = {},
        const RequestConfig&  config       = {}
    );

    static asio::awaitable<std::expected<HttpResponse, std::string>> postAsync(
        std::string_view     url,
        std::string_view     body,
        std::string_view     contentType  = "text/plain",
        const HeaderMap&     extraHeaders = {},
        const RequestConfig& config       = {}
    );

    static asio::awaitable<std::expected<HttpResponse, std::string>> putAsync(
        std::string_view     url,
        std::string_view     body,
        std::string_view     contentType  = "text/plain",
        const HeaderMap&     extraHeaders = {},
        const RequestConfig& config       = {}
    );

    static asio::awaitable<std::expected<HttpResponse, std::string>> patchAsync(
        std::string_view     url,
        std::string_view     body,
        std::string_view     contentType  = "text/plain",
        const HeaderMap&     extraHeaders = {},
        const RequestConfig& config       = {}
    );

    static asio::awaitable<std::expected<HttpResponse, std::string>> deleteAsync(
        std::string_view     url,
        const HeaderMap&     extraHeaders = {},
        const RequestConfig& config       = {}
    );

    static asio::awaitable<std::expected<HttpResponse, std::string>> optionsAsync(
        std::string_view     url,
        const HeaderMap&     extraHeaders = {},
        const RequestConfig& config       = {}
    );

    static asio::awaitable<std::expected<std::string, std::string>> fetchMarkdown(
        std::string_view     url,
        const RequestConfig& config = RequestConfig{
            .connectTimeout = std::chrono::seconds{15}, 
            .readChunkTimeout = std::chrono::seconds{15},
        }
    );
};
} // namespace util
} // namespace agentxx