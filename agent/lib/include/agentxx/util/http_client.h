#pragma once

#include "agentxx/util/log.h"
#include "hical/core/Coroutine.h"
#include "hical/core/HeaderMap.h"
#include "hical/core/SslContext.h"
#include "html2md/html2md.h"
#include <algorithm>
#include <asio/cancel_after.hpp>
#include <asio/this_coro.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <cctype>
#include <charconv>
#include <expected>
#include <neograph/json.h>
#include <openssl/ssl.h>
#include <optional>
#include <sstream>

namespace agentxx {
namespace util {

struct HttpResponse {
  int status = 0;
  std::string body;
  hical::HeaderMap headers;

  std::string_view findHeader(std::string_view name) const noexcept {
    return headers.find(name);
  }

  bool isSuccess() const noexcept { return status / 100 == 2; }

  std::string contentType() const noexcept {
    auto ct = headers.find("content-type");
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
    std::ostringstream out;
    out << std::hex;
    for (unsigned char c : s) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
          c == '~') {
        out << c;
      } else if (c == ' ') {
        out << '+';
      } else {
        out << '%';
        if (c < 16)
          out << '0';
        out << (int)c;
        out << std::dec << std::hex;
      }
    }
    return out.str();
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
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
  }

  static inline bool redirectChangesToGet(int status) noexcept {
    return status == 301 || status == 302 || status == 303;
  }

  static inline std::string
  resolveRedirectUrl(std::string_view originalUrl,
                     std::string_view location) noexcept {
    if (location.find("://") != std::string_view::npos)
      return std::string(location);
    auto [base, path] = splitUrl(originalUrl);
    if (location.starts_with('/'))
      return base + std::string(location);
    auto slashPos = path.rfind('/');
    std::string basePath =
        (slashPos != std::string_view::npos && slashPos > 0)
            ? std::string(path.substr(0, slashPos + 1))
            : "/";
    return base + basePath + std::string(location);
  }

