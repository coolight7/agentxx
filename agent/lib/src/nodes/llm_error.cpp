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

/// 从错误文本里读 `retry-after` (支持 JSON 字段与 HTTP 头两种写法, 单位: 秒)
/// - 形如 `retry-after: 12` / `"retry_after": 12` / `Retry-After: 12` / `retry-after=12`
/// - 大小写不敏感; 键与数字之间允许 `"`/`:`/`=` 与空白
/// - 读不到或非法返回 0
int parseRetryAfterSeconds(std::string_view rawText) {
    const std::string text = toLowerCopy(rawText);
    const std::array<std::string_view, 3> keys{"retry-after", "retry_after", "retryafter"};
    for (const auto key : keys) {
        size_t pos = 0;
        while ((pos = text.find(key, pos)) != std::string_view::npos) {
            pos += key.size();
            // 跳过键与值之间的分隔符与空白 (`{"retry-after": 7}` 中间有引号与冒号)
            while (pos < text.size()
                   && (text[pos] == ':' || text[pos] == '=' || text[pos] == '"'
                       || text[pos] == '\'' || text[pos] == ' '
                       || text[pos] == '\t')) {
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

} // namespace

LlmErrorKind classifyLlmError(std::string_view errInfo) noexcept {
    if (errInfo.empty()) {
        return LlmErrorKind::Unknown;
    }
    const std::string text = toLowerCopy(errInfo);

    // 1) 上下文超限 (必须先判: 400/413 都可能带这些关键词, 它们是可恢复的)
    if (containsAny(
            text,
            {"context_length_exceeded",
             "maximum context length",
             "context window",
             "too many tokens",
             "prompt is too long",
             "reduce the length",
             "exceeds the maximum",
             "413",
             "request entity too large",
             "请求过长",
             "上下文过长"}
        )) {
        return LlmErrorKind::ContextOverflow;
    }

    // 2) 网络/传输层故障 (可重试)
    // 排在状态码类之前: 这类文本常夹带地址与端口数字 (`https://host:10403 connect failed`
    // 含 "403"), 若先判鉴权/额度/请求非法, 网络故障会被误判成"重试也没用"而直接停止
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
                "end of file",
                "eof",
                "asio.ssl",
                "asio.netdb",
                "network",
                "unreachable",
                "no route to host",
                "transport error",
                "i/o error",
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
                // TLS 握手
                "handshake",
                "ssl error",
                "tls error",
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
            }
        )) {
        return LlmErrorKind::Network;
    }

    // 3) 鉴权 (重试无意义)
    if (containsAny(
            text,
            {"401",
             "403",
             "unauthorized",
             "authentication",
             "invalid api key",
             "invalid_api_key",
             "api key is invalid",
             "鉴权",
             "密钥无效"}
        )) {
        return LlmErrorKind::Auth;
    }

    // 4) 额度/计费 (重试无意义)
    if (containsAny(
            text,
            {"402",
             "insufficient_quota",
             "quota exceeded",
             "quota has been exceeded",
             "out of credit",
             "billing",
             "insufficient balance",
             "余额不足",
             "额度不足",
             "已耗尽"}
        )) {
        return LlmErrorKind::Quota;
    }

    // 5) 请求非法 (重试无意义; 模型名/参数写错等)
    if (containsAny(
            text,
            {"400",
             "invalid_request_error",
             "invalid request",
             "bad request",
             "unsupported parameter",
             "unknown parameter",
             "malformed",
             "参数错误",
             "请求非法"}
        )) {
        return LlmErrorKind::InvalidRequest;
    }

    // 6) 限流 (可重试; 使用更大的基数并优先采信 retry-after)
    if (containsAny(
            text,
            {"429",
             "rate limit",
             "rate_limit",
             "too many requests",
             "slow down",
             "速率限制",
             "限速",
             "请求频率"}
        )) {
        return LlmErrorKind::RateLimit;
    }

    // 7) 超时 (可重试; 连接层的超时已在网络类里判过)
    if (containsAny(
            text,
            {"timeout",
             "timed out",
             "超时"}
        )) {
        return LlmErrorKind::Timeout;
    }

    // 8) 服务端错误 (可重试)
    if (containsAny(
            text,
            {"500",
             "502",
             "503",
             "504",
             "internal server error",
             "bad gateway",
             "service unavailable",
             "gateway timeout",
             "overloaded",
             "server error",
             "服务不可用"}
        )) {
        return LlmErrorKind::Server;
    }

    return LlmErrorKind::Unknown;
}

bool isLlmErrorRetryable(LlmErrorKind kind) noexcept {
    switch (kind) {
        case LlmErrorKind::Auth:
        case LlmErrorKind::Quota:
        case LlmErrorKind::InvalidRequest:
            return false;
        default:
            return true;
    }
}

bool isLlmContextOverflow(LlmErrorKind kind) noexcept {
    return kind == LlmErrorKind::ContextOverflow;
}

std::string_view llmErrorKindText(LlmErrorKind kind) noexcept {
    switch (kind) {
        case LlmErrorKind::Auth:
            return "authentication failed";
        case LlmErrorKind::Quota:
            return "quota or billing problem";
        case LlmErrorKind::InvalidRequest:
            return "invalid request";
        case LlmErrorKind::ContextOverflow:
            return "context length exceeded";
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

    // 基数按类别区分: 只影响"等多久", 不影响"是否重试"
    // - 限流 5 秒: 上游明确要求放慢, 等最久 (通常还会给出 retry-after, 优先采用)
    // - 服务端错误 3 秒: 上游自身出问题, 给一点恢复时间
    // - 超时/未知/上下文超限 2 秒: 与历史行为一致
    // - 网络故障 1 秒: 多为瞬时 (连接被掐、路由抖动、域名解析失败), 尽快重来
    // - 不重试的三类不会走到这里, 与"未知"同基数只为避免出现 0 秒
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
        case LlmErrorKind::ContextOverflow:
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
