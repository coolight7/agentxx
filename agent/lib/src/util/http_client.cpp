#include "agentxx/util/http_client.h"

#include "html2md/html2md.h"
#include <openssl/ssl.h>

namespace agentxx {
namespace util {

std::string_view HttpResponse::findHeader(std::string_view name) const noexcept {
    return headers.getSingle(name);
}

bool HttpResponse::isSuccess() const noexcept {
    return status / 100 == 2;
}

std::string HttpResponse::contentType() const noexcept {
    auto ct = headers.getSingle("content-type");
    if (ct.empty()) {
        return {};
    }
    auto semi = ct.find(';');
    if (semi != std::string_view::npos) {
        ct = ct.substr(0, semi);
    }
    std::string result(ct);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
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

bool HttpResponse::isJsonContentType(std::string_view contentType) noexcept {
    return contentType == "application/json" || contentType.ends_with("+json");
}

bool HttpResponse::isTextContentType(std::string_view contentType) noexcept {
    if (contentType.empty()) {
        return true;
    }
    if (contentType.starts_with("text/")) {
        return true;
    }
    if (isJsonContentType(contentType)) {
        return true;
    }
    if (contentType == "application/xml" || contentType.ends_with("+xml")) {
        return true;
    }
    if (contentType == "application/x-www-form-urlencoded") {
        return true;
    }
    return false;
}

std::optional<neograph::json> HttpResponse::bodyJson() const {
    auto ct = contentType();
    if (!isJsonContentType(ct)) {
        return std::nullopt;
    }
    if (body.empty()) {
        return std::nullopt;
    }
    try {
        return neograph::json::parse(body);
    } catch (const neograph::json::parse_error&) {
        return std::nullopt;
    }
}

std::optional<std::string> HttpResponse::bodyText() const {
    auto ct = contentType();
    if (!isTextContentType(ct)) {
        return std::nullopt;
    }
    return body;
}

std::pair<std::string, std::string> HttpClient::splitUrl(std::string_view url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return std::pair<std::string, std::string>{url, "/"};
    }
    auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) {
        return std::pair<std::string, std::string>{url, "/"};
    }
    return std::pair<std::string, std::string>{url.substr(0, path_start), url.substr(path_start)};
}

std::string HttpClient::urlEncode(std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string           out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
            || c == '_' || c == '.' || c == '~') {
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

bool HttpClient::respIsSucc(const HttpResponse& resp) {
    return resp.isSuccess();
}

bool HttpClient::isValidUrl(std::string_view url) noexcept {
    if (url.empty()) {
        return false;
    }
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return true;
    }
    auto scheme = url.substr(0, scheme_end);
    if (scheme != "http" && scheme != "https") {
        return false;
    }
    auto rest = url.substr(scheme_end + 3);
    return !rest.empty();
}

bool HttpClient::isRedirectStatus(int status) noexcept {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

bool HttpClient::redirectChangesToGet(int status) noexcept {
    return status == 301 || status == 302 || status == 303;
}

std::string HttpClient::resolveRedirectUrl(
    std::string_view originalUrl,
    std::string_view location
) noexcept {
    if (location.find("://") != std::string_view::npos) {
        return std::string(location);
    }
    // Protocol-relative URL: //host/path -> scheme://host/path
    if (location.starts_with("//")) {
        auto schemeEnd = originalUrl.find("://");
        if (schemeEnd != std::string_view::npos) {
            return fmt::format("{}:{}", originalUrl.substr(0, schemeEnd), location);
        }
        return std::string(location);
    }
    auto [base, path] = splitUrl(originalUrl);
    if (location.starts_with('/')) {
        return base + std::string(location);
    }
    auto        slashPos = path.rfind('/');
    std::string basePath = (slashPos != std::string_view::npos && slashPos > 0)
                               ? std::string(path.substr(0, slashPos + 1))
                               : "/";
    return base + basePath + std::string(location);
}

const HeaderMap& HttpClient::defaultHeaders() {
    static const HeaderMap headers = [] {
        HeaderMap h;
        h.set(
            "User-Agent",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/119.0.6045.160 Safari/537.36"
        );
        h.set("Accept", "*/*");
        h.set("Accept-Language", "zh-CN,zh;q=0.9");
        return h;
    }();
    return headers;
}

asio::ssl::context& HttpClient::sharedSslCtx(bool verify) {
    static std::unique_ptr<asio::ssl::context> verifiedCtx;
    static std::unique_ptr<asio::ssl::context> unverifiedCtx;
    static std::once_flag                      verifiedFlag;
    static std::once_flag                      unverifiedFlag;

    if (verify) {
        std::call_once(verifiedFlag, [] {
            auto ctx = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv12_client);
            ctx->set_verify_mode(asio::ssl::verify_peer);
            ctx->set_default_verify_paths();
            verifiedCtx = std::move(ctx);
        });
        return *verifiedCtx;
    } else {
        std::call_once(unverifiedFlag, [] {
            auto ctx = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv12_client);
            ctx->set_verify_mode(asio::ssl::verify_none);
            unverifiedCtx = std::move(ctx);
        });
        return *unverifiedCtx;
    }
}

