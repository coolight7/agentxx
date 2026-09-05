#pragma once

#include "agentxx/util/http_header.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/awaitable.hpp"
#include "asio/cancel_after.hpp"
#include "asio/ip/tcp.hpp"
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
    /// 响应是否允许 keep-alive (HTTP/1.1 默认或显式 Connection: keep-alive)。
    /// - 由 exchange 在解析完整响应后填充 (基于响应版本与 Connection 头)
    /// - 连接池据此决定响应完成后连接是否可归还复用
    bool keepAlive = false;

    std::string_view findHeader(std::string_view name) const noexcept;

    bool isSuccess() const noexcept;

    std::string contentType() const noexcept;

    static bool isJsonContentType(std::string_view contentType) noexcept;

    static bool isTextContentType(std::string_view contentType) noexcept;

    std::optional<neograph::json> bodyJson() const;

    std::optional<std::string> bodyText() const;
};

/// 默认最大响应体大小 (10 MB), 防止内存耗尽
inline constexpr uint64_t kDefaultMaxResponseBody = 10 * 1024 * 1024;

/// 单次请求配置 (低频定制项)
/// - connectTimeout: DNS 解析 + TCP 连接 + TLS 握手 的总期限。三个阶段共享
///   同一期限, 最坏总耗时恰为 connectTimeout (而不是分段限制的 3 倍)。
///   注意: DNS 解析 (getaddrinfo) 一旦开始无法中断, 因此放到后台协程执行,
///   请求方在此期限到达时放弃等待 (见
///   [http_client.cpp](/agent/lib/src/util/http_client.cpp) startDnsResolve/waitDnsResolve)
/// - sslVerify: 本次请求是否校验 TLS 证书; nullopt 时回退全局默认
///   (见 HttpClient::setSslVerify)
/// - sendTimeout: 写请求体的期限; nullopt 时按请求体大小自动推导
///   (见 HttpClient::calcTimeoutBySize)
/// - readChunkTimeout: 相邻数据块到达间隔的期限; readChunkTimeout 内没有新数据
///   到达视为超时 (每收到新数据计时器重置)。这是"块间"超时, 不是整个响应的
///   总超时 —— 只要数据持续到达, 连接就一直保持
/// - keepAlive: 启用 HTTP keep-alive + 连接池复用 (见 maxConcurrentConnections)。
///   false (默认) 时请求头带 Connection: close, 每次新建连接、响应后立即关闭。
/// - maxConcurrentConnections: keepAlive=true 时生效, 每个端点
///   (scheme://host:port + sslVerify) 的最大并发连接数; 默认 5, 0 = 不限制
///   (仍复用空闲连接)。超过上限的并发请求排队等待空闲连接。
struct RequestConfig {
    std::chrono::milliseconds                connectTimeout           = std::chrono::seconds{30};
    std::optional<std::chrono::milliseconds> sendTimeout              = std::nullopt;
    std::chrono::milliseconds                readChunkTimeout         = std::chrono::seconds{30};
    std::optional<bool>                      sslVerify                = std::nullopt;
    size_t                                   followRedirect           = 3;
    bool                                     keepAlive                = false;
    size_t                                   maxConcurrentConnections = 5;
    uint64_t                                 maxResponseBody          = kDefaultMaxResponseBody;
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
    // 共享 SSL context 池 —— 避免每个请求都创建 OpenSSL context
    // (SSL_CTX_new 开销大)。两个惰性初始化的 context: 一个开启证书校验
    // (生产默认), 一个关闭。
    // -----------------------------------------------------------------------
    static asio::ssl::context& sharedSslCtx(bool verify);

