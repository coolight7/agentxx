#include "agentxx/protocol/anthropic_provider.h"
#include "agentxx/protocol/openai_provider.h"

namespace agentxx {
namespace server {

std::unique_ptr<AnthropicProvider>
    AnthropicProvider::create(const agentxx::agent::ModelConfig& config) {
    return std::unique_ptr<AnthropicProvider>(new AnthropicProvider(config));
}

std::shared_ptr<neograph::Provider>
    AnthropicProvider::create_shared(const agentxx::agent::ModelConfig& config) {
    return std::shared_ptr<neograph::Provider>(new AnthropicProvider(config));
}

std::string AnthropicProvider::get_name() const {
    return "anthropic";
}

asio::awaitable<neograph::ChatCompletion> AnthropicProvider::invoke(
    const neograph::CompletionParams& params,
    neograph::StreamCallback          on_chunk
) {
    if (!on_chunk) {
        co_return co_await invoke_format_data(params, nullptr);
    }
    co_return co_await invoke_format_data(
        params,
        [on_chunk](const neograph::ChatStreamChunk& chunk) {
            switch (chunk.type) {
                case neograph::ChatStreamChunk::TYPE_CONTENT:
                    on_chunk(chunk.data);
                    break;
                case neograph::ChatStreamChunk::TYPE_THINKING:
                    break;
            }
        }
    );
}

asio::awaitable<neograph::ChatCompletion> AnthropicProvider::invoke_format_data(
    const neograph::CompletionParams&  params,
    neograph::FormatDataStreamCallback on_chunk
) {
    if (on_chunk) {
        auto body      = buildBody(params);
        body["stream"] = true;
        auto result    = co_await doStream(params, body, on_chunk);
        co_return result;
    }
    co_return co_await completeAsync(params);
}

std::pair<std::string, neograph::json> AnthropicProvider::convertMessages(
    const std::vector<neograph::ChatMessage>& messages,
    bool                                      sendThinking
) {
    std::string    system;
    neograph::json arr = neograph::json::array();

    for (const auto& msg : messages) {
        if (msg.role == "system") {
            if (!system.empty()) {
                system += "\n";
            }
            system += msg.content;
            continue;
        }

        if (msg.role == "tool") {
            neograph::json j;
            j["role"]                  = "user";
            neograph::json content_arr = neograph::json::array();
            neograph::json tool_result;
            tool_result["type"]        = "tool_result";
            tool_result["tool_use_id"] = msg.tool_call_id;
            tool_result["content"]     = msg.content;
            content_arr.push_back(std::move(tool_result));
            j["content"] = std::move(content_arr);
            arr.push_back(std::move(j));
        } else if (msg.role == "assistant" && !msg.tool_calls.empty()) {
            neograph::json j;
            j["role"]                  = "assistant";
            neograph::json content_arr = neograph::json::array();
            if (sendThinking && !msg.reasoning_content.empty()) {
                content_arr.push_back({
                    {"type",     "thinking"           },
                    {"thinking", msg.reasoning_content}
                });
            }
            if (!msg.content.empty()) {
                content_arr.push_back({
                    {"type", "text"     },
                    {"text", msg.content}
                });
            }
            for (const auto& tc : msg.tool_calls) {
                neograph::json tool_use;
                tool_use["type"] = "tool_use";
                tool_use["id"]   = tc.id;
                tool_use["name"] = tc.name;
                try {
                    tool_use["input"] = neograph::json::parse(tc.arguments);
                } catch (...) {
                    tool_use["input"] = neograph::json::object();
                }
                content_arr.push_back(std::move(tool_use));
            }
            j["content"] = std::move(content_arr);
            arr.push_back(std::move(j));
        } else if (sendThinking && msg.role == "assistant" && !msg.reasoning_content.empty()) {
            neograph::json j;
            j["role"]                  = "assistant";
            neograph::json content_arr = neograph::json::array();
            content_arr.push_back({
                {"type",     "thinking"           },
                {"thinking", msg.reasoning_content}
            });
            if (!msg.content.empty()) {
                content_arr.push_back({
                    {"type", "text"     },
                    {"text", msg.content}
                });
            }
            j["content"] = std::move(content_arr);
            arr.push_back(std::move(j));
        } else {
            neograph::json j;
            j["role"]    = msg.role;
            j["content"] = msg.content;
            arr.push_back(std::move(j));
        }
    }

    return {system, std::move(arr)};
}

neograph::json AnthropicProvider::convertTools(const std::vector<neograph::ChatTool>& tools) {
    neograph::json arr = neograph::json::array();
    for (const auto& tool : tools) {
        neograph::json t;
        t["name"]         = tool.name;
        t["description"]  = tool.description;
        t["input_schema"] = tool.parameters;
        arr.push_back(std::move(t));
    }
    return arr;
}

neograph::ChatCompletion AnthropicProvider::parseResponse(const neograph::json& resp) {
    neograph::ChatCompletion completion;
    completion.message.role = "assistant";

    if (resp.contains("content") && resp["content"].is_array()) {
        for (const auto& block : resp["content"]) {
            auto type = block.value("type", std::string{});
            if (type == "text") {
                completion.message.content += block.value("text", std::string{});
            } else if (type == "tool_use") {
                neograph::ToolCall tc;
                tc.id   = block.value("id", std::string{});
                tc.name = block.value("name", std::string{});
                if (block.contains("input")) {
                    tc.arguments = block["input"].dump();
                }
                completion.message.tool_calls.push_back(std::move(tc));
            } else if (type == "thinking") {
                completion.message.reasoning_content += block.value("thinking", std::string{});
            }
        }
    }

    if (resp.contains("usage")) {
        auto u                             = resp["usage"];
        completion.usage.prompt_tokens     = u.value("input_tokens", 0);
        completion.usage.completion_tokens = u.value("output_tokens", 0);
        completion.usage.total_tokens
            = completion.usage.prompt_tokens + completion.usage.completion_tokens;
    }

    return completion;
}

AnthropicProvider::AnthropicProvider(agentxx::agent::ModelConfig config) :
    config_(std::move(config)) {
    if (config_.baseUrl.empty()) {
        config_.baseUrl = kDefaultBaseUrl;
    }
}

neograph::json AnthropicProvider::buildBody(const neograph::CompletionParams& params) const {
    neograph::json body;
    body["model"] = params.model.empty() ? config_.modelName : params.model;
    if (config_.modelSupportMaxToken > 0) {
        body["max_tokens"] = config_.modelSupportMaxToken;
    }

    auto [system, messages] = convertMessages(params.messages, config_.sendThinking);
    body["messages"]        = std::move(messages);
    if (!system.empty()) {
        body["system"] = system;
    }

    if (!params.tools.empty()) {
        body["tools"] = convertTools(params.tools);
    }

    if (params.temperature >= 0.0f) {
        body["temperature"] = params.temperature;
    }

    if (params.max_tokens > 0) {
        body["max_tokens"] = params.max_tokens;
    }

    if (config_.extra_config.is_object()) {
        for (const auto& [key, val] : config_.extra_config.items()) {
            if (!body.contains(key)) {
                body[key] = val;
            }
        }
    }

    if (!params.extra_fields.empty()) {
        for (const auto& [key, val] : params.extra_fields.items()) {
            body[key] = val;
        }
    }

    return body;
}

asio::awaitable<neograph::ChatCompletion>
    AnthropicProvider::completeAsync(const neograph::CompletionParams& params) {
    using namespace agentxx::util;

    auto bodyJson = buildBody(params);
    auto bodyStr  = bodyJson.dump();

    HeaderMap headers;
    headers.set("x-api-key", config_.apiKey);
    headers.set("anthropic-version", config_.anthropicVersion);

    auto resp = co_await HttpClient::postAsync(
        config_.baseUrl + "/v1/messages",
        bodyStr,
        "application/json",
        headers,
        HttpClient::RequestConfig{
            .connectTimeout   = std::chrono::seconds{config_.connectTimeoutSeconds},
            .readChunkTimeout = std::chrono::seconds{config_.readChunkTimeoutSeconds},
            .sslVerify        = config_.sslVerify,
        }
    );

    if (!resp.has_value()) {
        throw std::runtime_error("HTTP request failed: " + resp.error());
    }

    auto& r = resp.value();

    if (r.status == 429) {
        auto raw        = r.findHeader("retry-after");
        int  retryAfter = -1;
        if (!raw.empty()) {
            int seconds    = 0;
            auto [ptr, ec] = agentxx::util::parseNumberFromString(raw, seconds);
            if (ec == std::errc{} && seconds >= 0) {
                retryAfter = seconds;
            }
        }
        throw neograph::RateLimitError("API error (HTTP 429): " + r.body, retryAfter);
    }

    if (r.status != 200) {
        throw std::runtime_error("API error (HTTP " + std::to_string(r.status) + "): " + r.body);
    }

    auto respJson   = neograph::json::parse(r.body);
    auto completion = parseResponse(respJson);
    if (config_.extractToolCallsFromContent && completion.message.tool_calls.empty()) {
        OpenAIProvider::extractToolCalls(
            completion.message.reasoning_content,
            completion.message.tool_calls
        );
        OpenAIProvider::extractToolCalls(completion.message.content, completion.message.tool_calls);
    }
    co_return completion;
}

asio::awaitable<neograph::ChatCompletion> AnthropicProvider::doStream(
    const neograph::CompletionParams&  params,
    const neograph::json&              body,
    neograph::FormatDataStreamCallback on_chunk
) {
    using namespace agentxx::util;

    auto bodyStr = body.dump();

    HeaderMap headers;
    headers.set("x-api-key", config_.apiKey);
    headers.set("anthropic-version", config_.anthropicVersion);

    neograph::ChatCompletion completion;
    completion.message.role = "assistant";
    std::string                       fullContent;
    std::string                       fullThinking;
    std::map<int, neograph::ToolCall> tcMap;
    std::map<int, std::string>        blockTypes;
    std::string                       lineBuffer;

    co_await HttpClient::requestSseAsync(
        "POST",
        config_.baseUrl + "/v1/messages",
        bodyStr,
        "application/json",
        headers,
        HttpClient::RequestConfig{
            .connectTimeout   = std::chrono::seconds{config_.connectTimeoutSeconds},
            .readChunkTimeout = std::chrono::seconds{config_.readChunkTimeoutSeconds},
            .sslVerify        = config_.sslVerify,
        },
        [&](std::string_view chunk) {
            lineBuffer += chunk;
            try {
                processSseBuffer(
                    lineBuffer,
                    completion,
                    fullContent,
                    fullThinking,
                    tcMap,
                    blockTypes,
                    on_chunk
                );
            } catch (const std::exception&) {
            }
        }
    );

    if (!lineBuffer.empty()) {
        try {
            processSseBuffer(
                lineBuffer,
                completion,
                fullContent,
                fullThinking,
                tcMap,
                blockTypes,
                on_chunk,
                /*finalFlush=*/true
            );
        } catch (const std::exception&) {
        }
    }

    completion.message.content           = fullContent;
    completion.message.reasoning_content = fullThinking;
    for (auto& [idx, tc] : tcMap) {
        if (tc.id.empty()) {
            tc.id = "call_" + std::to_string(idx);
        }
        completion.message.tool_calls.push_back(std::move(tc));
    }
    if (config_.extractToolCallsFromContent && completion.message.tool_calls.empty()) {
        OpenAIProvider::extractToolCalls(completion.message.content, completion.message.tool_calls);
        OpenAIProvider::extractToolCalls(
            completion.message.reasoning_content,
            completion.message.tool_calls
        );
    }

    co_return completion;
}

} // namespace server
} // namespace agentxx
