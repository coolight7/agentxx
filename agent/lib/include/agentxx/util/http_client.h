#pragma once

#include "agentxx/util/http_header.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/awaitable.hpp"
#include "asio/cancel_after.hpp"
#include "asio/this_coro.hpp"
#include "html2md/html2md.h"
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
#include <memory>
#include <mutex>
#include <neograph/json.h>
#include <openssl/ssl.h>
#include <optional>
#include <string>

namespace agentxx {
namespace util {

struct HttpResponse {
  int status = 0;
  std::string body;
  HeaderMap headers;

  std::string_view findHeader(std::string_view name) const noexcept {
    return headers.getSingle(name);
  }

  bool isSuccess() const noexcept { return status / 100 == 2; }

  std::string contentType() const noexcept {
    auto ct = headers.getSingle("content-type");
    if (ct.empty())
      return {};
    auto semi = ct.find(';');
    if (semi != std::string_view::npos) {
      ct = ct.substr(0, semi);
    }
    std::string result(ct);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    size_t start = 0;
    while (start < result.size() && result[start] == ' ') {
      ++start;
    }
    size_t end = result.size();
    while (end > start && result[end - 1] == ' ') {
      --end;
    }
    return result.substr(start, end - start);
  }

  static bool isJsonContentType(std::string_view contentType) noexcept {
    return contentType == "application/json" || contentType.ends_with("+json");
  }

  static bool isTextContentType(std::string_view contentType) noexcept {
    if (contentType.empty())
      return true;
    if (contentType.starts_with("text/"))
      return true;
    if (isJsonContentType(contentType))
      return true;
    if (contentType == "application/xml" || contentType.ends_with("+xml"))
      return true;
    if (contentType == "application/x-www-form-urlencoded")
      return true;
    return false;
  }

  std::optional<neograph::json> bodyJson() const {
    auto ct = contentType();
    if (!isJsonContentType(ct))
      return std::nullopt;
    if (body.empty())
      return std::nullopt;
    try {
      return neograph::json::parse(body);
    } catch (const neograph::json::parse_error &) {
      return std::nullopt;
    }
  }

  std::optional<std::string> bodyText() const {
    auto ct = contentType();
    if (!isTextContentType(ct))
      return std::nullopt;
    return body;
  }
};

class HttpClient {
public:
  static inline std::pair<std::string, std::string>
  splitUrl(std::string_view url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
      return std::pair<std::string, std::string>{url, "/"};
    }
    auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) {
      return std::pair<std::string, std::string>{url, "/"};
    }
    return std::pair<std::string, std::string>{url.substr(0, path_start),
                                               url.substr(path_start)};
  }

  static inline std::string urlEncode(std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
          c == '~') {
        out.push_back(static_cast<char>(c));
      } else if (c == ' ') {
        out.push_back('+');
      } else {
        out.push_back('%');
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0xF]);
      }
    }
    return out;
  }

  static inline bool respIsSucc(const HttpResponse &resp) {
    return resp.isSuccess();
  }

  static inline bool isValidUrl(std::string_view url) noexcept {
    if (url.empty())
      return false;
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
      return !url.empty();
    }
    auto scheme = url.substr(0, scheme_end);
    if (scheme != "http" && scheme != "https")
      return false;
    auto rest = url.substr(scheme_end + 3);
    return !rest.empty();
  }

public:
  static inline bool isRedirectStatus(int status) noexcept {
    return status == 301 || status == 302 || status == 303 || status == 307 ||
           status == 308;
  }

  static inline bool redirectChangesToGet(int status) noexcept {
    return status == 301 || status == 302 || status == 303;
  }

  static inline std::string
  resolveRedirectUrl(std::string_view originalUrl,
                     std::string_view location) noexcept {
    if (location.find("://") != std::string_view::npos)
      return std::string(location);
    // Protocol-relative URL: //host/path -> scheme://host/path
    if (location.starts_with("//")) {
      auto schemeEnd = originalUrl.find("://");
      if (schemeEnd != std::string_view::npos)
        return fmt::format("{}:{}", originalUrl.substr(0, schemeEnd), location);
      return std::string(location);
    }
    auto [base, path] = splitUrl(originalUrl);
    if (location.starts_with('/'))
      return base + std::string(location);
    auto slashPos = path.rfind('/');
    std::string basePath = (slashPos != std::string_view::npos && slashPos > 0)
                               ? std::string(path.substr(0, slashPos + 1))
                               : "/";
    return base + basePath + std::string(location);
  }

  static inline HeaderMap defaultHeaders() {
    HeaderMap headers;
    headers.set(
        "User-Agent",
        R"(Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/119.0.6045.160 Safari/537.36)");
    headers.set("Accept", "*/*");
    headers.set("Accept-Language", "zh-CN,zh;q=0.9");
    return headers;
  }

