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

/// Anthropic Messages API provider.
/// Supports non-streaming, streaming, tool_use, and extended thinking.
class AnthropicProvider : public neograph::Provider {
public:

    static std::unique_ptr<AnthropicProvider> create(const agentxx::agent::ModelConfig& config);

    static std::shared_ptr<neograph::Provider>
        create_shared(const agentxx::agent::ModelConfig& config);

    ~AnthropicProvider() override = default;

    std::string get_name() const override;

    asio::awaitable<neograph::ChatCompletion> invoke(
        const neograph::CompletionParams& params,
        neograph::StreamCallback          on_chunk = nullptr
    ) override;

    asio::awaitable<neograph::ChatCompletion> invoke_format_data(
        const neograph::CompletionParams&  params,
        neograph::FormatDataStreamCallback on_chunk = nullptr
    ) override;

    // --- Public static helpers (exposed for unit testing) ---

    /// Convert neograph messages to Anthropic format.
    /// Returns {system_string, messages_json_array}.
    /// @param sendThinking 是否携带 thinking 内容块
    static std::pair<std::string, neograph::json> convertMessages(
        const std::vector<neograph::ChatMessage>& messages,
        bool                                      sendThinking = false
    );

    /// Convert neograph tools to Anthropic format.
    static neograph::json convertTools(const std::vector<neograph::ChatTool>& tools);

    /// Parse a non-streaming Anthropic response.
    static neograph::ChatCompletion parseResponse(const neograph::json& resp);

    /// Process Anthropic SSE buffer.
    /// - finalFlush: 连接关闭时对末尾未以 "\n\n" 结尾的最后一个事件块也进行解析
    static void processSseBuffer(
        std::string&                       buf,
        neograph::ChatCompletion&          completion,
        std::string&                       fullContent,
        std::string&                       fullThinking,
        std::map<int, neograph::ToolCall>& tcMap,
        std::map<int, std::string>&        blockTypes,
        neograph::FormatDataStreamCallback on_chunk,
        bool                               finalFlush = false
    ) {
        size_t pos;
        while ((pos = buf.find("\n\n")) != std::string::npos) {
            std::string block = buf.substr(0, pos);
            buf.erase(0, pos + 2);
            processSseBlock(
                block,
                completion,
                fullContent,
                fullThinking,
                tcMap,
                blockTypes,
                on_chunk
            );
        }
        if (finalFlush && !buf.empty()) {
            // 连接 abrupt 关闭时, 最后一个事件可能没有 trailing "\n\n", 此处补解析
            std::string block = std::move(buf);
            buf.clear();
            processSseBlock(
                block,
                completion,
                fullContent,
                fullThinking,
                tcMap,
                blockTypes,
                on_chunk
            );
        }
    }

    /// 解析单个 SSE 事件块 (以 "\n\n" 分隔的一块, 含若干 event:/data: 行)
    static void processSseBlock(
        std::string_view                   block,
        neograph::ChatCompletion&          completion,
        std::string&                       fullContent,
        std::string&                       fullThinking,
        std::map<int, neograph::ToolCall>& tcMap,
        std::map<int, std::string>&        blockTypes,
        neograph::FormatDataStreamCallback on_chunk
    ) {
        std::string currentEvent;
        std::string payload;

        size_t lineStart = 0;
        while (lineStart < block.size()) {
            auto        lineEnd = block.find('\n', lineStart);
            std::string line{
                (lineEnd == std::string::npos) ? block.substr(lineStart)
                                               : block.substr(lineStart, lineEnd - lineStart)
            };
            lineStart = (lineEnd == std::string::npos) ? block.size() : lineEnd + 1;

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            // SSE 规范: 字段名后冒号之后的单个前导空格可选; 多个 data: 行以 "\n" 拼接
            if (line.rfind("event:", 0) == 0) {
                auto val = line.substr(6);
                if (!val.empty() && val.front() == ' ') {
                    val.erase(0, 1);
                }
                currentEvent = val;
            } else if (line.rfind("data:", 0) == 0) {
                auto val = line.substr(5);
                if (!val.empty() && val.front() == ' ') {
                    val.erase(0, 1);
                }
                if (!payload.empty()) {
                    payload += "\n";
                }
                payload += val;
            }
        }

        if (payload.empty()) {
            return;
        }

        neograph::json j;
        try {
            j = neograph::json::parse(payload);
        } catch (...) {
            return;
        }

        // 允许异常时字节抛出给到 ModelCallNode ，以便自动处理
        if (currentEvent == "message_start") {
            if (j.contains("message") && j["message"].contains("usage")) {
                auto u                         = j["message"]["usage"];
                completion.usage.prompt_tokens = u.value("input_tokens", 0);
            }
        } else if (currentEvent == "content_block_start") {
            int idx = j.value("index", 0);
            if (j.contains("content_block")) {
                auto type       = j["content_block"].value("type", std::string{});
                blockTypes[idx] = type;
                if (type == "tool_use") {
                    tcMap[idx].id   = j["content_block"].value("id", std::string{});
                    tcMap[idx].name = j["content_block"].value("name", std::string{});
                }
            }
        } else if (currentEvent == "content_block_delta") {
            int idx = j.value("index", 0);
            if (j.contains("delta")) {
                auto deltaType = j["delta"].value("type", std::string{});
                if (deltaType == "text_delta") {
                    auto text    = j["delta"].value("text", std::string{});
                    fullContent += text;
                    if (on_chunk) {
                        on_chunk(neograph::ChatStreamChunk{
                            neograph::ChatStreamChunk::TYPE_CONTENT,
                            text,
                        });
                    }
                } else if (deltaType == "thinking_delta") {
                    auto thinking  = j["delta"].value("thinking", std::string{});
                    fullThinking  += thinking;
                    if (on_chunk) {
                        on_chunk(neograph::ChatStreamChunk{
                            neograph::ChatStreamChunk::TYPE_THINKING,
                            thinking
                        });
                    }
                } else if (deltaType == "input_json_delta") {
                    auto partialJson      = j["delta"].value("partial_json", std::string{});
                    tcMap[idx].arguments += partialJson;
                }
            }
        } else if (currentEvent == "message_delta") {
            if (j.contains("usage")) {
                completion.usage.completion_tokens = j["usage"].value<int>("output_tokens", 0);
                completion.usage.total_tokens
                    = completion.usage.prompt_tokens + completion.usage.completion_tokens;
            }
        }
    }

private:

    static constexpr const char* kDefaultBaseUrl = "https://api.anthropic.com";

    explicit AnthropicProvider(agentxx::agent::ModelConfig config);

    neograph::json buildBody(const neograph::CompletionParams& params) const;

    asio::awaitable<neograph::ChatCompletion> completeAsync(const neograph::CompletionParams& params
    );

    asio::awaitable<neograph::ChatCompletion> doStream(
        const neograph::CompletionParams&  params,
        const neograph::json&              body,
        neograph::FormatDataStreamCallback on_chunk
    );

    agentxx::agent::ModelConfig config_;
};

} // namespace server
} // namespace agentxx
