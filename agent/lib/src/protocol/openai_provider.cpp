#include "agentxx/protocol/openai_provider.h"

namespace agentxx {
namespace server {

std::unique_ptr<OpenAIProvider> OpenAIProvider::create(const agentxx::agent::ModelConfig& config) {
    return std::unique_ptr<OpenAIProvider>(new OpenAIProvider(config));
}

std::shared_ptr<neograph::Provider>
    OpenAIProvider::create_shared(const agentxx::agent::ModelConfig& config) {
    return std::shared_ptr<neograph::Provider>(new OpenAIProvider(config));
}

std::string OpenAIProvider::get_name() const {
    return "openai";
}

asio::awaitable<neograph::ChatCompletion> OpenAIProvider::invoke(
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

asio::awaitable<neograph::ChatCompletion> OpenAIProvider::invoke_format_data(
    const neograph::CompletionParams&  params,
    neograph::FormatDataStreamCallback on_chunk
) {
    if (on_chunk) {
        auto body              = buildBody(params);
        body["stream"]         = true;
        body["stream_options"] = {
            {"include_usage", true}
        };
        auto result = co_await doStream(params, body, on_chunk);
        co_return result;
    }
    co_return co_await completeAsync(params);
}

OpenAIProvider::OpenAIProvider(agentxx::agent::ModelConfig config) :
    config_(std::move(config)) {
    if (config_.baseUrl.empty()) {
        config_.baseUrl = kDefaultBaseUrl;
    }
}

neograph::json OpenAIProvider::buildBody(const neograph::CompletionParams& params) const {
    neograph::json body;
    body["model"]    = params.model.empty() ? config_.modelName : params.model;
    body["messages"] = neograph::messages_to_json(params.messages);

    if (!config_.sendThinking) {
        neograph::json src     = body["messages"];
        neograph::json cleaned = neograph::json::array();
        for (auto val : src) {
            neograph::json obj = neograph::json::object();
            for (auto [k, v] : val.items()) {
                if (k != "reasoning_content") {
                    obj[k] = v;
                }
            }
            cleaned.push_back(obj);
        }
        body["messages"] = cleaned;
    }

    if (!params.tools.empty()) {
        body["tools"]       = neograph::tools_to_json(params.tools);
        body["tool_choice"] = "auto";
    }

    if (params.temperature >= 0.0f) {
        body["temperature"] = params.temperature;
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
    OpenAIProvider::completeAsync(const neograph::CompletionParams& params) {
    using namespace agentxx::util;

    auto bodyJson = buildBody(params);
    auto bodyStr  = bodyJson.dump();

    HeaderMap headers;
    headers.set("Authorization", "Bearer " + config_.apiKey);

    auto resp = co_await HttpClient::postAsync(
        config_.baseUrl + "/chat/completions",
        bodyStr,
        "application/json",
        headers,
        HttpClient::RequestConfig{
            .connectTimeout = std::chrono::seconds{config_.connectTimeoutSeconds},
            .readTimeout    = std::chrono::seconds{config_.readTimeoutSeconds},
            .sslVerify      = config_.sslVerify,
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

    auto respJson = neograph::json::parse(r.body);
    auto choice   = respJson.at("choices").at(0);

    neograph::ChatCompletion completion;
    completion.message = neograph::parse_response_message(choice);

    if (completion.message.reasoning_content.empty()) {
        extractThinkTags(completion.message.content, completion.message.reasoning_content);
    }
    if (config_.extractToolCallsFromContent && completion.message.tool_calls.empty()) {
        extractToolCalls(completion.message.reasoning_content, completion.message.tool_calls);
        extractToolCalls(completion.message.content, completion.message.tool_calls);
    }

    if (completion.message.reasoning_content.empty()) {
        if (choice.contains("reasoning_content") && !choice["reasoning_content"].is_null()) {
            completion.message.reasoning_content = choice["reasoning_content"].get<std::string>();
        } else if (choice.contains("thinking") && !choice["thinking"].is_null()) {
            completion.message.reasoning_content = choice["thinking"].get<std::string>();
        }
    }

    if (respJson.contains("usage")) {
        auto u                             = respJson["usage"];
        completion.usage.prompt_tokens     = u.value("prompt_tokens", 0);
        completion.usage.completion_tokens = u.value("completion_tokens", 0);
        completion.usage.total_tokens      = u.value("total_tokens", 0);
    }

    co_return completion;
}

asio::awaitable<neograph::ChatCompletion> OpenAIProvider::doStream(
    const neograph::CompletionParams&  params,
    const neograph::json&              body,
    neograph::FormatDataStreamCallback on_chunk
) {
    using namespace agentxx::util;

    auto bodyStr = body.dump();

    HeaderMap headers;
    headers.set("Authorization", "Bearer " + config_.apiKey);

    neograph::ChatCompletion completion;
    completion.message.role = "assistant";
    std::string                       fullContent;
    std::string                       fullThinking;
    std::map<int, neograph::ToolCall> tcMap;
    std::string                       lineBuffer;

    co_await HttpClient::requestSseAsync(
        "POST",
        config_.baseUrl + "/chat/completions",
        bodyStr,
        "application/json",
        headers,
        HttpClient::RequestConfig{
            .connectTimeout = std::chrono::seconds{config_.connectTimeoutSeconds},
            .readTimeout    = std::chrono::seconds{config_.readTimeoutSeconds},
            .sslVerify      = config_.sslVerify,
        },
        [&](std::string_view chunk) {
            lineBuffer += chunk;
            processSseBuffer(
                lineBuffer,
                completion,
                fullContent,
                fullThinking,
                tcMap,
                on_chunk
            );
        }
    );

    if (!lineBuffer.empty()) {
        processSseBuffer(
            lineBuffer,
            completion,
            fullContent,
            fullThinking,
            tcMap,
            on_chunk,
            /*finalFlush=*/true
        );
    }

    if (fullThinking.empty()) {
        extractThinkTags(fullContent, fullThinking);
    }
    if (config_.extractToolCallsFromContent && tcMap.empty()) {
        extractToolCalls(fullContent, completion.message.tool_calls);
        extractToolCalls(fullThinking, completion.message.tool_calls);
    }
    completion.message.content           = fullContent;
    completion.message.reasoning_content = fullThinking;
    for (auto& [idx, tc] : tcMap) {
        if (tc.id.empty()) {
            tc.id = "call_" + std::to_string(idx);
        }
        completion.message.tool_calls.push_back(std::move(tc));
    }

    if (fullContent.empty() && fullThinking.empty() && completion.message.tool_calls.empty()) {
        XX_LOGW(
            "LLM stream completed with empty response | model={} usage={}/{}/{}",
            params.model,
            completion.usage.prompt_tokens,
            completion.usage.completion_tokens,
            completion.usage.total_tokens
        );
    }

    co_return completion;
}

void OpenAIProvider::extractToolCalls(
    std::string&                     content,
    std::vector<neograph::ToolCall>& toolCalls
) {
    auto trim = [](std::string_view s) -> std::string {
        size_t start = 0;
        while (start < s.size()
               && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r')) {
            ++start;
        }
        size_t end = s.size();
        while (end > start
               && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\n'
                   || s[end - 1] == '\r')) {
            --end;
        }
        return std::string{s.substr(start, end - start)};
    };

    auto tryExtract = [&](std::string_view jsonStr) -> bool {
        std::string trimmed = trim(jsonStr);
        if (trimmed.empty()) {
            return false;
        }
        neograph::json j = neograph::json::parse(trimmed);
        if (!j.is_object()) {
            return false;
        }

        neograph::ToolCall tc;
        bool               found = false;

        if (j.contains("name") && j["name"].is_string()) {
            neograph::json nameVal = j["name"];
            tc.name                = nameVal.get<std::string>();
            if (j.contains("arguments")) {
                neograph::json argsVal = j["arguments"];
                if (argsVal.is_object()) {
                    tc.arguments = argsVal.dump();
                } else if (argsVal.is_string()) {
                    tc.arguments = argsVal.get<std::string>();
                }
                found = true;
            }
        }

        if (!found && j.contains("function") && j["function"].is_object()) {
            neograph::json fn = j["function"];
            if (fn.contains("name") && fn["name"].is_string()) {
                neograph::json nameVal = fn["name"];
                tc.name                = nameVal.get<std::string>();
                if (fn.contains("arguments")) {
                    neograph::json argsVal = fn["arguments"];
                    if (argsVal.is_object()) {
                        tc.arguments = argsVal.dump();
                    } else if (argsVal.is_string()) {
                        tc.arguments = argsVal.get<std::string>();
                    }
                }
                found = true;
            }
        }

        if (found) {
            tc.id = "extr_" + std::to_string(toolCalls.size());
            toolCalls.push_back(std::move(tc));
            return true;
        }
        return false;
    };

    std::string cleaned;
    size_t      pos      = 0;
    int         numFound = 0;

    /// Phase 1: Extract from ```json code fences
    while (pos < content.size()) {
        auto fenceStart = content.find("```json", pos);
        if (fenceStart == std::string::npos) {
            break;
        }

        auto fenceEnd = content.find("```", fenceStart + 7);
        if (fenceEnd == std::string::npos) {
            cleaned += content.substr(pos);
            pos      = content.size();
            break;
        }

        cleaned += content.substr(pos, fenceStart - pos);

        std::string jsonStr = content.substr(fenceStart + 7, fenceEnd - fenceStart - 7);
        if (tryExtract(jsonStr)) {
            ++numFound;
        } else if (!trim(jsonStr).empty()) {
            cleaned += "```json\n" + jsonStr + "\n```";
        }

        pos = fenceEnd + 3;
    }

    if (pos < content.size()) {
        cleaned += content.substr(pos);
    }

    /// Phase 2: If nothing found from code fences, try the last JSON object in content
    if (numFound == 0) {
        auto lastBrace = cleaned.rfind('}');
        if (lastBrace != std::string::npos && lastBrace > 0) {
            auto openBrace = cleaned.rfind('{', lastBrace);
            if (openBrace != std::string::npos) {
                std::string candidate = cleaned.substr(openBrace, lastBrace - openBrace + 1);
                if (tryExtract(candidate)) {
                    cleaned.erase(openBrace);
                    ++numFound;
                }
            }
        }
    }

    if (numFound > 0) {
        content = trim(cleaned);
    }
}

void OpenAIProvider::extractThinkTags(std::string& content, std::string& thinking) {
    std::string cleaned;
    size_t      pos = 0;
    while (pos < content.size()) {
        auto start = content.find("<think>", pos);
        if (start == std::string::npos) {
            cleaned += content.substr(pos);
            break;
        }
        cleaned  += content.substr(pos, start - pos);
        auto end  = content.find("</think>", start + 7);
        if (end == std::string::npos) {
            thinking += content.substr(start + 7);
            content.erase(start);
            return;
        }
        thinking += content.substr(start + 7, end - start - 7);
        pos       = end + 8;
    }
    content = cleaned;
}

} // namespace server
} // namespace agentxx
