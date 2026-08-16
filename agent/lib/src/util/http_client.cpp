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
#include <asio/execution/context.hpp>
#include <asio/execution_context.hpp>
#include <asio/steady_timer.hpp>
#include <map>
#include <neograph/provider.h>
#include <openssl/ssl.h>
#include <variant>
#include <vector>

namespace agentxx {
namespace util {

namespace {

/// 连接池存活标记 (平凡存储, 进程退出期间始终可安全读取):
/// HttpConnectionPool 是函数局部静态, 若它晚于某个 io_context 构造, 退出时就会
/// 先于该 io_context 销毁; 之后该 io_context 上的 HttpPoolContextGuard 析构时
/// 不能再调用 instance() (访问已销毁的对象是 UB), 需先检查本标记。
std::atomic<bool> g_poolAlive{false};

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

/// 对 URL 路径做词法规范化 (纯字符串操作, 不访问文件系统):
/// 去除 "." 段, 解析 ".." 段 (不越过根), 如 "/a/b/../c/./d" -> "/a/c/d",
/// "/a/../../b" -> "/b" (超出的 ".." 丢弃)
static std::string normalizeUrlPath(std::string_view path) {
    std::string         result;
    std::vector<size_t> segStarts; // 每段内容写入前 result.size() (含可能的前导 '/')
    result.reserve(path.size());
    size_t i = 0;
    while (i < path.size()) {
        // 跳过 '/'
        while (i < path.size() && path[i] == '/') {
            ++i;
        }
        if (i >= path.size()) {
            break;
        }
        size_t segEnd = path.find('/', i);
        if (segEnd == std::string_view::npos) {
            segEnd = path.size();
        }
        std::string_view seg = path.substr(i, segEnd - i);
        if (seg == ".") {
            // 当前目录段, 忽略
        } else if (seg == "..") {
            // 上级目录: 回退到上一个段的起始位置 (不越过根)
            if (!segStarts.empty()) {
                result.resize(segStarts.back());
                segStarts.pop_back();
            }
        } else {
            if (result.empty() || result.back() != '/') {
                result += '/';
            }
            segStarts.push_back(result.size());
            result.append(seg);
        }
        i = (segEnd == path.size()) ? segEnd : segEnd + 1;
    }
    if (result.empty()) {
        return "/";
    }
    return result;
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
        pollTimer.expires_after(std::chrono::milliseconds(100));
        co_await pollTimer.async_wait(asio::use_awaitable);
    }
    if (state->ec) {
        throw neograph_asio_system_error(state->ec);
    }
    co_return std::move(state->results);
}

// ---------------------------------------------------------------------------
// HTTP 连接池 (keep-alive 连接复用)
//
// 背景: 旧实现每次请求都新建 TCP+TLS 连接、响应后立即关闭, LLM API 调用频繁时
// 每次都要付出 DNS 解析 + TCP 握手 + TLS 握手 (100ms~1s+) 的建连开销。连接池
// 将空闲连接缓存复用, 同时把同一端点的并发连接数限制在可配置上限内。
//
// 设计要点:
// - 池键 = scheme://host:port + sslVerify (TLS 上下文不同不能互用)
// - 仅 RequestConfig.keepAlive=true 时启用; keepAlive=false 保持旧行为
//   (Connection: close, 每次新建/关闭, 不受并发上限约束)
// - 空闲连接超时 (kPoolIdleTimeout) 未复用视为可能已被服务端断开, 获取时丢弃
// - 复用失效连接 (服务端关闭了空闲 keep-alive 连接) 时自动用新连接重试一次,
//   仅限 eof/connection reset/broken pipe 等"请求未到达服务端"的错误
//   (见 isPoolRetryableError); 响应被截断/超时等不重试, 避免请求重复执行
// - 并发达到上限时轮询等待 (kPoolWaitPollInterval), 等待可被取消且不改池状态
// - 线程安全: 池状态用互斥锁保护; 连接的读写由借出的协程独占, 无并发访问
// - 连接绑定创建它的 io_context (socket 的 any_io_executor): 复用期间该
//   io_context 必须存活 (本项目 LLM 调用集中在 agent 的 io_context, 满足要求)
// ---------------------------------------------------------------------------

/// 连接池键: 同一端点 (协议+主机+端口+证书校验) 的连接可互相复用
struct HttpPoolKey {
    bool        https = false;
    std::string host; // IPv6 字面量不含方括号 (与 ParsedUrl::host 一致)
    uint16_t    port   = 0;
    bool        verify = true; // sslVerify 不同则 TLS 上下文不同, 不能复用