  static inline hical::HeaderMap defaultHeaders() {
    hical::HeaderMap headers;
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
        int p = 0;
        std::from_chars(hostPort.data() + cb + 2,
                        hostPort.data() + hostPort.size(), p);
        port = static_cast<uint16_t>(p);
      }
    } else {
      auto colon = hostPort.rfind(':');
      if (colon != std::string_view::npos) {
        host = std::string{hostPort.substr(0, colon)};
        int p = 0;
        std::from_chars(hostPort.data() + colon + 1,
                        hostPort.data() + hostPort.size(), p);
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
  static hical::Awaitable<std::expected<HttpResponse, std::string>>
  exchange(Stream &stream,
           boost::beast::http::request<boost::beast::http::string_body> &req,
           std::chrono::steady_clock::time_point deadline) {
    namespace http = boost::beast::http;
    auto rem = [&] {
      auto d = deadline - std::chrono::steady_clock::now();
      return std::chrono::duration_cast<std::chrono::milliseconds>(d);
    };

    co_await http::async_write(
        stream, req,
        boost::asio::cancel_after(rem(), boost::asio::use_awaitable));

    boost::beast::flat_buffer buffer;
    http::response<http::string_body> res;
    co_await http::async_read(
        stream, buffer, res,
        boost::asio::cancel_after(rem(), boost::asio::use_awaitable));

    HttpResponse resp;
    resp.status = res.result_int();
    for (auto const &field : res) {
      resp.headers.set(field.name_string(), field.value());
    }
    resp.body = std::move(res.body());
    co_return std::expected<HttpResponse, std::string>{std::move(resp)};
  }

  static inline hical::Awaitable<std::expected<HttpResponse, std::string>>
  requestAsync(std::string_view method, const std::string &url,
               std::string_view body, std::string_view contentType,
               const hical::HeaderMap &extraHeaders,
               std::chrono::milliseconds timeout,
               size_t followRedirect = 0) {
    namespace http = boost::beast::http;
    using boost::asio::ip::tcp;

    std::string currentUrl = url;
    std::string currentMethod(method);
    std::string currentBody(body);
    std::string currentContentType(contentType);

    std::expected<HttpResponse, std::string> result;
    for (size_t redirectCount = 0;; ++redirectCount) {
      auto executor = co_await boost::asio::this_coro::executor;
      auto deadline = std::chrono::steady_clock::now() + timeout;
      auto rem = [&] {
        auto d = deadline - std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(d);
      };

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
        if (!extraHeaders.contains("host"))
          req.set(http::field::host, hostHeader);
        req.set(http::field::user_agent,
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/119.0.6045.160 Safari/537.36");
        req.set(http::field::accept, "*/*");
        req.set(http::field::accept_encoding, "identity");
        req.set(http::field::connection, "close");
        for (const auto &[k, v] : extraHeaders) {
          if (k == "host" || k == "Host")
            continue;
          req.set(k, v);
        }
        if (!currentBody.empty()) {
          if (!currentContentType.empty())
            req.set(http::field::content_type, currentContentType);
          req.body() = currentBody;
          req.prepare_payload();
        }

        tcp::resolver resolver(executor);
        auto endpoints = co_await resolver.async_resolve(
            parsed->host, std::to_string(parsed->port),
            boost::asio::cancel_after(rem(), boost::asio::use_awaitable));

        if (isHttps) {
          boost::asio::ssl::context sslCtx(
              boost::asio::ssl::context::tlsv12_client);
          sslCtx.set_verify_mode(boost::asio::ssl::verify_none);
          boost::asio::ssl::stream<tcp::socket> stream(executor, sslCtx);
          if (!parsed->host.empty())
            ::SSL_set_tlsext_host_name(stream.native_handle(),
                                       parsed->host.c_str());
          co_await boost::asio::async_connect(
              stream.lowest_layer(), endpoints,
              boost::asio::cancel_after(rem(), boost::asio::use_awaitable));
          co_await stream.async_handshake(
              boost::asio::ssl::stream_base::client,
              boost::asio::cancel_after(rem(), boost::asio::use_awaitable));
          result = co_await exchange(stream, req, deadline);
        } else {
          tcp::socket stream(executor);
          co_await boost::asio::async_connect(
              stream, endpoints,
              boost::asio::cancel_after(rem(), boost::asio::use_awaitable));
          result = co_await exchange(stream, req, deadline);
        }
      } catch (const boost::system::system_error &e) {
        if (e.code() == boost::asio::error::operation_aborted)
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
  static inline hical::Awaitable<std::expected<HttpResponse, std::string>>
  getAsync(const std::string &url, const hical::HeaderMap &extraHeaders = {},
           std::chrono::milliseconds timeout = std::chrono::seconds{60},
           size_t followRedirect = 0) {
    co_return co_await requestAsync("GET", url, {}, "", extraHeaders, timeout,
                                    followRedirect);
  }

  static inline hical::Awaitable<std::expected<HttpResponse, std::string>>
  postAsync(const std::string &url, const neograph::json &body,
            const hical::HeaderMap &extraHeaders = {},
            std::chrono::milliseconds timeout = std::chrono::seconds{60},
            size_t followRedirect = 0) {
    co_return co_await requestAsync("POST", url, body.dump(),
                                    "application/json", extraHeaders, timeout,
                                    followRedirect);
  }

  static inline hical::Awaitable<std::expected<HttpResponse, std::string>>
  postAsync(const std::string &url, std::string_view body,
            std::string_view contentType = "text/plain",
            const hical::HeaderMap &extraHeaders = {},
            std::chrono::milliseconds timeout = std::chrono::seconds{60},
            size_t followRedirect = 0) {
    co_return co_await requestAsync("POST", url, body, contentType,
                                    extraHeaders, timeout, followRedirect);
  }

  static inline hical::Awaitable<std::expected<HttpResponse, std::string>>
  putAsync(const std::string &url, std::string_view body,
           std::string_view contentType = "text/plain",
           const hical::HeaderMap &extraHeaders = {},
           std::chrono::milliseconds timeout = std::chrono::seconds{60},
           size_t followRedirect = 0) {
    co_return co_await requestAsync("PUT", url, body, contentType, extraHeaders,
                                    timeout, followRedirect);
  }

  static inline hical::Awaitable<std::expected<std::string, std::string>>
  fetchMarkdown(const std::string &url,
                std::chrono::milliseconds timeout = std::chrono::seconds{15},
                size_t followRedirect = 0) {
    auto resp = co_await getAsync(url, {}, timeout, followRedirect);
    if (resp.has_value()) {
      if (respIsSucc(resp.value())) {
        auto options = html2md::Options{
            .splitLines = false,
        };
        auto convert = html2md::Converter{resp.value().body, &options};
        co_return std::expected<std::string, std::string>{convert.convert()};
      } else {
        XX_LOGE("fetchMarkdown resp failed: StatusCode {}",
                resp.value().status);
      }
    }
    XX_LOGE("fetchMarkdown error: {}", resp.error());
    co_return std::unexpected{resp.error()};
  }
};
} // namespace util
} // namespace agentxx