private:
  struct ParsedUrl {
    std::string scheme;
    std::string host;
    uint16_t port;
    std::string path;
  };

  // -----------------------------------------------------------------------
  // Shared SSL context pool — avoids per-request OpenSSL context creation
  // (SSL_CTX_new is expensive). Two lazily-initialized contexts: one with
  // certificate verification enabled (production default), one without.
  // -----------------------------------------------------------------------
  static asio::ssl::context &sharedSslCtx(bool verify) {
    static std::unique_ptr<asio::ssl::context> verifiedCtx;
    static std::unique_ptr<asio::ssl::context> unverifiedCtx;
    static std::once_flag verifiedFlag;
    static std::once_flag unverifiedFlag;

    if (verify && false) {
      std::call_once(verifiedFlag, [] {
        auto ctx = std::make_unique<asio::ssl::context>(
            asio::ssl::context::tlsv12_client);
        ctx->set_verify_mode(asio::ssl::verify_peer);
        ctx->set_default_verify_paths();
        verifiedCtx = std::move(ctx);
      });
      return *verifiedCtx;
    } else {
      std::call_once(unverifiedFlag, [] {
        auto ctx = std::make_unique<asio::ssl::context>(
            asio::ssl::context::tlsv12_client);
        ctx->set_verify_mode(asio::ssl::verify_none);
        unverifiedCtx = std::move(ctx);
      });
      return *unverifiedCtx;
    }
  }

  static inline std::atomic<bool> sslVerifyEnabled_{true};

  /// Default max response body size (10 MB) to prevent memory exhaustion
  static constexpr uint64_t kDefaultMaxResponseBody = 10 * 1024 * 1024;

  static inline std::optional<ParsedUrl> parseUrl(std::string_view url) {
    auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos)
      return std::nullopt;
    std::string scheme{url.substr(0, schemeEnd)};
    if (scheme != "http" && scheme != "https")
      return std::nullopt;
    auto rest = url.substr(schemeEnd + 3);
    if (rest.empty())
      return std::nullopt;
    auto pathStart = rest.find('/');
    std::string_view hostPort =
        (pathStart == std::string::npos) ? rest : rest.substr(0, pathStart);
    std::string path = (pathStart == std::string::npos)
                           ? "/"
                           : std::string{rest.substr(pathStart)};
    uint16_t port = (scheme == "https") ? 443 : 80;
    std::string host;
    if (hostPort.starts_with('[')) {
      auto cb = hostPort.find(']');
      if (cb == std::string::npos)
        return std::nullopt;
      host = std::string{hostPort.substr(0, cb + 1)};
      if (cb + 1 < hostPort.size() && hostPort[cb + 1] == ':') {
        auto portStart = hostPort.data() + cb + 2;
        auto portEnd = hostPort.data() + hostPort.size();
        int p = 0;
        auto [ptr, ec] = std::from_chars(portStart, portEnd, p);
        if (ec != std::errc{} || ptr != portEnd || p <= 0 || p > 65535)
          return std::nullopt;
        port = static_cast<uint16_t>(p);
      }
    } else {
      auto colon = hostPort.rfind(':');
      if (colon != std::string_view::npos) {
        host = std::string{hostPort.substr(0, colon)};
        auto portStart = hostPort.data() + colon + 1;
        auto portEnd = hostPort.data() + hostPort.size();
        int p = 0;
        auto [ptr, ec] = std::from_chars(portStart, portEnd, p);
        if (ec != std::errc{} || ptr != portEnd || p <= 0 || p > 65535)
          return std::nullopt;
        port = static_cast<uint16_t>(p);
      } else {
        host = std::string{hostPort};
      }
    }
    if (host.empty())
      return std::nullopt;
    return ParsedUrl{std::move(scheme), std::move(host), port, std::move(path)};
  }

  template <typename Stream>
  static asio::awaitable<std::expected<HttpResponse, std::string>>
  exchange(Stream &stream,
           boost::beast::http::request<boost::beast::http::string_body> &req,
           std::chrono::steady_clock::time_point deadline,
           uint64_t maxResponseBody) {
    namespace http = boost::beast::http;
    auto rem = [&] {
      auto d = deadline - std::chrono::steady_clock::now();
      return std::chrono::duration_cast<std::chrono::milliseconds>(
          std::max(d, std::chrono::steady_clock::duration::zero()));
    };

    co_await http::async_write(stream, req,
                               asio::cancel_after(rem(), asio::use_awaitable));

    boost::beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(maxResponseBody);
    co_await http::async_read(stream, buffer, parser,
                              asio::cancel_after(rem(), asio::use_awaitable));

    auto res = parser.release();
    HttpResponse resp;
    resp.status = res.result_int();
    for (auto const &field : res) {
      resp.headers.set(field.name_string(), field.value());
    }
    resp.body = std::move(res.body());
    co_return std::expected<HttpResponse, std::string>{std::move(resp)};
  }

  static inline asio::awaitable<std::expected<HttpResponse, std::string>>
  requestAsync(std::string_view method, const std::string &url,
               std::string_view body, std::string_view contentType,
               const HeaderMap &extraHeaders, std::chrono::milliseconds timeout,
               size_t followRedirect = 3,
               uint64_t maxResponseBody = kDefaultMaxResponseBody) {
    namespace http = boost::beast::http;
    using asio::ip::tcp;

    std::string currentUrl = url;
    std::string currentMethod(method);
    std::string currentBody(body);
    std::string currentContentType(contentType);

    // Total deadline across all redirect hops — prevents a redirect chain
    // from exceeding the caller's timeout budget.
    auto totalDeadline = std::chrono::steady_clock::now() + timeout;
    auto rem = [&] {
      auto d = totalDeadline - std::chrono::steady_clock::now();
      return std::chrono::duration_cast<std::chrono::milliseconds>(
          std::max(d, std::chrono::steady_clock::duration::zero()));
    };

    std::expected<HttpResponse, std::string> result;
    for (size_t redirectCount = 0;; ++redirectCount) {
      auto executor = co_await asio::this_coro::executor;

      // Check total timeout before each hop
      if (rem().count() <= 0) {
        result = std::unexpected{std::string{"timeout"}};
        break;
      }

      result = std::unexpected{std::string{"unknown error"}};
      try {
        auto parsed = parseUrl(currentUrl);
        if (!parsed)
          throw std::runtime_error{"invalid url: " + currentUrl};
        bool isHttps = parsed->scheme == "https";
        bool defaultPort = (isHttps && parsed->port == 443) ||
                           (!isHttps && parsed->port == 80);
        std::string hostHeader = parsed->host;
        if (!defaultPort)
          hostHeader += ":" + std::to_string(parsed->port);

        http::request<http::string_body> req{
            http::string_to_verb(currentMethod), parsed->path, 11};
        bool hasHost = extraHeaders.contains("host");
        if (!hasHost) {
          req.set(http::field::host, hostHeader);
        }
        req.set(http::field::user_agent,
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/119.0.6045.160 Safari/537.36");
        req.set(http::field::accept, "*/*");
        req.set(http::field::accept_encoding, "identity");
        req.set(http::field::connection, "close");

        for (const auto &[k, v] : extraHeaders.data) {
          // Skip host — set explicitly above (case-insensitive)
          if (isIgnoreCaseEqual(k, "host")) {
            continue;
          }
          req.set(k, stringVectorJoin(v, "; "));
        }
        if (!currentBody.empty()) {
          if (!currentContentType.empty()) {
            req.set(http::field::content_type, currentContentType);
          }
          req.body() = currentBody;
          req.prepare_payload();
        }

        tcp::resolver resolver(executor);
        auto endpoints = co_await resolver.async_resolve(
            parsed->host, std::to_string(parsed->port),
            asio::cancel_after(rem(), asio::use_awaitable));

        if (isHttps) {
          bool verify = sslVerifyEnabled_.load(std::memory_order_relaxed);
          auto &sslCtx = sharedSslCtx(verify);
          asio::ssl::stream<tcp::socket> stream(executor, sslCtx);
          if (!parsed->host.empty())
            ::SSL_set_tlsext_host_name(stream.native_handle(),
                                       parsed->host.c_str());
          co_await asio::async_connect(
              stream.lowest_layer(), endpoints,
              asio::cancel_after(rem(), asio::use_awaitable));
          // Set TCP no_delay before handshake for lower latency
          boost::system::error_code tcpEc;
          stream.lowest_layer().set_option(asio::ip::tcp::no_delay(true),
                                           tcpEc);
          co_await stream.async_handshake(
              asio::ssl::stream_base::client,
              asio::cancel_after(rem(), asio::use_awaitable));
          result =
              co_await exchange(stream, req, totalDeadline, maxResponseBody);
          // Graceful SSL shutdown (ignore errors — peer may have closed)
          boost::system::error_code sslEc;
          co_await stream.async_shutdown(
              asio::redirect_error(asio::use_awaitable, sslEc));
        } else {
          tcp::socket stream(executor);
          co_await asio::async_connect(
              stream, endpoints,
              asio::cancel_after(rem(), asio::use_awaitable));
          // Set TCP no_delay
          boost::system::error_code tcpEc;
          stream.set_option(asio::ip::tcp::no_delay(true), tcpEc);
          result =
              co_await exchange(stream, req, totalDeadline, maxResponseBody);
        }
      } catch (const boost::system::system_error &e) {
        if (e.code() == asio::error::operation_aborted)
          result = std::unexpected{std::string{"timeout"}};
        else
          result = std::unexpected{std::string{e.what()}};
      } catch (const std::exception &e) {
        result = std::unexpected{std::string{e.what()}};
      }

      // Check if we should follow a redirect
      if (!result.has_value())
        break;
      if (redirectCount >= followRedirect)
        break;
      auto &resp = result.value();
      if (!isRedirectStatus(resp.status))
        break;
      auto location = resp.findHeader("location");
      if (location.empty())
        break;

      currentUrl = resolveRedirectUrl(currentUrl, location);
      if (redirectChangesToGet(resp.status)) {
        currentMethod = "GET";
        currentBody = {};
        currentContentType = {};
      }
    }
    co_return result;
  }

public:
  /// Enable/disable SSL certificate verification (default: enabled).
  /// Disable only for testing with self-signed certificates.
  static void setSslVerify(bool enable) noexcept {
    sslVerifyEnabled_.store(enable, std::memory_order_relaxed);
  }

  static bool getSslVerify() noexcept {
    return sslVerifyEnabled_.load(std::memory_order_relaxed);
  }

  static inline asio::awaitable<std::expected<HttpResponse, std::string>>
  getAsync(const std::string &url, const HeaderMap &extraHeaders = {},
           std::chrono::milliseconds timeout = std::chrono::seconds{60},
           size_t followRedirect = 3,
           uint64_t maxResponseBody = kDefaultMaxResponseBody) {
    co_return co_await requestAsync("GET", url, {}, "", extraHeaders, timeout,
                                    followRedirect, maxResponseBody);
  }

  static inline asio::awaitable<std::expected<HttpResponse, std::string>>
  headAsync(const std::string &url, const HeaderMap &extraHeaders = {},
            std::chrono::milliseconds timeout = std::chrono::seconds{60},
            size_t followRedirect = 3,
            uint64_t maxResponseBody = kDefaultMaxResponseBody) {
    co_return co_await requestAsync("HEAD", url, {}, "", extraHeaders, timeout,
                                    followRedirect, maxResponseBody);
  }

  static inline asio::awaitable<std::expected<HttpResponse, std::string>>
  postAsync(const std::string &url, const neograph::json &body,
            const HeaderMap &extraHeaders = {},
            std::chrono::milliseconds timeout = std::chrono::seconds{60},
            size_t followRedirect = 3,
            uint64_t maxResponseBody = kDefaultMaxResponseBody) {
    co_return co_await requestAsync("POST", url, body.dump(),
                                    "application/json", extraHeaders, timeout,
                                    followRedirect, maxResponseBody);
  }

  static inline asio::awaitable<std::expected<HttpResponse, std::string>>
  postAsync(const std::string &url, std::string_view body,
            std::string_view contentType = "text/plain",
            const HeaderMap &extraHeaders = {},
            std::chrono::milliseconds timeout = std::chrono::seconds{60},
            size_t followRedirect = 3,
            uint64_t maxResponseBody = kDefaultMaxResponseBody) {
    co_return co_await requestAsync("POST", url, body, contentType,
                                    extraHeaders, timeout, followRedirect,
                                    maxResponseBody);
  }

  static inline asio::awaitable<std::expected<HttpResponse, std::string>>
  putAsync(const std::string &url, std::string_view body,
           std::string_view contentType = "text/plain",
           const HeaderMap &extraHeaders = {},
           std::chrono::milliseconds timeout = std::chrono::seconds{60},
           size_t followRedirect = 3,
           uint64_t maxResponseBody = kDefaultMaxResponseBody) {
    co_return co_await requestAsync("PUT", url, body, contentType, extraHeaders,
                                    timeout, followRedirect, maxResponseBody);
  }

  static inline asio::awaitable<std::expected<HttpResponse, std::string>>
  patchAsync(const std::string &url, std::string_view body,
             std::string_view contentType = "text/plain",
             const HeaderMap &extraHeaders = {},
             std::chrono::milliseconds timeout = std::chrono::seconds{60},
             size_t followRedirect = 3,
             uint64_t maxResponseBody = kDefaultMaxResponseBody) {
    co_return co_await requestAsync("PATCH", url, body, contentType,
                                    extraHeaders, timeout, followRedirect,
                                    maxResponseBody);
  }

  static inline asio::awaitable<std::expected<HttpResponse, std::string>>
  deleteAsync(const std::string &url, const HeaderMap &extraHeaders = {},
              std::chrono::milliseconds timeout = std::chrono::seconds{60},
              size_t followRedirect = 3,
              uint64_t maxResponseBody = kDefaultMaxResponseBody) {
    co_return co_await requestAsync("DELETE", url, {}, "", extraHeaders,
                                    timeout, followRedirect, maxResponseBody);
  }

  static inline asio::awaitable<std::expected<HttpResponse, std::string>>
  optionsAsync(const std::string &url, const HeaderMap &extraHeaders = {},
               std::chrono::milliseconds timeout = std::chrono::seconds{60},
               size_t followRedirect = 3,
               uint64_t maxResponseBody = kDefaultMaxResponseBody) {
    co_return co_await requestAsync("OPTIONS", url, {}, "", extraHeaders,
                                    timeout, followRedirect, maxResponseBody);
  }

  static inline asio::awaitable<std::expected<std::string, std::string>>
  fetchMarkdown(const std::string &url,
                std::chrono::milliseconds timeout = std::chrono::seconds{15},
                size_t followRedirect = 3) {
    auto resp = co_await getAsync(url, {}, timeout, followRedirect);
    if (!resp.has_value()) {
      XX_LOGE("fetchMarkdown error: {}", resp.error());
      co_return std::unexpected{resp.error()};
    }
    auto &respVal = resp.value();
    if (!respIsSucc(respVal)) {
      XX_LOGE("fetchMarkdown resp failed: StatusCode {}", respVal.status);
      co_return std::unexpected{std::to_string(respVal.status)};
    }
    auto options = html2md::Options{
        .splitLines = false,
    };
    auto convert = html2md::Converter{respVal.body, &options};
    co_return std::expected<std::string, std::string>{convert.convert()};
  }
};
} // namespace util
} // namespace agentxx