    bool operator==(const HttpPoolKey&) const = default;
};

struct HttpPoolKeyLess {
    bool operator()(const HttpPoolKey& a, const HttpPoolKey& b) const {
        if (a.https != b.https) {
            return a.https < b.https;
        }
        if (a.port != b.port) {
            return a.port < b.port;
        }
        if (a.verify != b.verify) {
            return a.verify < b.verify;
        }
        return a.host < b.host;
    }
};

/// 池化连接: 持有一个 TCP 或 TLS 流 (TLS 流内部持有 TCP socket)
struct PooledConnection {
    // asio 1.30+ 的 tcp::socket 无默认构造函数 (必须传 executor), 因此显式构造:
    // 默认按 TCP 构造 (std::in_place_index<0>), 创建 TLS 连接时再 emplace 替换
    explicit PooledConnection(asio::any_io_executor executor) :
        stream(std::in_place_index<0>, std::move(executor)) {}

    std::variant<asio::ip::tcp::socket, asio::ssl::stream<asio::ip::tcp::socket>> stream;
    std::chrono::steady_clock::time_point                                         lastUsed;
    bool fresh = true; ///< 本次是否新建 (非复用), 供失效重试决策
};

/// io_context 生命周期守卫: 连接池是进程级单例, 但池中连接绑定创建它的
/// io_context (socket 的 any_io_executor)。若某个 io_context 在池中连接销毁前先被
/// 销毁 (如测试/插件使用临时 io_context), 销毁 socket 会访问已销毁的 reactor,
/// 造成 use-after-free 崩溃。
///
/// 本服务注册到 io_context 上 (首次进入连接池时经 use_service 惰性注册): io_context
/// 析构时 asio 按"后注册先销毁"的顺序销毁服务, 本服务晚于 io_context 的内部服务
/// (scheduler/epoll_reactor) 注册, 因此本服务析构时 reactor 仍存活, 可以在析构中
/// 安全释放该 io_context 上的全部空闲连接。
class HttpConnectionPool;

class HttpPoolContextGuard : public asio::execution_context::service {
public:

    static asio::execution_context::id id;

    explicit HttpPoolContextGuard(asio::execution_context& owner) :
        service(owner) {}

    ~HttpPoolContextGuard() override;

    void shutdown() override {}
};

// 前置声明: HttpConnectionPool::acquire 在类内调用, 定义位于类之后
asio::awaitable<std::shared_ptr<PooledConnection>>
    createConnection(const HttpPoolKey& key, const RequestConfig& config);

/// 空闲连接最大存活时长: 超过该时长未复用的连接在下次获取时关闭重建。
/// 多数网关/负载均衡的 keep-alive 空闲超时在 60~75s, 120s 足够覆盖常见复用间隔;
/// 即使服务端提前断开, 复用失败也会自动用新连接重试一次 (见 isPoolRetryableError)。
inline constexpr auto kPoolIdleTimeout = std::chrono::seconds{120};

/// 并发上限等待时的轮询间隔: 等待是稀有的 (仅同一端点并发超过上限时), 50ms 粒度
/// 可接受; 相比等待队列, 轮询天然支持取消且无跨 executor 唤醒的复杂度
inline constexpr auto kPoolWaitPollInterval = std::chrono::milliseconds{50};

/// 复用失效重试判定: 仅当错误表明"连接在对端已被关闭/重置" (请求几乎未到达
/// 服务端) 时, 才允许用新连接重试一次。
/// - 优先按 asio/beast 错误码判定 (不依赖本地化错误消息, Windows 中文系统下
///   asio 错误消息为中文, 文本匹配不可靠)
/// - 匹配: eof / connection reset (Windows: WSAECONNRESET 10054) /
///   connection aborted (Windows: WSAECONNABORTED 10053, 对端关闭空闲连接后
///   读写时报此错) / broken pipe / beast end_of_stream
/// - 不匹配 (服务端已接收并处理请求, 重试会造成重复执行, 由上层决定):
///   响应截断 (partial message / ssl stream_truncated)、各类超时、取消
bool isPoolRetryableError(const neograph_asio_error_code& ec, std::string_view errmsg) {
    if (ec) {
        if (ec == asio::error::eof || ec == asio::error::connection_reset
            || ec == asio::error::connection_aborted || ec == asio::error::broken_pipe) {
            return true;
        }
        // beast: 连接在消息边界处关闭 (读下一个请求的响应时立即 EOF)
        if (ec == boost::beast::http::error::end_of_stream) {
            return true;
        }
    }
    // 兜底: 文本匹配 (非 asio 错误 / 被包装的错误消息)
    if (errmsg.empty()) {
        return false;
    }
    auto lower = agentxx::util::toLower(errmsg);
    if (lower.find("end of file") != std::string_view::npos
        || lower.find("end of stream") != std::string_view::npos) {
        return true;
    }
    if (lower.find("connection reset") != std::string_view::npos
        || lower.find("connection aborted") != std::string_view::npos
        || lower.find("broken pipe") != std::string_view::npos) {
        return true;
    }
    return false;
}

class HttpConnectionPool {
public:

