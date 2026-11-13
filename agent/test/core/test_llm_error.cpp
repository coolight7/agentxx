/// test_llm_error —— LLM 错误分类与重试策略
///
/// 覆盖:
/// - classifyLlmError: 各家 provider 的常见错误文本落到正确分类
///   (鉴权 / 额度 / 请求非法 / 限流 / 超时 / 网络 / 服务端 / 未知)
/// - 分类只用于决定退避: **任何错误都会重试** (重试次数只由 `llm.max_retry` 控制),
///   所以本模块只断言分类与退避, 不再有"这类不值得重试"的判定
/// - 不识别"上下文超限": 413 / `context length` 一类文本不做特殊判定 (见 llm_error.h
///   的警告), 只落到普通分类并照常重试
/// - llmRetryDelaySeconds: 有界指数退避、类别只影响等待基数 (网络最短、限流最长)、
///   `retry-after` 优先、上限夹取、抖动不会让等待变成 0
#include "agentxx-test/core/test_llm_error.h"

#include "agentxx/nodes/llm_error.h"
#include <string>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_le_passed = 0;
int g_le_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_le_passed
#define XX_TEST_FAILED g_le_failed

namespace agentxx {
namespace test {

using namespace agentxx::nodes;

TestResult testLlmError() {
    g_le_passed = 0;
    g_le_failed = 0;

    // ---------------- 分类: 鉴权 / 额度 / 请求非法 (退避基数 2 秒) ----------------

    {
        const char* authCases[] = {
            R"(HTTP 401 Unauthorized)",
            R"({"error":{"type":"authentication_error","message":"Invalid API key"}})",
            "403 Forbidden",
        };
        for (const auto* text : authCases) {
            const auto kind = classifyLlmError(text);
            XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::Auth);
        }

        const char* quotaCases[] = {
            R"({"error":{"code":"insufficient_quota","message":"You exceeded your current quota"}})",
            "HTTP 402 Payment Required",
            "账户余额不足，请充值",
        };
        for (const auto* text : quotaCases) {
            const auto kind = classifyLlmError(text);
            XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::Quota);
        }

