/// test_llm_error —— LLM 错误分类与重试策略 (计划 LLM-2 / LLM-3)
///
/// 覆盖:
/// - classifyLlmError: 各家 provider 的常见错误文本落到正确分类
///   (鉴权 / 额度 / 请求非法 / 上下文超限 / 限流 / 超时 / 服务端 / 未知)
/// - 可重试性: 鉴权、额度、请求非法不可重试; 其余可重试
/// - 上下文超限识别 (触发一次压缩后重试的入口条件)
/// - llmRetryDelaySeconds: 有界指数退避、限流基数更大、`retry-after` 优先、
///   上限夹取、抖动不会让等待变成 0
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
            const char*                   text;
            agentxx::nodes::LlmErrorKind  want;
        };
        const Case overflowCases[] = {
            {R"({"error":{"message":"This model's maximum context length is 128000 tokens"}})",
             LlmErrorKind::ContextOverflow},
            {R"({"error":{"code":"context_length_exceeded"}})", LlmErrorKind::ContextOverflow},
            {R"(HTTP 400: prompt is too long)", LlmErrorKind::ContextOverflow},
            {R"(413 Request Entity Too Large)", LlmErrorKind::ContextOverflow},
            {"上下文过长，请缩减历史", LlmErrorKind::ContextOverflow},
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
            "connection reset by peer",
            "操作超时",
        };
        for (const auto* text : timeoutCases) {
            const auto kind = classifyLlmError(text);
            XX_TEST_EXPECT_TRUE(kind == LlmErrorKind::Timeout);
            XX_TEST_EXPECT_TRUE(isLlmErrorRetryable(kind));
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
        // 指数增长且不超过上限
        const int first  = llmRetryDelaySeconds(LlmErrorKind::Server, 1, "500");
        const int second = llmRetryDelaySeconds(LlmErrorKind::Server, 2, "500");
        const int tenth  = llmRetryDelaySeconds(LlmErrorKind::Server, 10, "500");
        XX_TEST_EXPECT_TRUE(first >= 1 && first <= 4);   // 2 + 抖动(0~2)
        XX_TEST_EXPECT_TRUE(second >= first);
        XX_TEST_EXPECT_TRUE(tenth <= kLlmRetryMaxDelaySeconds);

        // 限流基数更大 (等待更久)
        const int rate = llmRetryDelaySeconds(LlmErrorKind::RateLimit, 1, "429");
        XX_TEST_EXPECT_TRUE(rate >= first);

        // retry-after 优先 (服务端给了明确时间就按它来)
        const int honored
            = llmRetryDelaySeconds(LlmErrorKind::RateLimit, 3, R"({"retry-after": 7})");
        XX_TEST_EXPECT_EQ(honored, 7);
        const int honoredHeader
            = llmRetryDelaySeconds(LlmErrorKind::Timeout, 1, "Retry-After: 12\r\n");
        XX_TEST_EXPECT_EQ(honoredHeader, 12);

        // 超大 retry-after 夹到上限, 不为 0
        const int clamped
            = llmRetryDelaySeconds(LlmErrorKind::RateLimit, 1, R"("retry_after": 9999)");
        XX_TEST_EXPECT_EQ(clamped, kLlmRetryMaxDelaySeconds);
        XX_TEST_EXPECT_TRUE(
            llmRetryDelaySeconds(LlmErrorKind::Unknown, 1, "x") >= 1
        );
    }

    return TestResult{g_le_passed, g_le_failed};
}

} // namespace test
} // namespace agentxx