    /// TLS 全版本自动协商: 把传入上下文的协议版本范围放宽为
    /// [TLS 1.0, 编译期支持的最高版本] (当前最高 1.3), 握手时由 OpenSSL
    /// 自动选取双方共有的最高版本。
    /// - SSLv2/SSLv3 不在放宽范围 (早已废弃/不安全, 且现代 OpenSSL 构建通常
    ///   已在编译期移除), 需配合 set_options(no_sslv2 | no_sslv3) 使用
    /// - 显式将安全等级设为 level 1: 部分 OpenSSL 构建/发行版默认等级更高,
    ///   其版本回调会直接禁用 < TLS 1.2 的版本, 导致 min_proto_version 放宽
    ///   无效; level 1 仅禁 SSLv3 及以下与低强度套件 (RSA<1024 等)
    /// - 必须在 set_options 之后调用 (set_options 内部经 native_handle 写
    ///   SSL_OP_* 标志, 与本函数的 min/max 版本设置互不覆盖, 但顺序统一
    ///   更易排查)
    static void enableTlsAutoNegotiate(asio::ssl::context& ctx);

private:

    struct ParsedUrl {
        std::string scheme;
        std::string host; // IPv6 字面量不含方括号 (如 "::1"), 便于直接用于 DNS 解析
        uint16_t    port;
        std::string path;
    };

    /// 按 HTTP 规范构造 Host 头: IPv6 字面量需重新加上方括号, 非默认端口附加 ":port"
    static std::string buildHostHeader(const ParsedUrl& parsed);

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
        // HEAD 响应按 RFC 7231 §4.3.2 只返回头部、不携带 body (但 Content-Length 保留),
        // 解析器不知道请求方法, 必须显式 skip, 否则会一直等待永远不会到达的 body 直到超时
        if (req.method() == http::verb::head) {
            parser.skip(true);
        }
        // 连接关闭但响应未解析完整时, async_read_some 会抛出 eof 错误 (由调用方捕获),
        // 不会返回截断的 body
        while (!parser.is_done()) {
            try {
                co_await http::async_read_some(
                    stream,
                    buffer,
                    parser,
                    asio::cancel_after(config.readChunkTimeout, asio::use_awaitable)
                );
            } catch (const neograph_asio_system_error& e) {
                // 服务器在响应完整发送前关闭了连接 (Content-Length 偏大 / chunked
                // 编码不完整 / 网关超时掐断), beast 报 partial_message。原始错误信息
                // 只含 "partial message [beast.http:2 ...]" 无任何请求上下文, 甚至
                // 无法区分是响应头还是 body 阶段失败, 这里转换为带状态与已收字节数
                // 的友好提示, 便于上层 (LLM/provider/MCP) 诊断与识别瞬时错误。
                // 注意: 该错误是"响应被截断", 不代表请求失败, 服务器可能已处理完
                // 请求, 仅响应在传输中丢失; 幂等/可容忍重复执行的请求可安全重试。
                if (e.code() == http::error::partial_message) {
                    // body().size() = 已接收字节数; Content-Length 头声明值用于
                    // 区分"服务器 Content-Length 写错"与"连接提前断开"
                    auto cl = parser.get()[http::field::content_length];
                    throw std::runtime_error(fmt::format(
                        "HTTP response truncated: server closed connection before sending "
                        "complete response (status {}, {} of {} bytes received)",
                        parser.get().result_int(),
                        parser.get().body().size(),
                        cl.empty() ? std::string{"?"} : std::string{cl}
                    ));
                }
                throw;
            }
        }