std::optional<HttpClient::ParsedUrl> HttpClient::parseUrl(std::string_view url) {
    auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        return std::nullopt;
    }
    std::string scheme{url.substr(0, schemeEnd)};
    if (scheme != "http" && scheme != "https") {
        return std::nullopt;
    }
    auto rest = url.substr(schemeEnd + 3);
    if (rest.empty()) {
        return std::nullopt;
    }
    auto             pathStart = rest.find('/');
    std::string_view hostPort = (pathStart == std::string::npos) ? rest : rest.substr(0, pathStart);
    std::string path = (pathStart == std::string::npos) ? "/" : std::string{rest.substr(pathStart)};
    uint16_t    port = (scheme == "https") ? 443 : 80;
    std::string host;
    if (hostPort.starts_with('[')) {
        auto cb = hostPort.find(']');
        if (cb == std::string::npos) {
            return std::nullopt;
        }
        host = std::string{hostPort.substr(0, cb + 1)};
        if (cb + 1 < hostPort.size() && hostPort[cb + 1] == ':') {
            auto portStart = hostPort.data() + cb + 2;
            auto portEnd   = hostPort.data() + hostPort.size();
            int  p         = 0;
            auto [ptr, ec] = std::from_chars(portStart, portEnd, p);
            if (ec != std::errc{} || ptr != portEnd || p <= 0 || p > 65535) {
                return std::nullopt;
            }
            port = static_cast<uint16_t>(p);
        }
    } else {
        auto colon = hostPort.rfind(':');
        if (colon != std::string_view::npos) {
            host           = std::string{hostPort.substr(0, colon)};
            auto portStart = hostPort.data() + colon + 1;
            auto portEnd   = hostPort.data() + hostPort.size();
            int  p         = 0;
            auto [ptr, ec] = std::from_chars(portStart, portEnd, p);
            if (ec != std::errc{} || ptr != portEnd || p <= 0 || p > 65535) {
                return std::nullopt;
            }
            port = static_cast<uint16_t>(p);
        } else {
            host = std::string{hostPort};
        }
    }
    if (host.empty()) {
        return std::nullopt;
    }
    return ParsedUrl{std::move(scheme), std::move(host), port, std::move(path)};
}

std::chrono::seconds HttpClient::calcSendTimeout(size_t bodyBytes) {
    int64_t seconds = (static_cast<int64_t>(bodyBytes) + 65535) / 65536;
    return std::chrono::seconds{std::max<int64_t>(30, seconds)};
}

asio::awaitable<std::expected<HttpResponse, std::string>> HttpClient::requestAsync(
    std::string_view     method,
    std::string_view     url,
    std::string_view     body,
    std::string_view     contentType,
    const HeaderMap&     extraHeaders,
    const RequestConfig& config
) {
    namespace http = boost::beast::http;
    using asio::ip::tcp;

    std::string currentUrl{url};
    std::string currentMethod(method);
    std::string currentBody(body);
    std::string currentContentType(contentType);

    auto          executor = co_await asio::this_coro::executor;
    tcp::resolver resolver(executor);

    std::expected<HttpResponse, std::string> result;
    for (size_t redirectCount = 0;; ++redirectCount) {
        result = std::unexpected{std::string{"unknown error"}};
        try {
            auto parsed = parseUrl(currentUrl);
            if (!parsed) {
                throw std::runtime_error{"invalid url: " + currentUrl};
            }
            bool isHttps     = parsed->scheme == "https";
            bool defaultPort = (isHttps && parsed->port == 443) || (!isHttps && parsed->port == 80);
            std::string hostHeader = parsed->host;
            if (!defaultPort) {
                hostHeader += ":" + std::to_string(parsed->port);
            }

            http::request<http::string_body> req{
                http::string_to_verb(currentMethod),
                parsed->path,
                11
            };
            bool hasHost = extraHeaders.contains("host");
            if (!hasHost) {
                req.set(http::field::host, hostHeader);
            }
            req.set(
                http::field::user_agent,
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/119.0.6045.160 Safari/537.36"
            );
            req.set(http::field::accept, "*/*");
            req.set(http::field::accept_encoding, "identity");
            if (!config.keepAlive) {
                req.set(http::field::connection, "close");
            }

            for (const auto& [k, v] : extraHeaders.data) {
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

            auto endpoints = co_await resolver.async_resolve(
                parsed->host,
                std::to_string(parsed->port),
                asio::cancel_after(config.connectTimeout, asio::use_awaitable)
            );

            if (isHttps) {
                bool verify
                    = config.sslVerify.value_or(sslVerifyEnabled_.load(std::memory_order_relaxed));
                auto&                          sslCtx = sharedSslCtx(verify);
                asio::ssl::stream<tcp::socket> stream(executor, sslCtx);
                if (!parsed->host.empty()) {
                    ::SSL_set_tlsext_host_name(stream.native_handle(), parsed->host.c_str());
                }
                co_await asio::async_connect(
                    stream.lowest_layer(),
                    endpoints,
                    asio::cancel_after(config.connectTimeout, asio::use_awaitable)
                );
                boost::system::error_code tcpEc;
                stream.lowest_layer().set_option(asio::ip::tcp::no_delay(true), tcpEc);
                co_await stream.async_handshake(
                    asio::ssl::stream_base::client,
                    asio::cancel_after(config.connectTimeout, asio::use_awaitable)
                );
                result = co_await exchange(stream, req, config);
                boost::system::error_code sslEc;
                co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, sslEc));
            } else {
                tcp::socket stream(executor);
                co_await asio::async_connect(
                    stream,
                    endpoints,
                    asio::cancel_after(config.connectTimeout, asio::use_awaitable)
                );
                boost::system::error_code tcpEc;
                stream.set_option(asio::ip::tcp::no_delay(true), tcpEc);
                result = co_await exchange(stream, req, config);
            }
        } catch (const boost::system::system_error& e) {
            if (e.code() == asio::error::operation_aborted) {
                result = std::unexpected{std::string{"timeout"}};
            } else {
                auto errInfo = std::string{e.what()};
                agentxx::util::autoConvertToUtf8(errInfo);
                result = std::unexpected{errInfo};
            }
        } catch (const std::exception& e) {
            auto errInfo = std::string{e.what()};
            agentxx::util::autoConvertToUtf8(errInfo);
            result = std::unexpected{errInfo};
        }

        if (!result.has_value()) {
            break;
        }
        if (redirectCount >= config.followRedirect) {
            break;
        }
        auto& resp = result.value();
        if (!isRedirectStatus(resp.status)) {
            break;
        }
        auto location = resp.findHeader("location");
        if (location.empty()) {
            break;
        }

        currentUrl = resolveRedirectUrl(currentUrl, location);
        if (redirectChangesToGet(resp.status)) {
            currentMethod      = "GET";
            currentBody        = {};
            currentContentType = {};
        }
    }
    co_return result;
}

