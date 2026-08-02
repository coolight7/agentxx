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

class OpenAIProvider : public neograph::Provider {
public:

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

    /// 从 content 文本中提取嵌入的 tool call JSON（LLM 未正确使用 tool_calls API 时的兜底）
    /// - 支持 ```json 代码块和行内 JSON 两种格式
    /// - 匹配模式: {"name":"...","arguments":...} 或 {"function":{"name":"...","arguments":...}}
    /// - 成功提取后从 content 中移除匹配的文本
    static void extractToolCalls(std::string& content, std::vector<neograph::ToolCall>& toolCalls);

private:

    static constexpr const char* kDefaultBaseUrl = "https://api.openai.com";

    explicit OpenAIProvider(agentxx::agent::ModelConfig config);

    neograph::json buildBody(const neograph::CompletionParams& params) const;

    asio::awaitable<neograph::ChatCompletion> completeAsync(const neograph::CompletionParams& params
    );

    asio::awaitable<neograph::ChatCompletion> doStream(
        const neograph::CompletionParams&  params,
        const neograph::json&              body,
        neograph::FormatDataStreamCallback on_chunk
    );

public:

    /// 处理 OpenAI SSE 缓冲区 (public 以便单测)
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
    ) {
        bool   done = false;
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            done |= processSseLine(line, completion, fullContent, fullThinking, tcMap, on_chunk);
        }
        if (finalFlush && !buf.empty()) {
            // 连接 abrupt 关闭时, 最后一行可能没有 trailing "\n", 此处补解析
            std::string line = std::move(buf);
            buf.clear();
            done |= processSseLine(line, completion, fullContent, fullThinking, tcMap, on_chunk);
        }
        return done;
    }

    /// 解析单行 SSE, 返回该行是否为 "data: [DONE]" 结束标记
    static bool processSseLine(
        std::string_view                   line_in,
        neograph::ChatCompletion&          completion,
        std::string&                       fullContent,
        std::string&                       fullThinking,
        std::map<int, neograph::ToolCall>& tcMap,
        neograph::FormatDataStreamCallback on_chunk
    ) {
        std::string line{line_in};
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // SSE 规范: "data:" 后的单个前导空格可选
        if (line.rfind("data:", 0) != 0) {
            return false;
        }
        std::string payload = line.substr(5);
        if (!payload.empty() && payload.front() == ' ') {
            payload.erase(0, 1);
        }

        if (payload == "[DONE]") {
            return true;
        }

        // 畸形 data 行 (部分代理/网关会注入非 JSON 内容) 应跳过而不是中断整个流
        neograph::json j;
        try {
            j = neograph::json::parse(payload);
        } catch (...) {
            return false;
        }

        if (j.contains("usage") && !j["usage"].is_null()) {
            auto u                             = j["usage"];
            completion.usage.prompt_tokens     = u.value("prompt_tokens", 0);
            completion.usage.completion_tokens = u.value("completion_tokens", 0);
            completion.usage.total_tokens      = u.value(
                "total_tokens",
                completion.usage.prompt_tokens + completion.usage.completion_tokens
            );
        }

        if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) {
            return false;
        }
        auto delta = j["choices"][0]["delta"];

        if (delta.contains("content") && !delta["content"].is_null()) {
            std::string token = delta["content"].get<std::string>();
            if (!token.empty()) {
                fullContent += token;
                if (on_chunk) {
                    on_chunk(
                        neograph::ChatStreamChunk{neograph::ChatStreamChunk::TYPE_CONTENT, token}
                    );
                }
            }
        }

        if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
            auto token = delta["reasoning_content"].get<std::string>();
            if (!token.empty()) {
                fullThinking += token;
                if (on_chunk) {
                    on_chunk(
                        neograph::ChatStreamChunk{neograph::ChatStreamChunk::TYPE_THINKING, token}
                    );
                }
            }
        } else if (delta.contains("thinking") && delta["thinking"].is_string()) {
            auto token = delta["thinking"].get<std::string>();
            if (!token.empty()) {
                fullThinking += token;
                if (on_chunk) {
                    on_chunk(
                        neograph::ChatStreamChunk{neograph::ChatStreamChunk::TYPE_THINKING, token}
                    );
                }
            }
        }

        if (delta.contains("tool_calls")) {
            for (const auto& tc : delta["tool_calls"]) {
                int idx = tc.value("index", 0);
                if (tc.contains("id")) {
                    tcMap[idx].id = tc["id"].get<std::string>();
                }
                if (tc.contains("function")) {
                    if (tc["function"].contains("name")) {
                        tcMap[idx].name += tc["function"]["name"].get<std::string>();
                    }
                    if (tc["function"].contains("arguments")) {
                        tcMap[idx].arguments += tc["function"]["arguments"].get<std::string>();
                    }
                }
            }
        }
        return false;
    }

    static void extractThinkTags(std::string& content, std::string& thinking);

private:

    agentxx::agent::ModelConfig config_;
};

} // namespace server
} // namespace agentxx