    HttpConnectionPool() {
        g_poolAlive.store(true, std::memory_order_relaxed);
    }

    /// 析构时释放全部剩余连接: 池销毁发生在引用它的 io_context 之后 (构造逆序),
    /// 此时这些 io_context 仍存活, 销毁 socket 是安全的
    ~HttpConnectionPool() {
        g_poolAlive.store(false, std::memory_order_relaxed);
    }

    static HttpConnectionPool& instance() {
        static HttpConnectionPool pool;
        return pool;
    }

    /// 获取连接: 优先复用本 io_context 的空闲连接, 否则新建 (受 maxConcurrent 并发上限约束)。
    /// - 空闲连接绑定创建它的 io_context, 只能被同一 io_context 复用
    ///   (跨上下文复用 socket 是未定义行为), 因此空闲池按 io_context 分桶
    /// - maxConcurrent==0 表示不限制 (始终新建, 仍可复用空闲)
    /// - 空闲连接超过 kPoolIdleTimeout 未复用 (服务端很可能已断开) 时丢弃重建
    /// - 并发达到上限时轮询等待; 等待可被取消 (中止等待, 不修改池状态)
    /// - 新建连接失败/被取消时归还并发名额并抛出异常
    asio::awaitable<std::shared_ptr<PooledConnection>>
        acquire(const HttpPoolKey& key, size_t maxConcurrent, const RequestConfig& config) {
        auto               executor = co_await asio::this_coro::executor;
        asio::steady_timer pollTimer(executor);
        auto&              ctx = asio::query(executor, asio::execution::context);

        for (;;) {
            bool create = false;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                auto&                       entry = entries_[key];
                auto                        now   = std::chrono::steady_clock::now();
                // 优先复用本 io_context 的空闲连接; 丢弃已超时的空闲连接 (析构关闭 socket)
                auto& idle = entry.idleByCtx[&ctx];
                while (!idle.empty()) {
                    auto conn = std::move(idle.back());
                    idle.pop_back();
                    if (now - conn->lastUsed <= kPoolIdleTimeout) {
                        ++entry.active;
                        ++entry.reused;
                        entry.peakActive = std::max(entry.peakActive, entry.active);
                        conn->fresh      = false;
                        co_return conn;
                    }
                }
                if (maxConcurrent == 0 || entry.active < maxConcurrent) {
                    ++entry.active;
                    ++entry.created;
                    entry.peakActive = std::max(entry.peakActive, entry.active);
                    create           = true;
                } else {
                    ++entry.queuedWaits;
                }
            }
            if (create) {
                try {
                    auto conn   = co_await createConnection(key, config);
                    conn->fresh = true;
                    co_return conn;
                } catch (...) {
                    // 新建失败或外部取消: 归还并发名额后原样抛出
                    std::lock_guard<std::mutex> lock(mtx_);
                    if (entries_[key].active > 0) {
                        --entries_[key].active;
                    }
                    throw;
                }
            }
            // 并发达到上限: 轮询等待空闲连接/名额 (可取消)
            pollTimer.expires_after(kPoolWaitPollInterval);
            co_await pollTimer.async_wait(asio::use_awaitable);
        }
    }

