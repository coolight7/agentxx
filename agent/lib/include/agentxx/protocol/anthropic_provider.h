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
/// 文档 https://vercel.com/docs/ai-gateway/sdks-and-apis/anthropic-messages-api
/// Supports non-streaming, streaming, tool_use, and extended thinking.
class AnthropicProvider : public neograph::Provider {
public:

    /// ChatMessage.extra 中保存带 signature 的 thinking/redacted_thinking 原始块的键。
    /// Anthropic 要求多轮对话回传 thinking 块时携带响应中的原始 signature, 故解析响应时
    /// 按序存入 extra, convertMessages 时原样回传。
    static constexpr const char* kThinkingBlocksKey = "anthropic_thinking_blocks";

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

    /// 向 completion.message.extra[kThinkingBlocksKey] 追加一个 thinking 相关块
    /// (thinking/redacted_thinking), 首次追加时初始化为数组
    static void appendThinkingBlock(neograph::ChatCompletion& completion, neograph::json block) {
        if (!completion.message.extra.contains(kThinkingBlocksKey)) {
            completion.message.extra[kThinkingBlocksKey] = neograph::json::array();
        }
        completion.message.extra[kThinkingBlocksKey].push_back(std::move(block));
    }

    /// Process Anthropic SSE buffer.
    /// - 事件分隔符同时支持 "\n\n" 与 "\r\n\r\n" (SSE 规范允许 \r\n 行结尾)
    /// - thinkingTexts/blockSignatures: 按 block index 累积 thinking 文本与 signature,
    ///   content_block_stop 时组装为带 signature 的 thinking 块存入 completion.message.extra
    /// - finalFlush: 连接关闭时对末尾未以 "\n\n" 结尾的最后一个事件块也进行解析
    /// - 返回本次调用是否处理到了 "message_stop" 结束事件 (用于检测流截断)
    static bool processSseBuffer(
        std::string&                       buf,
        neograph::ChatCompletion&          completion,
        std::string&                       fullContent,
        std::string&                       fullThinking,
        std::map<int, neograph::ToolCall>& tcMap,
        std::map<int, std::string>&        blockTypes,
        std::map<int, std::string>&        thinkingTexts,
        std::map<int, std::string>&        blockSignatures,
        neograph::FormatDataStreamCallback on_chunk,
        bool                               finalFlush = false
    ) {
        bool done = false;
        while (true) {
            // SSE 规范允许 \n 或 \r\n 行结尾, 事件分隔符相应可能是 "\n\n" 或 "\r\n\r\n",
            // 取最先出现者为界
            auto   posLf   = buf.find("\n\n");
            auto   posCrlf = buf.find("\r\n\r\n");
            size_t pos, sepLen;
            if (posCrlf != std::string::npos && (posLf == std::string::npos || posCrlf < posLf)) {
                pos    = posCrlf;
                sepLen = 4;
            } else if (posLf != std::string::npos) {
                pos    = posLf;
                sepLen = 2;
            } else {
                break;
            }
            std::string block = buf.substr(0, pos);
            buf.erase(0, pos + sepLen);
            done |= processSseBlock(
                block,
                completion,
                fullContent,
                fullThinking,
                tcMap,
                blockTypes,
                thinkingTexts,
                blockSignatures,
                on_chunk
            );
        }
        if (finalFlush && !buf.empty()) {
            // 连接 abrupt 关闭时, 最后一个事件可能没有 trailing "\n\n", 此处补解析
            std::string block = std::move(buf);
            buf.clear();
            done |= processSseBlock(
                block,
                completion,
                fullContent,
                fullThinking,
                tcMap,
                blockTypes,
                thinkingTexts,
                blockSignatures,
                on_chunk
            );
        }
        return done;
    }

    /// 解析单个 SSE 事件块 (以 "\n\n" 分隔的一块, 含若干 event:/data: 行)
    /// 返回该事件块是否为 "message_stop" 结束事件
    static bool processSseBlock(
        std::string_view                   block,
        neograph::ChatCompletion&          completion,
        std::string&                       fullContent,
        std::string&                       fullThinking,
        std::map<int, neograph::ToolCall>& tcMap,
        std::map<int, std::string>&        blockTypes,
        std::map<int, std::string>&        thinkingTexts,
        std::map<int, std::string>&        blockSignatures,
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
            return false;
        }

        neograph::json j;
        try {
            j = neograph::json::parse(payload);
        } catch (...) {
            return false;
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
                } else if (type == "redacted_thinking") {
                    // redacted_thinking 块必须在多轮对话中原样回传
                    neograph::json b;
                    b["type"] = "redacted_thinking";
                    b["data"] = j["content_block"].value("data", std::string{});
                    appendThinkingBlock(completion, std::move(b));
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
                    auto thinking       = j["delta"].value("thinking", std::string{});
                    fullThinking       += thinking;
                    thinkingTexts[idx] += thinking;
                    if (on_chunk) {
                        on_chunk(neograph::ChatStreamChunk{
                            neograph::ChatStreamChunk::TYPE_THINKING,
                            thinking
                        });
                    }
                } else if (deltaType == "signature_delta") {
                    // thinking 块的 signature, 多轮对话回传 thinking 时 Anthropic 要求携带
                    blockSignatures[idx] += j["delta"].value("signature", std::string{});
                } else if (deltaType == "input_json_delta") {
                    auto partialJson      = j["delta"].value("partial_json", std::string{});
                    tcMap[idx].arguments += partialJson;
                }
            }
        } else if (currentEvent == "content_block_stop") {
            int  idx = j.value("index", 0);
            auto it  = blockTypes.find(idx);
            if (it != blockTypes.end() && it->second == "thinking") {
                auto sigIt = blockSignatures.find(idx);
                // 仅保存带 signature 的 thinking 块: 无 signature 的 thinking 回传会被 API 拒绝
                if (sigIt != blockSignatures.end() && !sigIt->second.empty()) {
                    neograph::json b;
                    b["type"]      = "thinking";
                    b["thinking"]  = thinkingTexts[idx];
                    b["signature"] = sigIt->second;
                    appendThinkingBlock(completion, std::move(b));
                }
            }
        } else if (currentEvent == "message_delta") {
            if (j.contains("usage")) {
                completion.usage.completion_tokens = j["usage"].value<int>("output_tokens", 0);
                completion.usage.total_tokens
                    = completion.usage.prompt_tokens + completion.usage.completion_tokens;
            }
        }
        return currentEvent == "message_stop";
    }

private:

    static constexpr std::string_view kDefaultBaseUrl{"https://api.anthropic.com"};

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