        auto         res = parser.release();
        HttpResponse resp;
        resp.status = res.result_int();
        // 服务端是否允许 keep-alive: 连接池据此决定响应完成后连接可否复用
        resp.keepAlive = res.keep_alive();
        for (auto const& field : res) {
            resp.headers.set(field.name_string(), field.value());
        }
        resp.body = std::move(res.body());
        co_return std::expected<HttpResponse, std::string>{std::move(resp)};
    }

    /// 判断错误消息是否属于"瞬时传输错误" (响应被截断 / 连接重置 / 对端关闭 /
    /// 超时等)。这类错误通常是网络抖动或服务器/网关行为导致, 重试一次大概率
    /// 成功; 但重试可能造成请求重复执行 (如 POST), 是否重试由调用方根据
    /// 幂等性决定, 本函数只负责分类, 不负责重试。
    /// - 匹配的字符串来自本模块 exchange 转换后的错误 ("truncated") 以及
    ///   asio/beast/OpenSSL 稳定的错误消息 (connection reset / end of file /
    ///   stream truncated / timeout 等)
    static bool isTransientError(std::string_view errmsg) noexcept;

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
    /// - onChunk 返回 true 表示流已结束 (如收到 [DONE]/response.completed/message_stop),
    ///   此时立即断开连接并停止读取, 避免对端 keep-alive 不关闭时白等 readChunkTimeout
    /// - HTTP 429 时抛出 neograph::RateLimitError (解析 retry-after)
    /// - 其他非 2xx 时抛出 std::runtime_error
    /// - 网络/超时错误抛出 neograph_asio_system_error
    static asio::awaitable<void> requestSseAsync(
        std::string_view                      method,
        std::string_view                      url,
        std::string_view                      body,
        std::string_view                      contentType,
        const HeaderMap&                      extraHeaders,
        const RequestConfig&                  config,
        std::function<bool(std::string_view)> onChunk
    );

    /// DNS 解析 (支持超时/取消)。
    /// getaddrinfo 阻塞调用无法被中断 (cancel_after / cancellation_signal 都只能
    /// 标记取消, 真正返回要等系统 DNS 超时), 因此解析放到后台协程执行, 本协程
    /// 在 connectDeadline 前轮询结果:
    /// - 超过 connectDeadline 抛 std::runtime_error (立即返回, 后台解析自行收尾)
    /// - 外部取消经 co_await 传播, 立即中止
    /// http / websocket 客户端统一使用, 避免黑洞 DNS 时请求/连接无限挂起。
    static asio::awaitable<asio::ip::tcp::resolver::results_type> asyncResolveWithDeadline(
        std::string_view                      host,
        std::string_view                      port,
        std::chrono::steady_clock::time_point connectDeadline,
        std::chrono::milliseconds             connectTimeout
    );

    /// 启用/禁用 SSL 证书校验 (默认启用)
    /// - 仅建议在自签名证书的测试场景中关闭
    static void setSslVerify(bool enable) noexcept;

    static bool getSslVerify() noexcept;

    // -----------------------------------------------------------------------
    // 连接池 (HTTP keep-alive 复用)
    // - 仅 RequestConfig.keepAlive=true 时启用; 池键 = scheme://host:port + sslVerify
    // - 空闲连接可跨请求复用 (跳过 DNS/TCP/TLS 建连), 复用失效连接时自动重试一次
    // - 同一端点的并发连接数受 RequestConfig.maxConcurrentConnections 限制 (默认 5)
    // - 池为进程级单例, 供测试/调试查询; 空闲连接绑定创建它的 io_context,
    //   该 io_context 存活期间可安全复用
    // -----------------------------------------------------------------------

    struct PoolStats {
        size_t active      = 0; ///< 当前借出的连接数
        size_t idle        = 0; ///< 当前空闲可复用的连接数
        size_t created     = 0; ///< 累计新建连接数
        size_t reused      = 0; ///< 累计复用连接数
        size_t peakActive  = 0; ///< 历史并发峰值 (验证并发上限用)
        size_t queuedWaits = 0; ///< 累计因并发上限排队等待的次数
    };

    /// 查询指定 URL 对应端点的连接池统计 (未使用过连接池时返回全 0)
    static PoolStats poolStats(std::string_view url);

    /// 关闭并清空所有空闲连接 (测试收尾/io_context 销毁前使用; 不影响借出的连接)
    static void clearConnectionPool();

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

    /// 带自定义请求头的 fetchMarkdown (如 web_fetch_url_markdown / web_search tool 的 header 参数)
    static asio::awaitable<std::expected<std::string, std::string>> fetchMarkdown(
        std::string_view     url,
        const HeaderMap&     extraHeaders,
        const RequestConfig& config = RequestConfig{
            .connectTimeout = std::chrono::seconds{15},
            .readChunkTimeout = std::chrono::seconds{15},
        }
    );
};
} // namespace util
} // namespace agentxx