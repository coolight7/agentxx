#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/util/http_client.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "asio/awaitable.hpp"
#include "asio/use_awaitable.hpp"
#include <charconv>
#include <chrono>
#include <map>
#include <memory>
#include <neograph/api.h>
#include <neograph/provider.h>
#include <string>
#include <vector>

namespace agentxx {
namespace server {

/// OpenAI 协议 Provider
/// API 文档
/// - Chat Completions https://vercel.com/docs/ai-gateway/sdks-and-apis/openai-chat-completions
/// - Response https://vercel.com/docs/ai-gateway/sdks-and-apis/responses
///
/// 支持两种 API 形态 (由 type 控制):
///   - Chat Completions API  (`POST /chat/completions`, 默认)
///   - Responses API       (`POST /responses`, 即 Codex 使用的 API)
///
/// 兼容性增强:
///   - baseUrl 自动去除末尾 '/'，避免拼接出 `//chat/completions` 双斜杠
///   - apiPath 可自定义端点路径 (适配 DeepSeek/Moonshot/Ollama/Azure 等兼容服务)
///   - 发送 params.max_tokens (支持 max_completion_tokens 字段切换)
///   - finish_reason → stop_reason 归一化
///   - tool_calls 缺失 id 时自动回填 call_N (流式/非流式一致)
///   - 非 2xx 响应自动解析 error.message / error.code 提升报错可读性
///   - extraHeaders 支持自定义 HTTP 请求头
///   - 接受任意 2xx 状态码 (部分网关返回 201/202)
///   - 畸形响应容错: 非法 JSON / 缺失 choices / 非标准字段类型
///     (数字 tool_call id、字符串 usage、字符串 index 等) 不抛异常, 给出可读错误
///   - Responses API: status="failed" / response.incomplete / 顶层 error 对象处理
class OpenAIProvider : public neograph::Provider {
public:

    /// ChatMessage.extra 中保存 Responses API reasoning 原始项的键 (如 encrypted_content / id 等)
    static constexpr const char* kResponsesReasoningItemsKey = "responses_reasoning_items";
    /// ChatMessage.extra 中保存思考 token 数量的键
    static constexpr const char* kReasoningTokensKey = "reasoning_tokens";

    static std::unique_ptr<OpenAIProvider> create(const agentxx::agent::ModelConfig& config);

    static std::shared_ptr<neograph::Provider>
        create_shared(const agentxx::agent::ModelConfig& config);

    ~OpenAIProvider() override = default;

    std::string get_name() const override;

    asio::awaitable<neograph::ChatCompletion> invoke(
        const neograph::CompletionParams& params,
        neograph::StreamCallback          on_chunk = nullptr
    ) override;

    asio::awaitable<neograph::ChatCompletion> invoke_format_data(
        const neograph::CompletionParams&  params,
        neograph::FormatDataStreamCallback on_chunk = nullptr
    ) override;

    /// 从 content 文本中提取嵌入的 tool call（LLM 未正确使用 tool_calls API 时的兜底）
    /// - 支持 ```json 代码块和行内 JSON 两种格式
    /// - 兼容 llama.cpp 等本地模型的 XML 风格输出:
    ///   <tool_call><function=name>args</function></tool_call> (含缺失参数/未闭合标签场景)
    /// - 匹配模式: {"name":"...","arguments":...} 或 {"function":{"name":"...","arguments":...}}
    /// - 成功提取后从 content 中移除匹配的文本
    static void extractToolCalls(std::string& content, std::vector<neograph::ToolCall>& toolCalls);

    /// 处理 Chat Completions API 的 SSE 缓冲区 (public 以便单测)
    /// - finalFlush: 连接关闭时对末尾未以 "\n" 结尾的最后一行也进行解析
    /// - 返回本次调用是否处理到了 "data: [DONE]" 结束标记 (用于检测流截断)
    static bool processSseBuffer(
        std::string&                       buf,
        neograph::ChatCompletion&          completion,
        std::string&                       fullContent,
        std::string&                       fullThinking,
        std::map<int, neograph::ToolCall>& tcMap,
        neograph::FormatDataStreamCallback on_chunk,
        bool                               finalFlush = false
    );

