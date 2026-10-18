/// test_llm_error —— LLM 错误分类与重试策略
///
/// 覆盖:
/// - classifyLlmError: 各家 provider 的常见错误文本落到正确分类
///   (鉴权 / 额度 / 请求非法 / 上下文超限 / 限流 / 超时 / 网络 / 服务端 / 未知)
/// - 可重试性: 只有鉴权、额度、请求非法不可重试; 其余 (含网络/传输层故障) 都可重试
/// - 上下文超限识别 (触发一次压缩后重试的入口条件)
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

    // ---------------- 分类: 上下文超限 (最先判定, 可恢复) ----------------

    {
        struct Case {
            const char*                  text;
            agentxx::nodes::LlmErrorKind want;
        };

        const Case overflowCases[] = {
            {R"({"error":{"message":"This model's maximum context length is 128000 tokens"}})",
             LlmErrorKind::ContextOverflow                                                                                   },
            {R"({"error":{"code":"context_length_exceeded"}})",                                 LlmErrorKind::ContextOverflow},
            {R"(HTTP 400: prompt is too long)",                                                 LlmErrorKind::ContextOverflow},
            {R"(413 Request Entity Too Large)",                                                 LlmErrorKind::ContextOverflow},
            {"上下文过长，请缩减历史",                                               LlmErrorKind::ContextOverflow},
        };
        for (const auto& c : overflowCases) {
            const auto kind = classifyLlmError(c.text);
            XX_TEST_EXPECT_TRUE(kind == c.want);
            XX_TEST_EXPECT_TRUE(isLlmContextOverflow(kind));
            XX_TEST_EXPECT_TRUE(isLlmErrorRetryable(kind));
        }
    }

    // ---------------- 分类: 不可重试类 ----------------

    {
        const char* authCases[] = {
            R"(HTTP 401 Unauthorized)",
            R"({"error":{"type":"authentication_error","message":"Invalid API key"}})",
            "403 Forbidden",
        };
        for (const auto* text : authCases) {
            const auto kind = classifyLlmError(text);
            XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::Auth);
            XX_TEST_EXPECT_FALSE(isLlmErrorRetryable(kind));
            XX_TEST_EXPECT_FALSE(isLlmContextOverflow(kind));
        }

        const char* quotaCases[] = {
            R"({"error":{"code":"insufficient_quota","message":"You exceeded your current quota"}})",
            "HTTP 402 Payment Required",
            "账户余额不足，请充值",
        };
        for (const auto* text : quotaCases) {
            const auto kind = classifyLlmError(text);
            XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::Quota);
            XX_TEST_EXPECT_FALSE(isLlmErrorRetryable(kind));
        }

        const char* invalidCases[] = {
            R"({"error":{"type":"invalid_request_error","message":"unknown parameter: foo"}})",
            "HTTP 400 Bad Request",
        };
        for (const auto* text : invalidCases) {
            const auto kind = classifyLlmError(text);
            XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::InvalidRequest);
            XX_TEST_EXPECT_FALSE(isLlmErrorRetryable(kind));
        }
    }

    // ---------------- 分类: 可重试类 ----------------

    {
        const char* rateCases[] = {
            R"(HTTP 429 Too Many Requests)",
            R"({"error":{"message":"rate limit reached"}})",
            "请求频率过高，已限速",
        };
        for (const auto* text : rateCases) {
            const auto kind = classifyLlmError(text);
            XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::RateLimit);
            XX_TEST_EXPECT_TRUE(isLlmErrorRetryable(kind));
        }

        const char* timeoutCases[] = {
            "read timeout after 60s",
            "SSE read-header timeout after 30s",
            "操作超时",
        };
        for (const auto* text : timeoutCases) {
            const auto kind = classifyLlmError(text);
            XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::Timeout);
            XX_TEST_EXPECT_TRUE(isLlmErrorRetryable(kind));
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
            XX_TEST_EXPECT_TRUE(isLlmErrorRetryable(kind));
            XX_TEST_EXPECT_FALSE(isLlmContextOverflow(kind));
        }

        const char* serverCases[] = {
            R"(API error (HTTP 500): {"error":{"message":"simulated failure"}})",
            "503 Service Unavailable",
            "upstream overloaded",
        };
        for (const auto* text : serverCases) {
            const auto kind = classifyLlmError(text);
            XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::Server);
            XX_TEST_EXPECT_TRUE(isLlmErrorRetryable(kind));
        }
    }

    // ---------------- 分类: 未知 (按可重试处理, 与历史行为一致) ----------------

    {
        XX_TEST_EXPECT_TRUE(classifyLlmError("") == LlmErrorKind::Unknown);
        const auto kind = classifyLlmError("something totally unexpected happened");
        XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::Unknown);
        XX_TEST_EXPECT_TRUE(isLlmErrorRetryable(kind));
        XX_TEST_EXPECT_FALSE(isLlmContextOverflow(kind));
        XX_TEST_EXPECT_FALSE(llmErrorKindText(kind).empty());
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