    /// 归还连接: reusable=true 且底层连接仍打开时进入本 io_context 的空闲桶, 否则直接关闭
    void release(const HttpPoolKey& key, std::shared_ptr<PooledConnection> conn, bool reusable) {
        if (!conn) {
            return;
        }
        auto& ctx = std::visit(
            [](auto& s) -> asio::execution_context& {
                return asio::query(s.get_executor(), asio::execution::context);
            },
            conn->stream
        );
        std::lock_guard<std::mutex> lock(mtx_);
        auto&                       entry = entries_[key];
        if (entry.active > 0) {
            --entry.active;
        }
        if (reusable && isOpen(*conn)) {
            conn->lastUsed = std::chrono::steady_clock::now();
            entry.idleByCtx[&ctx].push_back(std::move(conn));
        }
    }

    struct Stats {
        size_t active      = 0;
        size_t idle        = 0;
        size_t created     = 0;
        size_t reused      = 0;
        size_t peakActive  = 0;
        size_t queuedWaits = 0;
    };

    Stats stats(const HttpPoolKey& key) const {
        std::lock_guard<std::mutex> lock(mtx_);
        auto                        it = entries_.find(key);
        if (it == entries_.end()) {
            return {};
        }
        const auto& entry = it->second;
        size_t      idle  = 0;
        for (const auto& [ctx, conns] : entry.idleByCtx) {
            idle += conns.size();
        }
        return Stats{
            entry.active,
            idle,
            entry.created,
            entry.reused,
            entry.peakActive,
            entry.queuedWaits,
        };
    }

    /// 关闭并清空全部空闲连接 (测试收尾/io_context 销毁前; 不影响借出的连接)
    void clear() {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& [key, entry] : entries_) {
            for (auto& [ctx, conns] : entry.idleByCtx) {
                conns.clear(); // 析构关闭 socket
            }
        }
    }

    /// io_context 销毁时由 HttpPoolContextGuard 回调: 释放该上下文上的全部空闲连接。
    /// 此时该 io_context 的 scheduler/reactor 等内部服务尚未销毁 (守卫后注册先销毁),
    /// 可安全析构这些 socket; 调用方持锁保证与 acquire/release/clear 互斥。
    void dropContext(asio::execution_context* ctx) {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& [key, entry] : entries_) {
            entry.idleByCtx.erase(ctx); // 析构关闭该上下文上的空闲 socket
        }
    }

private:

    struct Entry {
        // 空闲连接按 io_context 分桶: 连接只能被创建它的 io_context 复用,
        // 且该 io_context 销毁时 (经 HttpPoolContextGuard) 桶随上下文一起释放,
        // 避免池持有指向已销毁 io_context 的连接
        std::map<asio::execution_context*, std::vector<std::shared_ptr<PooledConnection>>>
               idleByCtx;
        size_t active      = 0;
        size_t created     = 0;
        size_t reused      = 0;
        size_t peakActive  = 0;
        size_t queuedWaits = 0;
    };

    mutable std::mutex                            mtx_;
    std::map<HttpPoolKey, Entry, HttpPoolKeyLess> entries_;

    static bool isOpen(const PooledConnection& conn) {
        return std::visit(
            [](const auto& s) {
                return s.lowest_layer().is_open();
            },
            conn.stream
        );
    }
};

asio::execution_context::id HttpPoolContextGuard::id;

HttpPoolContextGuard::~HttpPoolContextGuard() {
    // io_context 正在析构: 释放该上下文上的全部空闲连接。
    // asio 按后注册先销毁的顺序销毁服务, 本守卫晚于 reactor 注册, 此时 reactor 仍存活,
    // 析构 socket 是安全的。
    // 若连接池已先于本守卫销毁 (静态析构逆序: 池构造晚于本 io_context 时先销毁),
    // 池析构时已释放全部连接 (当时本 io_context 仍存活), 此处直接跳过, 不得再访问池。
    if (g_poolAlive.load(std::memory_order_relaxed)) {
        HttpConnectionPool::instance().dropContext(&context());
    }
}