    /// 解析 Chat Completions API 单行 SSE, 返回该行是否为 "data: [DONE]" 结束标记
    static bool processSseLine(
        std::string_view                   line_in,
        neograph::ChatCompletion&          completion,
        std::string&                       fullContent,
        std::string&                       fullThinking,
        std::map<int, neograph::ToolCall>& tcMap,
        neograph::FormatDataStreamCallback on_chunk
    );

    /// 处理 Responses API 的 SSE 缓冲区 (public 以便单测)
    /// - 与 processSseBuffer 相同语义; errOut 非空时记录 API 错误事件 (response.failed/error)
    /// - 返回是否处理到了结束标记 (response.completed 或 [DONE])
    static bool processResponsesSseBuffer(
        std::string&                       buf,
        neograph::ChatCompletion&          completion,
        std::string&                       fullContent,
        std::string&                       fullThinking,
        std::map<int, neograph::ToolCall>& tcMap,
        neograph::FormatDataStreamCallback on_chunk,
        bool                               finalFlush = false,
        std::string*                       errOut     = nullptr
    );

    /// 解析 Responses API 单行 SSE
    /// - 返回该行是否为结束标记 (response.completed / [DONE])
    /// - errOut 非空时记录 API 错误事件 (response.failed/error)
    static bool processResponsesSseLine(
        std::string_view                   line_in,
        neograph::ChatCompletion&          completion,
        std::string&                       fullContent,
        std::string&                       fullThinking,
        std::map<int, neograph::ToolCall>& tcMap,
        neograph::FormatDataStreamCallback on_chunk,
        std::string*                       errOut = nullptr
    );

    /// tool_calls 缺失 id 时回填 call_N (与流式路径行为一致)
    static void fillMissingToolCallIds(neograph::ChatCompletion& completion);

    /// 从 content 中提取 <think>...</think> 标签到 thinking (public 以便单测)
    static void extractThinkTags(std::string& content, std::string& thinking);

private:

    inline static constexpr std::string_view kDefaultBaseUrl{"https://api.openai.com"};
    inline static constexpr std::string_view kDefaultApiPath{"/chat/completions"};
    inline static constexpr std::string_view kDefaultResponsesPath{"/responses"};

    explicit OpenAIProvider(agentxx::agent::ModelConfig config);

    /// 组装请求 URL: baseUrl (去尾 '/') + apiPath (默认按 API 形态选择)
    std::string apiUrl() const;

    /// 填充请求头: Authorization + extraHeaders
    void applyHeaders(agentxx::util::HeaderMap& headers) const;

    /// 归一化 finish_reason → stop_reason
    static std::string mapStopReason(std::string_view finishReason);

    /// 从错误响应 body 中提取 error.message / error.code, 失败时返回原 body
    static std::string extractApiError(const std::string& body);

    neograph::json buildBody(const neograph::CompletionParams& params) const;

    neograph::json buildResponsesBody(const neograph::CompletionParams& params) const;

    asio::awaitable<neograph::ChatCompletion> completeAsync(const neograph::CompletionParams& params
    );

    asio::awaitable<neograph::ChatCompletion>
        completeAsyncResponses(const neograph::CompletionParams& params);

    asio::awaitable<neograph::ChatCompletion> doStream(
        const neograph::CompletionParams&  params,
        const neograph::json&              body,
        neograph::FormatDataStreamCallback on_chunk
    );

    asio::awaitable<neograph::ChatCompletion> doStreamResponses(
        const neograph::CompletionParams&  params,
        const neograph::json&              body,
        neograph::FormatDataStreamCallback on_chunk
    );

private:

    agentxx::agent::ModelConfig config_;
};

} // namespace server
} // namespace agentxx
