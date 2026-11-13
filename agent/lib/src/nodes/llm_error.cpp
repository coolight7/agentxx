#include "agentxx/nodes/llm_error.h"

#include "utilxx_base/string_util.h"
#include <algorithm>
#include <array>
#include <cctype>

namespace agentxx {
namespace nodes {

namespace {

/// 小写化 (英文关键词大小写不敏感; 中文不受影响)
std::string toLowerCopy(std::string_view text) {
    std::string out{text};
    utilxx_base::toLowerSelf(out);
    return out;
}

/// 文本是否包含任一片段 (调用方保证 text 已小写, 片段本身按小写书写)
bool containsAny(std::string_view text, std::initializer_list<std::string_view> needles) {
    for (const auto needle : needles) {
        if (text.find(needle) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

/// ASCII 字母/数字 (状态码两边的边界判定; 不随 locale 变化)
bool isAsciiAlnum(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/// 文本里是否有一个独立出现的状态码 (两边都不是 ASCII 字母/数字)
///
/// 裸子串匹配会把流式失败诊断里的字节数当成状态码: `recv=403684B` 含 `403`,
/// 于是"可重试的读超时"被判成"鉴权失败"直接结束本轮 (实测偶现)。这里要求代码
/// 独立成词 —— `HTTP 401` / `(402)` / `code=429` / `status: 503` 命中,
/// `403684` / `14011` 不命中。
bool containsStatusCode(
    std::string_view                        text,
    std::initializer_list<std::string_view> codes
) {
    for (const auto code : codes) {
        size_t pos = 0;
        while ((pos = text.find(code, pos)) != std::string_view::npos) {
            const size_t end   = pos + code.size();
            const bool   left  = pos == 0 || !isAsciiAlnum(text[pos - 1]);
            const bool   right = end >= text.size() || !isAsciiAlnum(text[end]);
            if (left && right) {
                return true;
            }
            ++pos;
        }
    }
    return false;
}

/// 从错误文本里读 `retry-after` (支持 JSON 字段与 HTTP 头两种写法, 单位: 秒)
/// - 形如 `retry-after: 12` / `"retry_after": 12` / `Retry-After: 12` / `retry-after=12`
/// - 大小写不敏感; 键与数字之间允许 `"`/`:`/`=` 与空白
/// - 读不到或非法返回 0
int parseRetryAfterSeconds(std::string_view rawText) {
    const std::string                     text = toLowerCopy(rawText);
    const std::array<std::string_view, 3> keys{"retry-after", "retry_after", "retryafter"};
    for (const auto key : keys) {
        size_t pos = 0;
        while ((pos = text.find(key, pos)) != std::string_view::npos) {
            pos += key.size();
            // 跳过键与值之间的分隔符与空白 (`{"retry-after": 7}` 中间有引号与冒号)
            while (pos < text.size()
                   && (text[pos] == ':' || text[pos] == '=' || text[pos] == '"' || text[pos] == '\''
                       || text[pos] == ' ' || text[pos] == '\t')) {
                ++pos;
            }
            int  value    = 0;
            bool hasDigit = false;
            while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
                hasDigit = true;
                value    = value * 10 + (text[pos] - '0');
                if (value > kLlmRetryMaxDelaySeconds * 10) {
                    // 防溢出: 超出上限一个数量级即夹住, 后续统一夹到上限
                    value = kLlmRetryMaxDelaySeconds * 10;
                    break;
                }
                ++pos;
            }
            if (hasDigit) {
                return value;
            }
        }
    }
    return 0;
}

/// 抖动 (0~2 秒, 由错误文本哈希决定: 同一错误每次重试抖动相同, 便于复现)
int jitterOf(std::string_view errInfo) noexcept {
    size_t h = 1469598103934665603ULL; // FNV-1a 64 位
    for (const char c : errInfo) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;
    }
    return static_cast<int>(h % 3);
}

/// 我们自己拼在错误文本末尾的流式诊断后缀的标记 (见 provider_common.h)
///
/// 它只会在"响应流没走完"时出现, 因此这个标记本身就是网络层失败的硬指标。
constexpr std::string_view kStreamSuffixMarker = " | stream: ";

/// 文本是否表示网络层面的失败 (**入参已小写**)
///
/// 判定规则: 只要失败发生在网络这一层就算 —— 连接/读写、域名解析、TLS 握手与证书、
/// 代理与隧道、HTTP/2 流控制、平台 socket 错误码, 以及响应流没走完 (我们的
/// ` | stream: ...` 诊断后缀)。
///
/// 判这一类的意义: 这些失败与"请求本身对不对"无关, 退避应当最短 (1 秒) 以便尽快
/// 重来; 因此 [classifyLlmError] 把它排在**所有状态码/鉴权/额度规则之前**, 文本里同时
/// 出现的 `401` / `403` / `400` 等状态码与鉴权字样只当噪声。
bool networkFailureIn(std::string_view text) noexcept {
    // 响应流没走完 (我们自己拼的后缀): 请求已经发出、上游可能已经回了一半
    if (text.find(kStreamSuffixMarker) != std::string_view::npos) {
        return true;
    }
    if (containsAny(
            text,
            {
                // 连接层 (boost asio 诊断信息 / 系统 socket 错误)
                "connection reset",
                "connection closed",
                "connection refused",
                "connection aborted",
                "connection terminated",
                "connection failed",
                "connection error",
                "connection timed out",
                "connection timeout",
                "connection dropped",
                "connection lost",
                "cannot connect",
                "could not connect",
                "couldn't connect",
                "failed to connect",
                "unable to connect",
                "connect failed",
                "connect error",
                "connect timeout",
                "broken pipe",
                "stream truncated",
                "stream closed",
                "stream error",
                "stream reset",
                "end of file",
                "unexpected eof",
                "premature",
                "incomplete message",
                "incomplete response",
                "partial response",
                "bad chunk",
                "chunked encoding",
                "socket",
                "econnreset",
                "econnaborted",
                "etimedout",
                "eai_again",
                "epipe",
                "eof",
                "asio.ssl",
                "asio.netdb",
                "network",
                "unreachable",
                "no route to host",
                "transport error",
                "i/o error",
                "input/output error",
                "read error",
                "write error",
                "recv error",
                "send error",
                "recv failed",
                "send failed",
                "remote closed",
                "closed by peer",
                "peer closed",
                // 域名解析
                "name resolution",
                "no such host",
                "host not found",
                "could not resolve",
                "couldn't resolve",
                "resolve host",
                "resolve the host",
                "getaddrinfo",
                "nodename nor servname",
                // TLS / 证书 (网络这一层的问题: 握手没成、证书链/有效期/主机名对不上,
                // 也可能是代理或时间偏移导致的, 一律先重试)
                "handshake",
                "ssl",
                "tls",
                "certificate",
                "cert verify",
                "self signed",
                "self-signed",
                "unknown ca",
                "wrong version number",
                "no shared cipher",
                "unsupported protocol",
                "hostname mismatch",
                // 代理 / 隧道 / HTTP 层连接性
                "proxy",
                "tunnel",
                "goaway",
                "rst_stream",
                "http2",
                "protocol error",
                "compression_error",
                // 中文系统文案 (Windows socket / .NET 常见说法)
                "网络",
                "无法连接",
                "无法解析",
                "连接被拒绝",
                "连接被重置",
                "连接重置",
                "连接被关闭",
                "连接中断",
                "连接超时",
                "连接失败",
                "连接异常",
                "连接尝试失败",
                "基础连接已经关闭",
                "意外关闭",
                "远程主机强迫关闭",
                "这样的主机",
                "积极拒绝",
                "没有反应",
                "已中止 i/o",
                "握手失败",
                "证书",
                "代理",
            }
        )) {
        return true;
    }
    // 平台 socket 错误码 (WSA / errno): 按独立数字判, 免得 `recv=110054B` 这类字节数命中
    return containsStatusCode(
        text,
        {"10050", "10051", "10052", "10053", "10054", "10055", "10060", "10061", "10065"}
    );
}

} // namespace

LlmErrorKind classifyLlmError(std::string_view errInfo) noexcept {
    if (errInfo.empty()) {
        return LlmErrorKind::Unknown;
    }
    std::string text = toLowerCopy(errInfo);

    // 1) 网络层失败 (最先判: 退避基数最短, 1 秒)
    // - 判定规则见 [networkFailureIn]: 连接/读写、域名解析、TLS 与证书、代理与隧道、
    //   HTTP/2 流控、平台 socket 错误码、响应流没走完 —— 都属于网络这一层的问题
    // - 必须在切诊断后缀**之前**判: 后缀本身就是"响应流没走完"的硬指标
    // - 排在最前面: 这类文本常夹带地址与端口数字 (`https://host:10403 connect failed`
    //   含 "403"), 也会夹带上游回的半个响应 (一个没读完的 401/400 正文)
    if (networkFailureIn(text)) {
        return LlmErrorKind::Network;
    }

    // 我们自己拼在末尾的流式诊断后缀 (` | stream: recv=...B events=... idle=...ms
    // finish=... tools=... args=...B name=...`, 见 provider_common.h) 只用于展示与排查,
    // 剩下的规则不该看它: 里面的字节数与上游原文都会带数字
    if (const auto cut = text.find(kStreamSuffixMarker); cut != std::string::npos) {
        text.resize(cut);
    }

    // 2) 鉴权 (退避基数 2 秒)
    if (containsAny(
            text,
            {"unauthorized",
             "authentication",
             "invalid api key",
             "invalid_api_key",
             "api key is invalid",
             "鉴权",
             "密钥无效"}
        )
        || containsStatusCode(text, {"401", "403"})) {
        return LlmErrorKind::Auth;
    }

    // 3) 额度/计费 (退避基数 2 秒)
    if (containsAny(
            text,
            {"insufficient_quota",
             "quota exceeded",
             "quota has been exceeded",
             "out of credit",
             "billing",
             "insufficient balance",
             "余额不足",
             "额度不足",
             "已耗尽"}
        )
        || containsStatusCode(text, {"402"})) {
        return LlmErrorKind::Quota;
    }

    // 4) 请求非法 (退避基数 2 秒; 模型名/参数写错等)
    if (containsAny(
            text,
            {"invalid_request_error",
             "invalid request",
             "bad request",
             "unsupported parameter",
             "unknown parameter",
             "malformed",
             "参数错误",
             "请求非法"}
        )
        || containsStatusCode(text, {"400"})) {
        return LlmErrorKind::InvalidRequest;
    }

    // 5) 限流 (使用更大的基数并优先采信 retry-after)
    if (containsAny(
            text,
            {"rate limit",
             "rate_limit",
             "too many requests",
             "slow down",
             "速率限制",
             "限速",
             "请求频率"}
        )
        || containsStatusCode(text, {"429"})) {
        return LlmErrorKind::RateLimit;
    }

    // 6) 超时 (连接层与流式读超时已在网络类里判过)
    if (containsAny(text, {"timeout", "timed out", "超时"})) {
        return LlmErrorKind::Timeout;
    }

    // 7) 服务端错误
    if (containsAny(
            text,
            {"internal server error",
             "bad gateway",
             "service unavailable",
             "gateway timeout",
             "overloaded",
             "server error",
             "服务不可用"}
        )
        || containsStatusCode(text, {"500", "502", "503", "504"})) {
        return LlmErrorKind::Server;
    }

    return LlmErrorKind::Unknown;
}

std::string_view llmErrorKindText(LlmErrorKind kind) noexcept {
    switch (kind) {
        case LlmErrorKind::Auth:
            return "authentication failed";
        case LlmErrorKind::Quota:
            return "quota or billing problem";
        case LlmErrorKind::InvalidRequest:
            return "invalid request";
        case LlmErrorKind::RateLimit:
            return "rate limited";
        case LlmErrorKind::Timeout:
            return "timeout";
        case LlmErrorKind::Network:
            return "network or transport error";
        case LlmErrorKind::Server:
            return "server error";
        case LlmErrorKind::Unknown:
            return "unknown error";
    }
    return "unknown error";
}

int llmRetryDelaySeconds(LlmErrorKind kind, size_t attempt, std::string_view errInfo) noexcept {
    // 响应里明确给出 retry-after 时优先采用 (服务端最了解何时可以再来)
    if (const int retryAfter = parseRetryAfterSeconds(errInfo); retryAfter > 0) {
        return std::min(retryAfter, kLlmRetryMaxDelaySeconds);
    }

    // 基数按类别区分 —— **类别只决定等多久, 不决定是否重试** (任何错误都会重试,
    // 上限由 `llm.max_retry` 控制):
    // - 限流 5 秒: 上游明确要求放慢, 等最久 (通常还会给出 retry-after, 优先采用)
    // - 服务端错误 3 秒: 上游自身出问题, 给一点恢复时间
    // - 鉴权/额度/请求非法 2 秒: 大多重试也过不了, 但仍会重试 (上游的 401/400/402
    //   也可能来自代理、网关或半个响应), 与"未知"同基数
    // - 超时/未知 2 秒: 与历史行为一致
    // - 网络故障 1 秒: 多为瞬时 (连接被掐、路由抖动、域名解析失败), 尽快重来
    int base = 2;
    switch (kind) {
        case LlmErrorKind::Network:
            base = 1;
            break;
        case LlmErrorKind::Server:
            base = 3;
            break;
        case LlmErrorKind::RateLimit:
            base = 5;
            break;
        case LlmErrorKind::Unknown:
        case LlmErrorKind::Timeout:
        case LlmErrorKind::Auth:
        case LlmErrorKind::Quota:
        case LlmErrorKind::InvalidRequest:
            base = 2;
            break;
    }
    int delay = base;
    for (size_t i = 1; i < attempt && delay < kLlmRetryMaxDelaySeconds; ++i) {
        delay *= 2;
    }
    delay += jitterOf(errInfo);
    return std::clamp(delay, 1, kLlmRetryMaxDelaySeconds);
}

} // namespace nodes
} // namespace agentxx