/// 建立一条到池键对应端点的连接 (DNS + TCP + TLS), connectTimeout 为总时限。
/// 建连逻辑与原请求路径一致: 后台 DNS 解析 + TCP 连接 + no_delay/TCP keepalive
/// + TLS 握手 (SNI 仅域名)。返回的连接尚未用于任何请求 (fresh=true)。
asio::awaitable<std::shared_ptr<PooledConnection>>
    createConnection(const HttpPoolKey& key, const RequestConfig& config) {
    using asio::ip::tcp;

    auto executor = co_await asio::this_coro::executor;

    // connectTimeout 是 DNS + TCP + TLS 的总上限 (各阶段共用剩余时间)
    auto connectDeadline = std::chrono::steady_clock::now() + config.connectTimeout;
    auto remainingMs     = [&]() -> std::chrono::milliseconds {
        auto now = std::chrono::steady_clock::now();
        if (now >= connectDeadline) {
            return std::chrono::seconds{1};
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(connectDeadline - now);
    };

    // DNS 解析不能直接 co_await: getaddrinfo 阻塞无法被取消/超时中断,
    // 黑洞 DNS 时会无限挂起 (见 HttpClient::asyncResolveWithDeadline 注释)
    auto endpoints = co_await HttpClient::asyncResolveWithDeadline(
        key.host,
        std::to_string(key.port),
        connectDeadline,
        config.connectTimeout
    );

    auto conn = std::make_shared<PooledConnection>(executor);
    if (key.https) {
        auto& sslCtx = HttpClient::sharedSslCtx(key.verify);
        auto& stream = conn->stream.emplace<asio::ssl::stream<tcp::socket>>(executor, sslCtx);
        // SNI 只能是域名, IP 字面量 (IPv6 含 ':') 不支持 SNI
        if (!key.host.empty() && key.host.find(':') == std::string::npos) {
            ::SSL_set_tlsext_host_name(stream.native_handle(), key.host.c_str());
        }
        co_await asio::async_connect(
            stream.lowest_layer(),
            endpoints,
            asio::cancel_after(remainingMs(), asio::use_awaitable)
        );
        neograph_asio_error_code tcpEc;
        stream.lowest_layer().set_option(asio::ip::tcp::no_delay(true), tcpEc);
        enableTcpKeepalive(stream.lowest_layer());
        co_await stream.async_handshake(
            asio::ssl::stream_base::client,
            asio::cancel_after(remainingMs(), asio::use_awaitable)
        );
    } else {
        auto& stream = conn->stream.emplace<tcp::socket>(executor);
        co_await asio::async_connect(
            stream,
            endpoints,
            asio::cancel_after(remainingMs(), asio::use_awaitable)
        );
        neograph_asio_error_code tcpEc;
        stream.set_option(asio::ip::tcp::no_delay(true), tcpEc);
        enableTcpKeepalive(stream);
    }
    // 注册 io_context 生命周期守卫: 必须在 socket 创建之后 — io_context 按
    // "后注册先销毁"的顺序销毁服务, 守卫需晚于 reactor (socket 构造时惰性注册)
    // 注册, 才能在 io_context 销毁时先于 reactor 释放本上下文上的空闲连接。
    asio::use_service<HttpPoolContextGuard>(asio::query(executor, asio::execution::context));
    conn->lastUsed = std::chrono::steady_clock::now();
    conn->fresh    = true;
    co_return conn;
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

bool HttpClient::isTransientError(std::string_view errmsg) noexcept {
    if (errmsg.empty()) {
        return false;
    }
    // 大小写不敏感匹配: asio/beast/OpenSSL 的错误消息首字母可能大写
    // (如 "Connection reset by peer" / "End of file")
    auto lower = agentxx::util::toLower(errmsg);
    // 响应被截断: 服务器在完整响应前关闭连接。包括 exchange 转换后的
    // "HTTP response truncated: ..."、beast 原始错误 "partial message"
    // (如 "partial message [beast.http:2]") 与 OpenSSL 的 "stream truncated"
    // (对端未发 close_notify 就断开, 常见于网关/负载均衡超时掐断连接)
    if (lower.find("truncat") != std::string::npos
        || lower.find("partial message") != std::string::npos) {
        return true;
    }
    // 连接被对端重置 / 管道破裂 (写请求时对端已关闭)
    if (lower.find("connection reset") != std::string::npos
        || lower.find("broken pipe") != std::string::npos) {
        return true;
    }
    // 对端正常关闭连接但请求未完成 (如服务器主动断开 keep-alive 连接)
    if (lower.find("end of file") != std::string::npos) {
        return true;
    }
    // 超时: 连接/写请求/读响应任一时间窗口超时。catchErrorAsync 把
    // operation_aborted 包装为 "timeout: ..."; asio 原生消息为
    // "connect timed out" / "operation timed out" 等 ("timed out" 变体)
    if (lower.find("timeout") != std::string::npos
        || lower.find("timed out") != std::string::npos) {
        return true;
    }
    return false;
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
    // 相对 Location 可能含 "./"、"../" 段 (如 Location: ../../login),
    // 拼接后做 URL 词法规范化 (纯字符串操作, 不访问文件系统)
    return base + normalizeUrlPath(basePath + std::string(location));
}

const HeaderMap& HttpClient::defaultHeaders() {
    static const HeaderMap headers = [] {
        HeaderMap h;
        // User-Agent 遵循 RFC 9110: product = token ["/" product-version],
        // 用 Agentxx/<版本号> 标识本客户端身份, 便于服务端识别与统计
        h.set("User-Agent", "agentxx/1.0.0");
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
        if (cb + 1 < hostPort.size()) {
            // `]` 后必须是 ':' + 端口, 其余形式 (如 "[::1]xxx") 视为非法 URL
            if (hostPort[cb + 1] != ':') {
                return std::nullopt;
            }
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
                } else if (currentMethod == "POST" || currentMethod == "PUT"
                           || currentMethod == "PATCH") {
                    // 空 body 也必须携带 Content-Length: 0, 否则部分服务器/代理会一直等待
                    // body 数据或拒绝请求 (RFC 7230 §3.3.2)
                    if (!currentContentType.empty()) {
                        req.set(http::field::content_type, currentContentType);
                    }
                    req.prepare_payload();
                }

                // 连接池: keepAlive=true 时启用 (复用空闲连接 + 并发上限), 否则保持
                // 旧行为 (每次新建连接, 响应后关闭, 不受并发上限约束)
                bool   usePool  = config.keepAlive;
                size_t poolMax  = usePool ? config.maxConcurrentConnections : 0;
                int maxAttempts = usePool ? 2 : 1; // 复用失效连接时用新连接重试一次

                HttpPoolKey poolKey;
                poolKey.https = isHttps;
                poolKey.host  = parsed->host;
                poolKey.port  = parsed->port;
                poolKey.verify
                    = config.sslVerify.value_or(sslVerifyEnabled_.load(std::memory_order_relaxed));
                auto& pool = HttpConnectionPool::instance();

                std::shared_ptr<PooledConnection> conn;
                for (int attempt = 0; attempt < maxAttempts; ++attempt) {
                    // keepAlive=true: 经连接池获取 (复用空闲/并发上限);
                    // keepAlive=false: 直接新建 (旧行为), 不产生池统计
                    if (usePool) {
                        // 注意: 不能用 `usePool ? co_await acquire(...) : co_await
                        // createConnection(...)` 三元表达式 — GCC 16 对协程内 co_await
                        // 三元表达式存在代码生成缺陷, 会同时执行两个分支 (连接被重复创建/泄漏,
                        // 连接池统计错乱), 必须用 if/else
                        conn = co_await pool.acquire(poolKey, poolMax, config);
                    } else {
                        conn = co_await createConnection(poolKey, config);
                    }
                    bool reused   = usePool && !conn->fresh;
                    bool poolable = false;
                    try {
                        result = co_await std::visit(
                            [&](auto& stream
                            ) -> asio::awaitable<std::expected<HttpResponse, std::string>> {
                                co_return co_await exchange(stream, req, config);
                            },
                            conn->stream
                        );
                        // 服务端同意 keep-alive (响应无 Connection: close) 时连接可归还复用
                        poolable = usePool && result.has_value() && result->keepAlive;
                    } catch (const neograph_asio_system_error& e) {
                        // 复用的空闲连接已被服务端关闭 (eof/reset/broken pipe, 请求
                        // 几乎未到达服务端): 释放坏连接后用新连接重试一次; 其他错误
                        // (响应截断/超时/取消) 说明服务端可能已处理请求, 原样抛出
                        // 由上层决定是否重试, 避免重复执行
                        if (reused && attempt + 1 < maxAttempts
                            && isPoolRetryableError(e.code(), e.what())) {
                            pool.release(poolKey, std::move(conn), false);
                            continue;
                        }
                        // 归还/关闭连接, 避免 active 计数泄漏
                        if (usePool) {
                            pool.release(poolKey, std::move(conn), false);
                        }
                        throw;
                    } catch (...) {
                        // 非传输错误 (如响应截断转换的 runtime_error): 不重试, 关闭连接
                        if (usePool) {
                            pool.release(poolKey, std::move(conn), false);
                        }
                        throw;
                    }
                    // keepAlive=false 的 HTTPS 请求保持旧行为: 发送 close_notify 友好关闭
                    // (失败不影响已成功获取的响应; cancel_after 防止对端不响应时挂起)
                    if (!usePool && isHttps) {
                        auto& sslStream = std::get<asio::ssl::stream<tcp::socket>>(conn->stream);
                        neograph_asio_error_code sslEc;
                        co_await sslStream.async_shutdown(asio::cancel_after(
                            std::chrono::seconds{5},
                            asio::redirect_error(asio::use_awaitable, sslEc)
                        ));
                    }
                    if (usePool) {
                        pool.release(poolKey, std::move(conn), poolable);
                    }
                    break;
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

    // 连接池: keepAlive=true 时启用 (复用空闲连接 + 并发上限), 否则保持旧行为
    // (每次新建连接, 结束后关闭, 不受并发上限约束)
    bool   usePool     = config.keepAlive;
    size_t poolMax     = usePool ? config.maxConcurrentConnections : 0;
    int    maxAttempts = usePool ? 2 : 1; // 复用失效连接时用新连接重试一次

    HttpPoolKey poolKey;
    poolKey.https  = isHttps;
    poolKey.host   = parsed->host;
    poolKey.port   = parsed->port;
    poolKey.verify = config.sslVerify.value_or(sslVerifyEnabled_.load(std::memory_order_relaxed));
    auto& pool     = HttpConnectionPool::instance();

    auto sendTimeout = config.sendTimeout.value_or(calcTimeoutBySize(req.body().size()));

    // 执行 SSE 交换; 返回连接是否可归还池复用 (流完整结束且服务端允许 keep-alive)。
    // - anyDelivered: 是否已向 onChunk 投递过数据; 投递后连接层失败不再重试
    //   (重试会向调用方重复投递数据), 由上层决定
    auto doSseExchange = [&](auto& stream, bool& anyDelivered) -> asio::awaitable<bool> {
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
        bool   reusable  = false;

        // 返回 true 表示 onChunk 告知流已结束 (如 [DONE]), 应停止读取
        auto flushBody = [&]() -> bool {
            auto& respBody = parser.get().body();
            bool  stop     = false;
            if (respBody.size() > processed) {
                anyDelivered = true;
                stop         = onChunk(std::string_view{respBody}.substr(processed));
            }
            // 清空已读 body 并重置偏移: SSE 为长连接流, 若只移动偏移不裁剪,
            // string_body 会随流持续无限增长 → OOM
            respBody.clear();
            processed = 0;
            return stop;
        };

        // 流结束处理: 若流已完整解析 (chunked 终止块已到达, 连接处于消息边界) 且
        // 服务端允许 keep-alive, 连接可归还池复用; 否则必须主动断开 (避免对端
        // keep-alive 不关闭时白等 readChunkTimeout)
        auto stopReading = [&]() {
            if (parser.is_done()) {
                reusable = parser.get().keep_alive();
            } else {
                neograph_asio_error_code ignore;
                stream.lowest_layer().close(ignore);
            }
        };

        // 初始 flush: 响应头与首个 body 数据可能在同一数据块中到达
        if (flushBody()) {
            stopReading();
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
                    stopReading();
                    break;
                }
            }
            // 自然结束 (循环因 parser.is_done() 退出): 连接处于消息边界
            if (!reusable) {
                flushBody();
                if (parser.is_done()) {
                    reusable = parser.get().keep_alive();
                }
            }
        }
        co_return reusable;
    };

    // 建连 (DNS+TCP+TLS) 与交换统一走连接池
    std::shared_ptr<PooledConnection> conn;
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        // keepAlive=true: 经连接池获取 (复用空闲/并发上限);
        // keepAlive=false: 直接新建 (旧行为), 不产生池统计
        if (usePool) {
            // 注意: 不能用三元表达式包 co_await (GCC 16 协程代码生成缺陷, 见 requestAsync 注释)
            conn = co_await pool.acquire(poolKey, poolMax, config);
        } else {
            conn = co_await createConnection(poolKey, config);
        }
        bool reused       = usePool && !conn->fresh;
        bool anyDelivered = false;
        bool poolable     = false;
        try {
            poolable = co_await std::visit(
                [&](auto& stream) -> asio::awaitable<bool> {
                    co_return co_await doSseExchange(stream, anyDelivered);
                },
                conn->stream
            );
        } catch (const neograph_asio_system_error& e) {
            // 复用的空闲连接已被服务端关闭 (eof/reset/broken pipe) 且尚未投递任何
            // 数据 (请求未到达服务端): 释放坏连接后用新连接重试一次; 已投递过数据
            // 或响应被截断/超时等错误原样抛出, 避免重复投递/重复执行
            if (reused && attempt + 1 < maxAttempts && !anyDelivered
                && isPoolRetryableError(e.code(), e.what())) {
                pool.release(poolKey, std::move(conn), false);
                continue;
            }
            // 归还/关闭连接, 避免 active 计数泄漏
            if (usePool) {
                pool.release(poolKey, std::move(conn), false);
            }
            throw;
        } catch (...) {
            if (usePool) {
                pool.release(poolKey, std::move(conn), false);
            }
            throw;
        }
        if (!usePool && isHttps) {
            // keepAlive=false 保持旧行为: close_notify 友好关闭
            auto& sslStream = std::get<asio::ssl::stream<tcp::socket>>(conn->stream);
            neograph_asio_error_code sslEc;
            co_await sslStream.async_shutdown(asio::cancel_after(
                std::chrono::seconds{5},
                asio::redirect_error(asio::use_awaitable, sslEc)
            ));
        }
        if (usePool) {
            pool.release(poolKey, std::move(conn), usePool && poolable);
        }
        break;
    }
}

void HttpClient::setSslVerify(bool enable) noexcept {
    sslVerifyEnabled_.store(enable, std::memory_order_relaxed);
}

bool HttpClient::getSslVerify() noexcept {
    return sslVerifyEnabled_.load(std::memory_order_relaxed);
}

HttpClient::PoolStats HttpClient::poolStats(std::string_view url) {
    auto parsed = parseUrl(url);
    if (!parsed) {
        return {};
    }
    HttpPoolKey key;
    key.https  = parsed->scheme == "https";
    key.host   = parsed->host;
    key.port   = parsed->port;
    key.verify = sslVerifyEnabled_.load(std::memory_order_relaxed);
    auto s     = HttpConnectionPool::instance().stats(key);
    return PoolStats{s.active, s.idle, s.created, s.reused, s.peakActive, s.queuedWaits};
}

void HttpClient::clearConnectionPool() {
    HttpConnectionPool::instance().clear();
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
    co_return co_await fetchMarkdown(url, {}, config);
}

asio::awaitable<std::expected<std::string, std::string>> HttpClient::fetchMarkdown(
    std::string_view     url,
    const HeaderMap&     extraHeaders,
    const RequestConfig& config
) {
    auto resp = co_await getAsync(url, extraHeaders, config);
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
