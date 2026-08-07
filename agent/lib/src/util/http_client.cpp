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

#include "agentxx/util/exception.h"
#include "agentxx/util/http_client.h"
#include "html2md/html2md.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/steady_timer.hpp>
#include <neograph/provider.h>
#include <openssl/ssl.h>

namespace agentxx {
namespace util {

namespace {

/// 启用 TCP keepalive 并缩短探测间隔 (OS 默认 idle 常为 2 小时, 毫无作用)。
/// LLM 流式响应存在长时间无数据的空窗期 (如 content 结束后等待 tool_call 下发),
/// 中间 NAT/负载均衡/网关会静默丢弃空闲连接, 导致 stream truncated / connection
/// reset 等错误; keepalive 探测包可在空窗期维持中间设备状态, 避免连接被丢弃。
/// best-effort: 设置失败不影响正常请求。
template<typename Socket>
void enableTcpKeepalive(Socket& sock) noexcept {
    neograph_asio_error_code ec;
    sock.set_option(asio::socket_base::keep_alive(true), ec);
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
    // asio 未预定义 keepalive 探测选项, 按 asio 文档 Custom socket option 的方式用
    // asio::detail::socket_option::integer 定义平台选项 (Linux/Android/Windows10+
    // 均支持这三个选项, 单位秒): 空闲 30 秒后开始探测, 每 10 秒一次, 连续 3 次无响应判定断开
    asio::detail::socket_option::integer<IPPROTO_TCP, TCP_KEEPIDLE>  idle{30};
    asio::detail::socket_option::integer<IPPROTO_TCP, TCP_KEEPINTVL> interval{10};
    asio::detail::socket_option::integer<IPPROTO_TCP, TCP_KEEPCNT>   count{3};
    sock.set_option(idle, ec);
    sock.set_option(interval, ec);
    sock.set_option(count, ec);
#endif
}

/// SSE 阶段操作的超时保护 (发送请求/读响应头/读 body 通用)。
///
/// 为什么不用 cancel_after:
/// - cancel_after 的 terminal 取消信号要穿透 beast composed op -> ssl::detail::io_op
///   -> socket read/write 整条链; 在 ssl::stream 上存在取消信号丢失的窗口 (timer 触发后
///   新发起的读/写看不到已发生的取消; 被取消的 SSL 操作还会把 pending_read_/pending_write_
///   标记 timer 卡在 pos_infin), 会导致协程永久挂起, 既无超时也无异常抛出。
/// - close 底层连接是唯一能 100% 打断挂起 SSL 读/写的手段; 对 SSE 流而言读取超时本就
///   意味着连接已不可用, 直接断开由上层 (provider) 重试/报错。
///
/// 语义: 在 timeout 内执行 op; 超时则强制关闭连接并抛 std::runtime_error;
/// op 自身抛出的异常 (eof/stream_truncated 等) 原样传播 (不会退化为等待超时)。
template<typename Stream, typename Op>
asio::awaitable<void> runSseOpWithTimeout(
    Stream&                   stream,
    std::chrono::milliseconds timeout,
    std::string_view          stage,
    Op&&                      op
) {
    auto ex = co_await asio::this_coro::executor;

    // 共享状态: timer 回调可能晚于协程迭代/退出执行, 必须堆分配避免悬空引用
    auto timer    = std::make_shared<asio::steady_timer>(ex, timeout);
    auto timedOut = std::make_shared<std::atomic<bool>>(false);

    // 协程退出 (正常或异常) 时必须取消计时器, 否则其回调可能晚于 stream 销毁执行
    struct TimerCleanup {
        asio::steady_timer& timer;

        ~TimerCleanup() {
            // 本 asio 版本的 timer::cancel() 只有无参形式 (可能抛异常),
            // 析构函数中不允许抛出, 因此吞掉
            try {
                timer.cancel();
            } catch (...) {
            }
        }
    } cleanup{*timer};

    timer->async_wait([timer, timedOut, &stream, stage, timeout](const neograph_asio_error_code& ec
                      ) {
        if (ec) {
            return; // 被取消 (操作先完成) 或计时器已销毁
        }
        timedOut->store(true);
        XX_LOGW(
            "HttpClient::requestSseAsync {} timeout after {} ms, closing connection",
            stage,
            timeout.count()
        );
        // 强制关闭底层连接: 挂起的读/写会以 operation_aborted 完成, 从而打断协程
        neograph_asio_error_code ignore;
        stream.lowest_layer().close(ignore);
    });

    try {
        co_await std::forward<Op>(op)();
    } catch (const neograph_asio_system_error&) {
        if (timedOut->load()) {
            // 超时已发生: close 打断 op 的完成错误 (operation_aborted) 按超时处理,
            // 而不是作为普通传输错误抛出
            throw std::runtime_error(
                fmt::format("SSE {} timeout after {} ms", stage, timeout.count())
            );
        }
        throw;
    }

    // op 正常完成, 计时器被上方 RAII 取消; 防御性检查: 若恰好同时到期 (单线程
    // 事件循环下 close 会先于 op 完成执行, 此分支实际不可达) 也按超时处理
    if (timedOut->load()) {
        throw std::runtime_error(fmt::format("SSE {} timeout after {} ms", stage, timeout.count()));
    }
}

} // namespace

/// DNS 解析的共享结果状态。
///
/// 背景: asio 的 async_resolve 在独立后台线程执行阻塞的 getaddrinfo (受系统
/// resolv.conf 的 timeout/attempts 控制), 该调用**无法被中断** —— 无论是
/// cancel_after 超时还是外部的 cancellation_signal, 都只能标记取消, 真正返回
/// 要等 getaddrinfo 完成。当 DNS 黑洞/服务器无响应时, 这个等待可能长达数十秒
/// 甚至永不返回, 导致请求挂死、用户取消失效、connectTimeout 形同虚设。
///
/// 因此把解析放到独立后台协程执行, 结果写入本状态; 请求协程轮询 done 标志,
/// 超时/取消时立即放弃等待 (后台协程持有状态直至解析完成自行收尾, 无悬垂)。
struct DnsResolveState {
    std::atomic<bool>                     done{false};
    neograph_asio_error_code              ec;
    asio::ip::tcp::resolver::results_type results;
};

/// 启动后台 DNS 解析。host/port 按值拷贝: 请求协程可能先于解析完成而销毁。
std::shared_ptr<DnsResolveState>
    startDnsResolve(asio::any_io_executor executor, std::string host, std::string port) {
    auto state = std::make_shared<DnsResolveState>();
    asio::co_spawn(
        executor,
        [state, host = std::move(host), port = std::move(port)]() -> asio::awaitable<void> {
            asio::ip::tcp::resolver  resolver(co_await asio::this_coro::executor);
            neograph_asio_error_code ec;
            auto                     results = co_await resolver.async_resolve(
                host,
                port,
                asio::redirect_error(asio::use_awaitable, ec)
            );
            state->ec      = ec;
            state->results = std::move(results);
            state->done.store(true, std::memory_order_release);
        },
        asio::detached
    );
    return state;
}

/// 等待 DNS 解析完成或超时。
/// - 解析完成: 有错误抛 neograph_asio_system_error, 否则返回 results
/// - 超过 connectDeadline: 抛 std::runtime_error 立即返回 (后台解析协程继续
///   自行完成, 不阻塞本协程; 若 getaddrinfo 永不返回, 该协程作为挂起状态保留,
///   但不影响后续请求)
/// - 外部取消: co_await 定时器时抛 operation_aborted, 由上层 catchErrorAsync
///   按取消语义处理, 立即中止
asio::awaitable<asio::ip::tcp::resolver::results_type> waitDnsResolve(
    std::shared_ptr<DnsResolveState>      state,
    std::chrono::steady_clock::time_point connectDeadline,
    std::chrono::milliseconds             connectTimeout
) {
    auto               executor = co_await asio::this_coro::executor;
    asio::steady_timer pollTimer(executor);
    while (!state->done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= connectDeadline) {
            throw std::runtime_error(
                fmt::format("DNS resolve timeout after {} ms", connectTimeout.count())
            );
        }
        // 短间隔轮询: 超时上限内的误差可忽略; 同时每次 co_await 都是取消检查点
        pollTimer.expires_after(std::chrono::milliseconds(10));
        co_await pollTimer.async_wait(asio::use_awaitable);
    }
    if (state->ec) {
        throw neograph_asio_system_error(state->ec);
    }
    co_return std::move(state->results);
}

asio::awaitable<asio::ip::tcp::resolver::results_type> HttpClient::asyncResolveWithDeadline(
    std::string_view                      host,
    std::string_view                      port,
    std::chrono::steady_clock::time_point connectDeadline,
    std::chrono::milliseconds             connectTimeout
) {
    auto executor = co_await asio::this_coro::executor;
    auto state    = startDnsResolve(executor, std::string{host}, std::string{port});
    co_return co_await waitDnsResolve(state, connectDeadline, connectTimeout);
}

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
    // 解析失败 (非法 JSON) 返回 nullopt 而不是抛异常
    return agentxx::util::catchError<std::optional<neograph::json>>(
        [this]() -> std::optional<neograph::json> {
            return neograph::json::parse(body);
        },
        [](std::string) -> std::optional<neograph::json> {
            return std::nullopt;
        }
    );
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
    if (scheme_end == std::string_view::npos) {
        return std::pair<std::string, std::string>{url, "/"};
    }
    auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string_view::npos) {
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
    if (scheme_end == std::string_view::npos) {
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
    // RFC 7231: Location 中的 fragment (#...) 不属于资源定位, 跟随重定向前应去除
    if (auto hash = location.find('#'); hash != std::string_view::npos) {
        location = location.substr(0, hash);
    }
    // 空 Location (或仅含 fragment): 指向原资源自身
    if (location.empty()) {
        return std::string(originalUrl);
    }
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
        // 去掉方括号: DNS 解析 (getaddrinfo) 与 SNI 不接受带括号的 IPv6 字面量,
        // 构造 Host 头时再按 HTTP 规范加回 (见 buildHostHeader)
        host = std::string{hostPort.substr(1, cb - 1)};
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

std::string HttpClient::buildHostHeader(const ParsedUrl& parsed) {
    bool isHttps     = parsed.scheme == "https";
    bool defaultPort = (isHttps && parsed.port == 443) || (!isHttps && parsed.port == 80);
    // IPv6 字面量 (host 含 ':') 在 Host 头中必须带方括号
    std::string hostHeader = (parsed.host.find(':') != std::string::npos)
                                 ? fmt::format("[{}]", parsed.host)
                                 : parsed.host;
    if (!defaultPort) {
        hostHeader += fmt::format(":{}", parsed.port);
    }
    return hostHeader;
}

std::chrono::seconds HttpClient::calcTimeoutBySize(size_t bodyBytes) {
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

    auto executor = co_await asio::this_coro::executor;

    std::expected<HttpResponse, std::string> result;
    for (size_t redirectCount = 0;; ++redirectCount) {
        result = std::unexpected{std::string{"unknown error"}};
        co_await agentxx::util::catchErrorAsync<bool>(
            [&]() -> asio::awaitable<bool> {
                auto parsed = parseUrl(currentUrl);
                if (!parsed) {
                    throw std::runtime_error{fmt::format("invalid url: {}", currentUrl)};
                }
                bool isHttps = parsed->scheme == "https";

                http::request<http::string_body> req{
                    http::string_to_verb(currentMethod),
                    parsed->path,
                    11
                };
                // 先设置计算的 Host 与默认头, extraHeaders 在最后统一覆盖
                // (beast::set 大小写不敏感地替换同名字段, 因此调用方自定义的 Host 等可生效)
                req.set(http::field::host, buildHostHeader(*parsed));
                for (const auto& [k, v] : defaultHeaders().data) {
                    req.set(k, stringVectorJoin(v, "; "));
                }
                req.set(http::field::accept_encoding, "identity");
                if (!config.keepAlive) {
                    req.set(http::field::connection, "close");
                }

                for (const auto& [k, v] : extraHeaders.data) {
                    req.set(k, stringVectorJoin(v, "; "));
                }
                if (!currentBody.empty()) {
                    if (!currentContentType.empty()) {
                        req.set(http::field::content_type, currentContentType);
                    }
                    req.body() = currentBody;
                    req.prepare_payload();
                } else if (currentMethod == "POST" || currentMethod == "PUT" || currentMethod == "PATCH") {
                    // 空 body 也必须携带 Content-Length: 0, 否则部分服务器/代理会一直等待
                    // body 数据或拒绝请求 (RFC 7230 §3.3.2)
                    if (!currentContentType.empty()) {
                        req.set(http::field::content_type, currentContentType);
                    }
                    req.prepare_payload();
                }

                // connectTimeout 是 DNS + TCP + TLS 的总上限:
                // 用 deadline 记录起始时刻, 后续各阶段 (含 DNS) 使用剩余时间,
                // 避免各阶段各用满导致总耗时成倍放大
                auto connectDeadline  = std::chrono::steady_clock::now() + config.connectTimeout;
                auto remainingTimeout = [&]() -> std::chrono::milliseconds {
                    auto now = std::chrono::steady_clock::now();
                    if (now >= connectDeadline) {
                        // 留一点时间
                        return std::chrono::seconds{1};
                    }
                    return std::chrono::duration_cast<std::chrono::milliseconds>(
                        connectDeadline - now
                    );
                };

                // DNS 解析不能直接 co_await: getaddrinfo 阻塞无法被取消/超时中断,
                // 黑洞 DNS 时会无限挂起。后台协程解析 + 轮询, 超时/取消立即放弃等待
                // (见 HttpClient::asyncResolveWithDeadline 注释)
                auto endpoints = co_await HttpClient::asyncResolveWithDeadline(
                    parsed->host,
                    std::to_string(parsed->port),
                    connectDeadline,
                    config.connectTimeout
                );

                if (isHttps) {
                    bool verify
                        = config.sslVerify.value_or(sslVerifyEnabled_.load(std::memory_order_relaxed
                        ));
                    auto&                          sslCtx = sharedSslCtx(verify);
                    asio::ssl::stream<tcp::socket> stream(executor, sslCtx);
                    // SNI 只能是域名, IP 字面量 (IPv6 含 ':') 不支持 SNI
                    if (!parsed->host.empty() && parsed->host.find(':') == std::string::npos) {
                        ::SSL_set_tlsext_host_name(stream.native_handle(), parsed->host.c_str());
                    }
                    co_await asio::async_connect(
                        stream.lowest_layer(),
                        endpoints,
                        asio::cancel_after(remainingTimeout(), asio::use_awaitable)
                    );
                    neograph_asio_error_code tcpEc;
                    stream.lowest_layer().set_option(asio::ip::tcp::no_delay(true), tcpEc);
                    enableTcpKeepalive(stream.lowest_layer());
                    co_await stream.async_handshake(
                        asio::ssl::stream_base::client,
                        asio::cancel_after(remainingTimeout(), asio::use_awaitable)
                    );
                    result = co_await exchange(stream, req, config);
                    // shutdown 失败 (如对端已提前关闭) 不影响已成功获取的响应,
                    // 用 redirect_error 捕获并忽略, 避免异常丢弃上面的成功 result
                    // 加 cancel_after 防止对端不响应 close_notify 时永久挂起
                    neograph_asio_error_code sslEc;
                    co_await stream.async_shutdown(asio::cancel_after(
                        std::chrono::seconds{5},
                        asio::redirect_error(asio::use_awaitable, sslEc)
                    ));
                } else {
                    tcp::socket stream(executor);
                    co_await asio::async_connect(
                        stream,
                        endpoints,
                        asio::cancel_after(remainingTimeout(), asio::use_awaitable)
                    );
                    neograph_asio_error_code tcpEc;
                    stream.set_option(asio::ip::tcp::no_delay(true), tcpEc);
                    enableTcpKeepalive(stream);
                    result = co_await exchange(stream, req, config);
                }
                co_return true;
            },
            [&](std::string errInfo) -> asio::awaitable<bool> {
                result = std::unexpected{errInfo};
                co_return true;
            }
        );

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

asio::awaitable<void> HttpClient::requestSseAsync(
    std::string_view                      method,
    std::string_view                      url,
    std::string_view                      body,
    std::string_view                      contentType,
    const HeaderMap&                      extraHeaders,
    const RequestConfig&                  config,
    std::function<bool(std::string_view)> onChunk
) {
    namespace http = boost::beast::http;
    using asio::ip::tcp;

    auto parsed = parseUrl(url);
    if (!parsed) {
        throw std::runtime_error{fmt::format("invalid url: {}", url)};
    }
    bool isHttps = parsed->scheme == "https";

    http::request<http::string_body> req{http::string_to_verb(method), parsed->path, 11};
    // 先设置计算的 Host 与默认头, extraHeaders 在最后统一覆盖
    // (beast::set 大小写不敏感地替换同名字段, 因此调用方自定义的 Host 等可生效)
    req.set(http::field::host, buildHostHeader(*parsed));
    for (const auto& [k, v] : defaultHeaders().data) {
        req.set(k, stringVectorJoin(v, "; "));
    }
    req.set(http::field::accept, "text/event-stream");
    req.set(http::field::accept_encoding, "identity");
    req.set(http::field::connection, "keep-alive");

    for (const auto& [k, v] : extraHeaders.data) {
        req.set(k, stringVectorJoin(v, "; "));
    }
    if (!body.empty()) {
        if (!contentType.empty()) {
            req.set(http::field::content_type, contentType);
        }
        req.body() = body;
        req.prepare_payload();
    }

    auto executor = co_await asio::this_coro::executor;

    // connectTimeout 是 DNS + TCP + TLS 的总上限: 各阶段 (含 DNS) 共用剩余时间
    auto connectDeadline = std::chrono::steady_clock::now() + config.connectTimeout;
    auto remainingMs     = [&]() -> std::chrono::milliseconds {
        auto now = std::chrono::steady_clock::now();
        if (now >= connectDeadline) {
            return std::chrono::seconds{1};
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(connectDeadline - now);
    };

    // DNS 解析不能直接 co_await: getaddrinfo 阻塞无法被取消/超时中断, 黑洞 DNS
    // 时会无限挂起。改为后台协程解析 + 轮询, 超时/取消立即放弃等待 (见
    // HttpClient::asyncResolveWithDeadline 注释)
    XX_LOGT("HttpClient::requestSseAsync: async_resolve");
    auto endpoints = co_await HttpClient::asyncResolveWithDeadline(
        parsed->host,
        std::to_string(parsed->port),
        connectDeadline,
        config.connectTimeout
    );

    auto sendTimeout = config.sendTimeout.value_or(calcTimeoutBySize(req.body().size()));

    auto doSseExchange = [&](auto& stream) -> asio::awaitable<void> {
        // 发送请求体: 服务器不读 body (如 TCP 窗口阻塞) 时写会长期挂起, 超时直接断开
        co_await runSseOpWithTimeout(
            stream,
            sendTimeout,
            "write-request",
            [&]() -> asio::awaitable<void> {
                co_await http::async_write(stream, req, asio::use_awaitable);
            }
        );

        boost::beast::flat_buffer                buf;
        http::response_parser<http::string_body> parser;
        parser.body_limit(std::numeric_limits<uint64_t>::max());

        // 读响应头: 部分服务器/网关收到请求后迟迟不返回响应头, 超时直接断开
        co_await runSseOpWithTimeout(
            stream,
            config.readChunkTimeout,
            "read-header",
            [&]() -> asio::awaitable<void> {
                co_await http::async_read_header(stream, buf, parser, asio::use_awaitable);
            }
        );

        if (parser.get().result_int() == 429) {
            co_await runSseOpWithTimeout(
                stream,
                config.readChunkTimeout,
                "read-body",
                [&]() -> asio::awaitable<void> {
                    co_await http::async_read(stream, buf, parser, asio::use_awaitable);
                }
            );
            auto resp       = parser.release();
            auto raw        = resp[http::field::retry_after];
            int  retryAfter = -1;
            if (!raw.empty()) {
                int seconds    = 0;
                auto [ptr, ec] = parseNumberFromString(raw, seconds);
                if (ec == std::errc{} && seconds >= 0) {
                    retryAfter = seconds;
                }
            }
            throw neograph::RateLimitError(
                fmt::format("API error (HTTP 429): {}", resp.body()),
                retryAfter
            );
        }

        // 部分网关对 SSE 返回 201/202 等其它 2xx 状态码也视为成功流,
        // 因此仅拒绝非 2xx (429 已在上面单独处理)
        if (parser.get().result_int() / 100 != 2) {
            co_await runSseOpWithTimeout(
                stream,
                config.readChunkTimeout,
                "read-body",
                [&]() -> asio::awaitable<void> {
                    co_await http::async_read(stream, buf, parser, asio::use_awaitable);
                }
            );
            auto resp = parser.release();
            // 错误 body 可能很大 (如 HTML 错误页), 截断避免异常消息爆炸
            constexpr size_t kMaxErrorBody = 2048;
            auto             errBody       = resp.body();
            if (errBody.size() > kMaxErrorBody) {
                errBody.resize(kMaxErrorBody);
            }
            throw std::runtime_error(
                fmt::format("API error (HTTP {}): {}", resp.result_int(), errBody)
            );
        }

        size_t processed = 0;

        // 返回 true 表示 onChunk 告知流已结束 (如 [DONE]), 应停止读取并断开连接
        auto flushBody = [&]() -> bool {
            auto& respBody = parser.get().body();
            bool  stop     = false;
            if (respBody.size() > processed) {
                stop = onChunk(std::string_view{respBody}.substr(processed));
            }
            // 清空已读 body 并重置偏移: SSE 为长连接流, 若只移动偏移不裁剪,
            // string_body 会随流持续无限增长 → OOM
            respBody.clear();
            processed = 0;
            return stop;
        };

        // 初始 flush: 响应头与首个 body 数据可能在同一数据块中到达
        if (flushBody()) {
            // 首个数据块即含流结束标记: 直接断开
            neograph_asio_error_code ignore;
            stream.lowest_layer().close(ignore);
        } else {
            while (!parser.is_done()) {
                // 读 body: 每次读操作独立计时 (readChunkTimeout = 块间最大间隔), 超时断开
                co_await runSseOpWithTimeout(
                    stream,
                    config.readChunkTimeout,
                    "read-body",
                    [&]() -> asio::awaitable<void> {
                        co_await http::async_read_some(stream, buf, parser, asio::use_awaitable);
                    }
                );
                if (flushBody()) {
                    // 流已结束 (如收到 [DONE]): 主动断开连接, 避免对端 keep-alive
                    // 不关闭时白等 readChunkTimeout; 断开后不会再读后续数据
                    neograph_asio_error_code ignore;
                    stream.lowest_layer().close(ignore);
                    break;
                }
            }
            flushBody();
        }
    };

    if (isHttps) {
        bool  verify = config.sslVerify.value_or(sslVerifyEnabled_.load(std::memory_order_relaxed));
        auto& sslCtx = sharedSslCtx(verify);
        asio::ssl::stream<tcp::socket> stream(executor, sslCtx);
        // SNI 只能是域名, IP 字面量 (IPv6 含 ':') 不支持 SNI
        if (!parsed->host.empty() && parsed->host.find(':') == std::string::npos) {
            ::SSL_set_tlsext_host_name(stream.native_handle(), parsed->host.c_str());
        }
        XX_LOGT("HttpClient::requestSseAsync: HTTPS async_connect");
        co_await asio::async_connect(
            stream.lowest_layer(),
            endpoints,
            asio::cancel_after(remainingMs(), asio::use_awaitable)
        );
        neograph_asio_error_code tcpEc;
        stream.lowest_layer().set_option(asio::ip::tcp::no_delay(true), tcpEc);
        enableTcpKeepalive(stream.lowest_layer());
        XX_LOGT("HttpClient::requestSseAsync: HTTPS async_handshake");
        co_await stream.async_handshake(
            asio::ssl::stream_base::client,
            asio::cancel_after(remainingMs(), asio::use_awaitable)
        );
        XX_LOGT("HttpClient::requestSseAsync: HTTPS doSseExchange");
        co_await doSseExchange(stream);
        XX_LOGT("HttpClient::requestSseAsync: HTTPS shutdown");
        neograph_asio_error_code sslEc;
        co_await stream.async_shutdown(asio::cancel_after(
            std::chrono::seconds{5},
            asio::redirect_error(asio::use_awaitable, sslEc)
        ));
        XX_LOGT("HttpClient::requestSseAsync: HTTPS DONE");
    } else {
        tcp::socket stream(executor);
        XX_LOGT("HttpClient::requestSseAsync: HTTP async_connect");
        co_await asio::async_connect(
            stream,
            endpoints,
            asio::cancel_after(remainingMs(), asio::use_awaitable)
        );
        neograph_asio_error_code tcpEc;
        stream.set_option(asio::ip::tcp::no_delay(true), tcpEc);
        enableTcpKeepalive(stream);
        XX_LOGT("HttpClient::requestSseAsync: HTTP doSseExchange");
        co_await doSseExchange(stream);
        XX_LOGT("HttpClient::requestSseAsync: HTTP DONE");
    }
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
