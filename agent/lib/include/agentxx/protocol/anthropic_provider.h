#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/util/http_client.h"
#include "agentxx/util/string_util.h"
#include "asio/awaitable.hpp"
#include "asio/cancel_after.hpp"
#include "asio/redirect_error.hpp"
#include "asio/use_awaitable.hpp"
#include <charconv>
#include <chrono>
#include <limits>
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
    static void processSseBuffer(
        std::string&                       buf,
        neograph::ChatCompletion&          completion,
        std::string&                       fullContent,
        std::string&                       fullThinking,
        std::map<int, neograph::ToolCall>& tcMap,
        std::map<int, std::string>&        blockTypes,
        neograph::FormatDataStreamCallback on_chunk
    ) {
        size_t pos;
        while ((pos = buf.find("\n\n")) != std::string::npos) {
            std::string block = buf.substr(0, pos);
            buf.erase(0, pos + 2);

            std::string currentEvent;
            std::string payload;

            size_t lineStart = 0;
            while (lineStart < block.size()) {
                auto        lineEnd = block.find('\n', lineStart);
                std::string line    = (lineEnd == std::string::npos)
                                          ? block.substr(lineStart)
                                          : block.substr(lineStart, lineEnd - lineStart);
                lineStart           = (lineEnd == std::string::npos) ? block.size() : lineEnd + 1;

                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                if (line.rfind("event: ", 0) == 0) {
                    currentEvent = line.substr(7);
                } else if (line.rfind("data: ", 0) == 0) {
                    payload = line.substr(6);
                }
            }

            if (payload.empty()) {
                continue;
            }

            try {
                auto j = neograph::json::parse(payload);

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
                                    text
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
                        auto u                             = j["usage"];
                        completion.usage.completion_tokens = u.value("output_tokens", 0);
                        completion.usage.total_tokens
                            = completion.usage.prompt_tokens + completion.usage.completion_tokens;
                    }
                }
            } catch (...) {
            }
        }
    }

private:

    static constexpr const char* kDefaultBaseUrl = "https://api.anthropic.com";

    explicit AnthropicProvider(agentxx::agent::ModelConfig config);

    neograph::json buildBody(const neograph::CompletionParams& params) const;

    struct ParsedEndpoint {
        std::string scheme;
        std::string host;
        uint16_t    port;
        std::string prefix;
    };

    static ParsedEndpoint parseEndpoint(const std::string& base_url);

    asio::awaitable<neograph::ChatCompletion> completeAsync(const neograph::CompletionParams& params
    );

    asio::awaitable<neograph::ChatCompletion> doStream(
        const neograph::CompletionParams&  params,
        const neograph::json&              body,
        neograph::FormatDataStreamCallback on_chunk
    );

    template<typename Stream>
    asio::awaitable<void> readSseStream(
        Stream&                               stream,
        std::chrono::steady_clock::time_point deadline,
        std::chrono::seconds                  readTimeout,
        neograph::ChatCompletion&             completion,
        std::string&                          fullContent,
        std::string&                          fullThinking,
        std::map<int, neograph::ToolCall>&    tcMap,
        std::map<int, std::string>&           blockTypes,
        std::string&                          lineBuffer,
        neograph::FormatDataStreamCallback    on_chunk
    ) {
        namespace http = boost::beast::http;

        auto rem = [&] {
            auto d = deadline - std::chrono::steady_clock::now();
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::max(d, std::chrono::steady_clock::duration::zero())
            );
        };

        boost::beast::flat_buffer                buf;
        http::response_parser<http::string_body> parser;
        parser.body_limit(std::numeric_limits<uint64_t>::max());
        parser.eager(true);

        co_await http::async_read_header(
            stream,
            buf,
            parser,
            asio::cancel_after(rem(), asio::use_awaitable)
        );

        if (parser.get().result_int() == 429) {
            co_await http::async_read(
                stream,
                buf,
                parser,
                asio::cancel_after(rem(), asio::use_awaitable)
            );
            auto resp       = parser.release();
            auto raw        = resp[http::field::retry_after];
            int  retryAfter = -1;
            if (!raw.empty()) {
                int seconds    = 0;
                auto [ptr, ec] = agentxx::util::parseNumberFromString(raw, seconds);
                if (ec == std::errc{} && seconds >= 0) {
                    retryAfter = seconds;
                }
            }
            throw neograph::RateLimitError("API error (HTTP 429): " + resp.body(), retryAfter);
        }

        if (parser.get().result_int() != 200) {
            co_await http::async_read(
                stream,
                buf,
                parser,
                asio::cancel_after(rem(), asio::use_awaitable)
            );
            auto resp = parser.release();
            throw std::runtime_error(
                "API error (HTTP " + std::to_string(resp.result_int()) + "): " + resp.body()
            );
        }

        size_t                    processed = 0;
        boost::system::error_code ec;
        while (!parser.is_done()) {
            co_await http::async_read_some(
                stream,
                buf,
                parser,
                asio::cancel_after(readTimeout, asio::redirect_error(asio::use_awaitable, ec))
            );
            if (ec) {
                break;
            }
            auto& body = parser.get().body();
            if (body.size() > processed) {
                lineBuffer += body.substr(processed);
                processed   = body.size();
                processSseBuffer(
                    lineBuffer,
                    completion,
                    fullContent,
                    fullThinking,
                    tcMap,
                    blockTypes,
                    on_chunk
                );
            }
        }
        if (!lineBuffer.empty()) {
            processSseBuffer(
                lineBuffer,
                completion,
                fullContent,
                fullThinking,
                tcMap,
                blockTypes,
                on_chunk
            );
        }
    }

    static asio::ssl::context& sslContext();

    agentxx::agent::ModelConfig config_;
};

} // namespace server
} // namespace agentxx