void HttpClient::setSslVerify(bool enable) noexcept {
    sslVerifyEnabled_.store(enable, std::memory_order_relaxed);
}

bool HttpClient::getSslVerify() noexcept {
    return sslVerifyEnabled_.load(std::memory_order_relaxed);
}

asio::awaitable<std::expected<HttpResponse, std::string>> HttpClient::getAsync(
    std::string_view     url,
    const HeaderMap&     extraHeaders,
    const RequestConfig& config
) {
    co_return co_await requestAsync("GET", url, {}, "", extraHeaders, config);
}

asio::awaitable<std::expected<HttpResponse, std::string>> HttpClient::headAsync(
    std::string_view     url,
    const HeaderMap&     extraHeaders,
    const RequestConfig& config
) {
    co_return co_await requestAsync("HEAD", url, {}, "", extraHeaders, config);
}

asio::awaitable<std::expected<HttpResponse, std::string>> HttpClient::postAsync(
    std::string_view      url,
    const neograph::json& body,
    const HeaderMap&      extraHeaders,
    const RequestConfig&  config
) {
    co_return co_await requestAsync(
        "POST",
        url,
        body.dump(),
        "application/json",
        extraHeaders,
        config
    );
}

asio::awaitable<std::expected<HttpResponse, std::string>> HttpClient::postAsync(
    std::string_view     url,
    std::string_view     body,
    std::string_view     contentType,
    const HeaderMap&     extraHeaders,
    const RequestConfig& config
) {
    co_return co_await requestAsync("POST", url, body, contentType, extraHeaders, config);
}

asio::awaitable<std::expected<HttpResponse, std::string>> HttpClient::putAsync(
    std::string_view     url,
    std::string_view     body,
    std::string_view     contentType,
    const HeaderMap&     extraHeaders,
    const RequestConfig& config
) {
    co_return co_await requestAsync("PUT", url, body, contentType, extraHeaders, config);
}

asio::awaitable<std::expected<HttpResponse, std::string>> HttpClient::patchAsync(
    std::string_view     url,
    std::string_view     body,
    std::string_view     contentType,
    const HeaderMap&     extraHeaders,
    const RequestConfig& config
) {
    co_return co_await requestAsync("PATCH", url, body, contentType, extraHeaders, config);
}

asio::awaitable<std::expected<HttpResponse, std::string>> HttpClient::deleteAsync(
    std::string_view     url,
    const HeaderMap&     extraHeaders,
    const RequestConfig& config
) {
    co_return co_await requestAsync("DELETE", url, {}, "", extraHeaders, config);
}

asio::awaitable<std::expected<HttpResponse, std::string>> HttpClient::optionsAsync(
    std::string_view     url,
    const HeaderMap&     extraHeaders,
    const RequestConfig& config
) {
    co_return co_await requestAsync("OPTIONS", url, {}, "", extraHeaders, config);
}

asio::awaitable<std::expected<std::string, std::string>>
    HttpClient::fetchMarkdown(std::string_view url, const RequestConfig& config) {
    auto resp = co_await getAsync(url, {}, config);
    if (!resp.has_value()) {
        XX_LOGE("fetchMarkdown error: {}", resp.error());
        co_return std::unexpected{resp.error()};
    }
    auto& respVal = resp.value();
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

} // namespace util
} // namespace agentxx
