#pragma once

#include "agentxx/util/http_server.h"
#include <asio/awaitable.hpp>
#include <memory>
#include <neograph/api.h>
#include <neograph/json.h>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.h"
#undef XX_TEST_PASSED
#undef XX_TEST_FAILED
#define XX_TEST_PASSED g_da_passed
#define XX_TEST_FAILED g_da_failed

namespace agentxx {
namespace test {

extern int g_da_passed;
extern int g_da_failed;

// ===========================================================================
// Local LLM Simulator — an OpenAI-compatible HTTP server
// ===========================================================================
extern std::string    g_da_sim_response_content;
extern int            g_da_sim_prompt_tokens;
extern int            g_da_sim_completion_tokens;
extern neograph::json g_da_sim_tool_calls;
/// 最后一次收到的 /chat/completions 请求体 (供测试断言模型名/消息前缀)
extern neograph::json g_da_sim_last_request;
/// 按到达顺序记录所有 /chat/completions 请求 (供测试断言多次请求)
extern std::vector<neograph::json> g_da_sim_requests;
/// 模拟 thinking 模型的推理文本: 非空时流式响应先推送一段
/// [reasoning_content] delta (TYPE_THINKING), 并通过非流式的 message 字段返回,
/// 用于验证 reasoning_content → Think 历史消息的持久化链路
extern std::string g_da_sim_reasoning_content;
/// 响应前延迟 (毫秒), 用于模拟慢速 LLM 以测试取消; 0 表示不延迟
extern int g_da_sim_delay_ms;
/// 累计请求计数 (每次 /chat/completions 请求递增, 含失败请求), 供测试验证调用次数
extern int g_da_sim_request_count;
/// 剩余失败次数: >0 时接下来的请求直接返回 HTTP 500 并递减, 用于模拟 LLM API 持续失败
extern int g_da_sim_fail_count;
/// 前 N 次请求返回 tool_calls (之后返回纯文本); -1 = 不限制 (旧行为)
extern int g_da_sim_tool_calls_remaining;

/// 模拟器配置选项
struct DaSimConfig {
    bool                      enableRandomDelay = false; // 启用随机延迟模拟网络抖动
    std::chrono::milliseconds minDelay{5};               // 最小延迟（毫秒）
    std::chrono::milliseconds maxDelay{50};              // 最大延迟（毫秒）

    bool injectError429    = false; // 模拟限流错误 (HTTP 429)
    bool injectError503    = false; // 模拟服务不可用 (HTTP 503)
    bool injectStreamBreak = false; // 模拟流式断开

    bool   supportThinkingBlock = false; // 支持 Anthropic thinking block
    bool   streamByWords        = true;  // 按单词分片模拟真实流量
    size_t wordsPerChunk        = 5;     // 每次 chunk 的单词数
};

struct DaSimServer {
    std::unique_ptr<agentxx::util::HttpServer> svr;
    std::thread                                thr;
    uint16_t                                   port = 0;

    DaSimServer() = default;
    DaSimServer(DaSimServer&& o) noexcept;
    DaSimServer& operator=(DaSimServer&& o) noexcept;
    DaSimServer(const DaSimServer&)            = delete;
    DaSimServer& operator=(const DaSimServer&) = delete;
    ~DaSimServer();

    void stop();
};

asio::awaitable<TestResult> run_agent_tests();

/// 启动 LLM 模拟器
DaSimServer startDaSimServer();

/// LLM API 持续失败时重试耗尽的行为测试 (重试停止 + 不重复执行 toolcall)
asio::awaitable<void> test_agent_llm_retry_exhaust();

/// Toolcall 拦截普通异常继续运行的行为测试 (错误消息化 + agent 继续)
asio::awaitable<void> test_agent_toolcall_intercept_exception();

} // namespace test
} // namespace agentxx
