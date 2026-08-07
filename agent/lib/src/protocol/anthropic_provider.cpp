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

    // 是否携带从 Anthropic 响应中捕获的带 signature 的 thinking 块
    auto hasThinkingBlocks = [](const neograph::ChatMessage& msg) {
        return msg.extra.contains(kThinkingBlocksKey) && msg.extra[kThinkingBlocksKey].is_array()
               && !msg.extra[kThinkingBlocksKey].empty();
    };
    // 追加 thinking 相关块: 优先使用响应中捕获的原始块 (含 signature, Anthropic 要求回传
    // thinking 时携带原始 signature); 无捕获时降级使用 reasoning_content (跨 provider/旧历史)
    auto appendThinkingBlocks = [&](neograph::json& contentArr, const neograph::ChatMessage& msg) {
        if (!sendThinking) {
            return;
        }
        if (hasThinkingBlocks(msg)) {
            for (const auto& b : msg.extra[kThinkingBlocksKey]) {
                contentArr.push_back(b);
            }
            return;
        }
        if (!msg.reasoning_content.empty()) {
            contentArr.push_back({
                {"type",     "thinking"           },
                {"thinking", msg.reasoning_content}
            });
        }
    };

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
            appendThinkingBlocks(content_arr, msg);
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
        } else if (sendThinking && msg.role == "assistant" && (!msg.reasoning_content.empty() || hasThinkingBlocks(msg))) {
            neograph::json j;
            j["role"]                  = "assistant";
            neograph::json content_arr = neograph::json::array();
            appendThinkingBlocks(content_arr, msg);
            if (!msg.content.empty()) {
                content_arr.push_back({
                    {"type", "text"     },
                    {"text", msg.content}
                });
            }
            j["content"] = std::move(content_arr);
            arr.push_back(std::move(j));
        } else if (!msg.image_urls.empty() || !msg.audio_urls.empty() || !msg.video_urls.empty()) {
            // 多模态消息: text + image/audio/video 块
            //   - 图片: {"type":"image","source":{base64|url}}
            //   - 音频: {"type":"audio","source":{"type":"base64",...}} (Anthropic 仅支持 base64)
            //   - 视频: {"type":"video","source":{base64|url}}
            // data URL 解析为 base64 源 (自动推导 media_type); HTTP URL 使用 url 源
            neograph::json j;
            j["role"]                   = msg.role;
            neograph::json content_arr  = neograph::json::array();
            auto           appendSource = [&](const std::string& url, const std::string& kind) {
                if (auto parsed = neograph::parse_data_url(url)) {
                    content_arr.push_back({
                        {"type",   kind            },
                        {"source",
                         {{"type", "base64"},
                          {"media_type", parsed->first},
                          {"data", parsed->second}}},
                    });
                } else {
                    // HTTP URL 或无法解析的 data URL: 使用 url 源 (Anthropic 图片/视频支持)
                    content_arr.push_back({
                        {"type",   kind                           },
                        {"source", {{"type", "url"}, {"url", url}}},
                    });
                }
            };
            if (!msg.content.empty()) {
                content_arr.push_back({
                    {"type", "text"     },
                    {"text", msg.content}
                });
            }
            for (const auto& url : msg.image_urls) {
                appendSource(url, "image");
            }
            for (const auto& url : msg.audio_urls) {
                appendSource(url, "audio");
            }
            for (const auto& url : msg.video_urls) {
                appendSource(url, "video");
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

    // Anthropic 要求 user/assistant 严格交替出现: "tool" 消息映射为 user 后可能出现连续
    // 同 role (如多个连续 tool 结果、user 后紧跟 tool 结果), 合并相邻同 role 消息,
    // 合并时将 content 统一规范化为 block 数组再拼接。
    // 注: neograph::json 为值语义 (back()/迭代返回深拷贝), 用 pending 暂存待合并消息
    auto normalizeContent = [](neograph::json m) -> neograph::json {
        if (!m["content"].is_array()) {
            std::string text
                = m["content"].is_string() ? m["content"].get<std::string>() : m["content"].dump();
            m["content"] = neograph::json::array();
            if (!text.empty()) {
                m["content"].push_back({
                    {"type", "text"},
                    {"text", text  }
                });
            }
        }
        return m;
    };
    neograph::json merged     = neograph::json::array();
    bool           hasPending = false;
    neograph::json pending;
    auto           flushPending = [&]() {
        if (hasPending) {
            merged.push_back(std::move(pending));
            hasPending = false;
        }
    };
    for (auto msg : arr) {
        auto role = msg["role"].get<std::string>();
        if (hasPending && pending["role"].get<std::string>() == role) {
            pending = normalizeContent(std::move(pending));
            msg     = normalizeContent(std::move(msg));
            for (auto block : msg["content"]) {
                pending["content"].push_back(std::move(block));
            }
        } else {
            flushPending();
            pending    = std::move(msg);
            hasPending = true;
        }
    }
    flushPending();

    return {system, std::move(merged)};
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
                // 记录带 signature 的原始块, 多轮对话时原样回传 (Anthropic 要求携带 signature)
                if (block.contains("signature") && block["signature"].is_string()
                    && !block["signature"].get<std::string>().empty()) {
                    appendThinkingBlock(completion, block);
                }
            } else if (type == "redacted_thinking") {
                // redacted_thinking 块必须原样回传
                appendThinkingBlock(completion, block);
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
    if (config_.modelContenxtMaxToken > 0) {
        body["max_tokens"] = config_.modelContenxtMaxToken;
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

    // Anthropic 强制要求 max_tokens, 缺失会返回 400; config/params/extra 均未指定时使用保守默认值
    if (!body.contains("max_tokens")) {
        body["max_tokens"] = 8192;
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
        fmt::format("{}/v1/messages", config_.baseUrl),
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
        throw std::runtime_error(fmt::format("HTTP request failed: {}", resp.error()));
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
        throw neograph::RateLimitError(fmt::format("API error (HTTP 429): {}", r.body), retryAfter);
    }

    if (r.status != 200) {
        throw std::runtime_error(fmt::format("API error (HTTP {}): {}", r.status, r.body));
    }

    auto respJson   = neograph::json::parse(r.body);
    auto completion = parseResponse(respJson);
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
    std::map<int, std::string>        thinkingTexts;
    std::map<int, std::string>        blockSignatures;
    std::string                       lineBuffer;
    // 是否收到 Anthropic SSE 结束事件 "message_stop", 用于检测流截断
    bool messageStopReceived = false;

    try {
        co_await HttpClient::requestSseAsync(
            "POST",
            fmt::format("{}/v1/messages", config_.baseUrl),
            bodyStr,
            "application/json",
            headers,
            HttpClient::RequestConfig{
                .connectTimeout   = std::chrono::seconds{config_.connectTimeoutSeconds},
                .readChunkTimeout = std::chrono::seconds{config_.readChunkTimeoutSeconds},
                .sslVerify        = config_.sslVerify,
            },
            // 返回 true 通知 http 层流已结束 (收到 message_stop): 立即断开连接停止读取,
            // 避免对端 keep-alive 不关闭时白等 readChunkTimeout
            [&](std::string_view chunk) -> bool {
                lineBuffer += chunk;
                if (processSseBuffer(
                        lineBuffer,
                        completion,
                        fullContent,
                        fullThinking,
                        tcMap,
                        blockTypes,
                        thinkingTexts,
                        blockSignatures,
                        on_chunk
                    )) {
                    messageStopReceived = true;
                    return true;
                }
                return false;
            }
        );
    } catch (const std::exception& e) {
        // 已收到 message_stop 说明消息已结束、业务数据已全部送达, 连接层在收尾阶段的
        // 错误 (如 ssl stream_truncated: 对端未发 close_notify 就关闭连接) 不应使请求失败
        if (!messageStopReceived) {
            throw;
        }
        XX_LOGW(
            "LLM stream transport error after message_stop, ignored | model={} err={}",
            params.model.empty() ? config_.modelName : params.model,
            e.what()
        );
    }

    if (!lineBuffer.empty()) {
        if (processSseBuffer(
                lineBuffer,
                completion,
                fullContent,
                fullThinking,
                tcMap,
                blockTypes,
                thinkingTexts,
                blockSignatures,
                on_chunk,
                /*finalFlush=*/true
            )) {
            messageStopReceived = true;
        }
    }

    // 未收到 message_stop 即视为流被截断: 长连接被中间代理/网关中断、或对端提前关闭
    // connection-close 定长的响应时, HTTP 层可能仍判定"完整", 必须在 SSE 协议层检测,
    // 否则会把截断的响应静默当作正常结果返回
    if (!messageStopReceived) {
        throw std::runtime_error(fmt::format(
            "SSE stream truncated: missing message_stop event | model={} content_chars={}",
            params.model.empty() ? config_.modelName : params.model,
            fullContent.size()
        ));
    }

    completion.message.content           = fullContent;
    completion.message.reasoning_content = fullThinking;
    for (auto& [idx, tc] : tcMap) {
        if (tc.id.empty()) {
            tc.id = fmt::format("call_{}", idx);
        }
        completion.message.tool_calls.push_back(std::move(tc));
    }

    co_return completion;
}

} // namespace server
} // namespace agentxx