        const char* invalidCases[] = {
            R"({"error":{"type":"invalid_request_error","message":"unknown parameter: foo"}})",
            "HTTP 400 Bad Request",
        };
        for (const auto* text : invalidCases) {
            const auto kind = classifyLlmError(text);
            XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::InvalidRequest);
        }
    }

    // ---------------- 分类: 限流 / 超时 / 网络 / 服务端 ----------------

    {
        const char* rateCases[] = {
            R"(HTTP 429 Too Many Requests)",
            R"({"error":{"message":"rate limit reached"}})",
            "请求频率过高，已限速",
        };
        for (const auto* text : rateCases) {
            const auto kind = classifyLlmError(text);
            XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::RateLimit);
        }

        const char* timeoutCases[] = {
            "read timeout after 60s",
            "SSE read-header timeout after 30s",
            "操作超时",
        };
        for (const auto* text : timeoutCases) {
            const auto kind = classifyLlmError(text);
            XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::Timeout);
        }

        // 真实观测到的文本: 流式读超时的诊断后缀里带收到的字节数, 里面恰好出现
        // `403` / `400` 三位数。裸子串匹配会把它们当成鉴权/请求非法, 于是
        // "可重试的读超时"第一次就结束本轮 (用户看到 "failed after 1 attempt(s)")
        const char* const streamSuffixCases[] = {
            "SSE read-body timeout after 60000 | stream: recv=403684B events=565 "
            "idle=60013ms finish=none tools=0 args=0B name=",
            "SSE read-body timeout after 60000 | stream: recv=400123B events=1 "
            "idle=60001ms finish=none tools=0 args=0B name=",
            "SSE read-body timeout after 60000 | stream: recv=402001B events=2 "
            "idle=60002ms finish=none tools=0 args=0B name=",
            "SSE read-body timeout after 60000 | stream: recv=413777B events=3 "
            "idle=60003ms finish=none tools=0 args=4096B name=x",
            "SSE read-body timeout after 60000 | stream: recv=429001B events=4 "
            "idle=60004ms finish=none tools=0 args=0B name=",
            "SSE read-body timeout after 60000 | stream: recv=500999B events=5 "
            "idle=60005ms finish=none tools=0 args=0B name=",
        };
        for (const auto* text : streamSuffixCases) {
            const auto kind = classifyLlmError(text);
            // 带自己的诊断后缀 = 响应流没走完 = 网络层失败, 判网络 (不按正文里的
            // "timeout" 判超时); 网络类拿最短的退避基数 (1 秒)
            // 后缀里的字节数 (`413777B` / `403684B`) 也不参与其余判定
            XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::Network);
        }

        // 状态码独立成词时仍按状态码判 (括号 / 冒号 / 等号 / 空格分隔)
        struct CodeCase {
            const char*                  text;
            agentxx::nodes::LlmErrorKind want;
        };
        const CodeCase codeCases[] = {
            {"http 401 unauthorized",                        LlmErrorKind::Auth         },
            {"http status 403 forbidden",                      LlmErrorKind::Auth         },
            {"请求失败 (402) 余额不足",                 LlmErrorKind::Quota        },
            {"http 400 bad request",                         LlmErrorKind::InvalidRequest},
            {"http 429 too many requests",                   LlmErrorKind::RateLimit    },
            {"http 503 service unavailable",                 LlmErrorKind::Server       },
            // 413 不再单独识别 (不做"上下文超限"判定): 落到 Unknown, 同样会重试
            {"http 413 payload too large",                   LlmErrorKind::Unknown      },
        };
        for (const auto& c : codeCases) {
            XX_TEST_EXPECT_TRUE(classifyLlmError(c.text) == c.want);
        }

        // 没有诊断后缀标记时也要挡住字节数误判 (只有独立成词的状态码才算)
        const char* const bareByteCountCases[] = {
            "SSE read-body timeout after 60000 recv=403684B events=565 idle=60013ms",
            "SSE read-body timeout after 60000 recv=400123B events=1 idle=60001ms",
            "SSE read-body timeout after 60000 recv=413777B events=2 idle=60002ms",
            "http status=5021 upstream",
        };
        for (const auto* text : bareByteCountCases) {
            const auto kind = classifyLlmError(text);
            XX_TEST_EXPECT_TRUE(
                kind == LlmErrorKind::Timeout || kind == LlmErrorKind::Unknown
            );
        }

        // 网络/传输层故障: 真实观测到的文本 (boost asio 诊断信息 + Windows socket 中文文案)
        const char* networkCases[] = {
            "stream truncated [asio.ssl.stream:1]",
            "connection reset by peer",
            "SSE stream truncated: missing [DONE] marker | model=gpt-4o content_chars=128",
            "asio.netdb:2 Host not found (authoritative)",
            "asio.misc:2 End of file",
            "write: broken pipe",
            "SSL handshake failed",
            "由于目标计算机积极拒绝，无法连接。 (10061)",
            "不知道这样的主机。",
            "远程主机强迫关闭了一个现有的连接。",
            "由于线程退出或应用程序请求，已中止 I/O 操作。",
            "连接尝试失败，因为连接方在一段时间后没有正确答复或连接的主机没有反应",
            "无法解析服务器的名称或地址",
            // 端口号里含 "403": 先判网络才不会被鉴权规则抢走 (旧行为下不重试)
            "https://api.example.com:10403 connect failed",
        };
        for (const auto* text : networkCases) {
            const auto kind = classifyLlmError(text);
            XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::Network);
        }

        // TLS / 证书 / 代理 / HTTP-2 / 平台 socket 错误码: 同属网络这一层, 一律重试
        const char* const moreNetworkCases[] = {
            "SSL certificate verify failed",
            "certificate has expired",
            "tlsv1 alert unknown ca",
            "error:0A00010B:SSL routines::wrong version number",
            "no shared cipher",
            "hostname mismatch in certificate",
            "CONNECT tunnel failed, response 502",
            "proxy connect error: 407 proxy authentication required",
            "http2 GOAWAY received (last stream id 13)",
            "RST_STREAM with error code 2",
            "wsarecv: 10054",
            "recv failed, error 10053",
            "send error: 10060",
            "epipe while writing body",
            "unexpected eof while reading response",
            "premature end of the response stream",
            "chunked encoding error: bad chunk",
            "socket read error",
        };
        for (const auto* text : moreNetworkCases) {
            const auto kind = classifyLlmError(text);
            XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::Network);
        }

        // 网络层的判定优先于状态码 / 鉴权 / 额度 / 请求非法:
        // 文本里同时出现它们时仍判网络 (退避最短)
        const char* const networkWinsCases[] = {
            "HTTP 401 Unauthorized | stream: recv=1024B events=1 idle=60000ms finish=none",
            "HTTP 403 Forbidden: connection reset by peer",
            "HTTP 400 Bad Request (tls handshake failed)",
            "insufficient_quota: proxy connect error",
            "invalid_request_error: socket read error",
            "413 Request Entity Too Large: connection closed before body",
            "401 unauthorized; getaddrinfo failed for api.example.com",
        };
        for (const auto* text : networkWinsCases) {
            const auto kind = classifyLlmError(text);
            XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::Network);
        }

        // 字节数里出现 socket 错误码的片段: 不算代码 (独立成词才算)
        {
            const auto kind = classifyLlmError("recv=110054B events=9 idle=1200ms");
            XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::Unknown || kind == LlmErrorKind::Timeout);
        }

    }

    // ---------------- 退避策略 ----------------

    {
        // 指数增长且不超过上限 (服务端错误基数 3 秒 + 抖动 0~2 秒)
        const int first  = llmRetryDelaySeconds(LlmErrorKind::Server, 1, "500");
        const int second = llmRetryDelaySeconds(LlmErrorKind::Server, 2, "500");
        const int tenth  = llmRetryDelaySeconds(LlmErrorKind::Server, 10, "500");
        XX_TEST_EXPECT_TRUE(first >= 3 && first <= 5);
        XX_TEST_EXPECT_TRUE(second >= first);
        XX_TEST_EXPECT_TRUE(tenth <= kLlmRetryMaxDelaySeconds);
        XX_TEST_EXPECT_TRUE(llmRetryDelaySeconds(LlmErrorKind::Unknown, 1, "x") >= 1);

        // 类别只决定等多久: 网络 < 超时(=未知) < 服务端 < 限流
        // (抖动由错误文本决定, 用同一段文本才能直接比较基数)
        const char* const seed    = "retry base ordering";
        const int         netSec  = llmRetryDelaySeconds(LlmErrorKind::Network, 1, seed);
        const int         timeSec = llmRetryDelaySeconds(LlmErrorKind::Timeout, 1, seed);
        const int         unkSec  = llmRetryDelaySeconds(LlmErrorKind::Unknown, 1, seed);
        const int         srvSec  = llmRetryDelaySeconds(LlmErrorKind::Server, 1, seed);
        const int         rateSec = llmRetryDelaySeconds(LlmErrorKind::RateLimit, 1, seed);
        XX_TEST_EXPECT_TRUE(netSec >= 1);
        XX_TEST_EXPECT_TRUE(netSec <= timeSec);
        XX_TEST_EXPECT_EQ(unkSec, timeSec);
        XX_TEST_EXPECT_TRUE(unkSec <= srvSec);
        XX_TEST_EXPECT_TRUE(srvSec <= rateSec);
        XX_TEST_EXPECT_TRUE(rateSec <= kLlmRetryMaxDelaySeconds);

        // 限流等得最久 (与上面 first 也保持不短于服务端)
        const int rate = llmRetryDelaySeconds(LlmErrorKind::RateLimit, 1, "429");
        XX_TEST_EXPECT_TRUE(rate >= first);

        // retry-after 优先 (任何类别都认: 服务端给了明确时间就按它来)
        const int honored
            = llmRetryDelaySeconds(LlmErrorKind::RateLimit, 3, R"({"retry-after": 7})");
        XX_TEST_EXPECT_EQ(honored, 7);
        const int honoredHeader
            = llmRetryDelaySeconds(LlmErrorKind::Timeout, 1, "Retry-After: 12\r\n");
        XX_TEST_EXPECT_EQ(honoredHeader, 12);
        const int honoredNetwork = llmRetryDelaySeconds(LlmErrorKind::Network, 1, "Retry-After: 9");
        XX_TEST_EXPECT_EQ(honoredNetwork, 9);

        // 超大 retry-after 夹到上限, 不为 0
        const int clamped
            = llmRetryDelaySeconds(LlmErrorKind::RateLimit, 1, R"("retry_after": 9999)");
        XX_TEST_EXPECT_EQ(clamped, kLlmRetryMaxDelaySeconds);
        XX_TEST_EXPECT_TRUE(
            llmRetryDelaySeconds(LlmErrorKind::Network, 10, "x") <= kLlmRetryMaxDelaySeconds
        );
    }

    return TestResult{g_le_passed, g_le_failed};
}

} // namespace test
} // namespace agentxx
