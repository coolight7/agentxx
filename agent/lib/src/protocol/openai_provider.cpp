#include "agentxx/protocol/openai_provider.h"
#include "agentxx/util/exception.h"
#include "fmt/format.h"
#include <chrono>
#include <random>

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
    if (config_.type == "openai-responses") {
        return "openai-responses";
    }
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
    XX_LOGT("OpenAIProvider::invoke_format_data START");
    if (config_.isOpenaiResponseApi()) {
        if (on_chunk) {
            auto body      = buildResponsesBody(params);
            body["stream"] = true;
            co_return co_await doStreamResponses(params, body, on_chunk);
        }
        co_return co_await completeAsyncResponses(params);
    }

    if (on_chunk) {
        auto body              = buildBody(params);
        body["stream"]         = true;
        body["stream_options"] = {
            {"include_usage", true}
        };
        co_return co_await doStream(params, body, on_chunk);
    }
    co_return co_await completeAsync(params);
}

OpenAIProvider::OpenAIProvider(agentxx::agent::ModelConfig config) :
    config_(std::move(config)) {
    if (config_.baseUrl.empty()) {
        config_.baseUrl = kDefaultBaseUrl;
    }
    // 去除 baseUrl 末尾 '/', 避免拼接出 "//chat/completions" 双斜杠
    while (!config_.baseUrl.empty() && config_.baseUrl.back() == '/') {
        config_.baseUrl.pop_back();
    }
}

std::string OpenAIProvider::apiUrl() const {
    std::string path = config_.apiPath;
    if (path.empty()) {
        path = config_.isOpenaiResponseApi() ? kDefaultResponsesPath : kDefaultApiPath;
    }
    // 确保以 '/' 开头
    if (path.empty() || path.front() != '/') {
        path = fmt::format("/{}", path);
    }
    return fmt::format("{}{}", config_.baseUrl, path);
}

void OpenAIProvider::applyHeaders(agentxx::util::HeaderMap& headers) const {
    // apiKey 为空时不发送 Authorization (部分本地服务/网关对空 Bearer 报错)
    if (!config_.apiKey.empty()) {
        headers.set("Authorization", fmt::format("Bearer {}", config_.apiKey));
    }
    // 自定义请求头 (可覆盖 Authorization, 适配网关/自定义鉴权)
    for (const auto& [k, v] : config_.extraHeaders) {
        headers.set(k, v);
    }
}

/// 安全提取 Responses API 事件的 output_index: 缺失/非数字时返回 0
/// (j.value("output_index", 0) 在字段为字符串等类型时会抛异常)
static int safeOutputIndex(const neograph::json& j) {
    if (j.contains("output_index") && j["output_index"].is_number_integer()) {
        return j["output_index"].get<int>();
    }
    return 0;
}

/// 从 JSON 对象中安全提取字符串字段: 字符串直接返回, 数字/对象等转为 dump,
/// 缺失/null 返回空 (json::value(key, "") 在类型不匹配时会抛异常)
static std::string jsonStrField(const neograph::json& obj, const char* key) {
    if (obj.is_object() && obj.contains(key)) {
        const auto& v = obj[key];
        if (v.is_string()) {
            return v.get<std::string>();
        }
        if (!v.is_null()) {
            return v.dump();
        }
    }
    return {};
}

/// 从 JSON 对象中安全提取整数字段: 兼容数字与字符串数字 (个别网关把 token 数
/// 序列化为字符串), 缺失/无法解析时返回 def
static int jsonIntField(const neograph::json& obj, const char* key, int def = 0) {
    if (obj.is_object() && obj.contains(key)) {
        const auto& v = obj[key];
        if (v.is_number_integer()) {
            return v.get<int>();
        }
        if (v.is_number()) {
            return static_cast<int>(v.get<double>());
        }
        if (v.is_string()) {
            std::string s   = v.get<std::string>();
            int         out = def;
            auto [ptr, ec]  = std::from_chars(s.data(), s.data() + s.size(), out);
            if (ec == std::errc{}) {
                return out;
            }
        }
    }
    return def;
}

/// 从 Responses API 的 usage 对象中提取 token 统计
/// - 官方字段: input_tokens / output_tokens / total_tokens
/// - 兼容部分网关沿用 Chat Completions 的 prompt_tokens / completion_tokens 命名
static void parseResponsesUsage(const neograph::json& u, neograph::ChatCompletion& completion) {
    if (!u.is_object()) {
        return;
    }
    completion.usage.prompt_tokens
        = jsonIntField(u, "input_tokens", jsonIntField(u, "prompt_tokens"));
    completion.usage.completion_tokens
        = jsonIntField(u, "output_tokens", jsonIntField(u, "completion_tokens"));
    completion.usage.total_tokens = jsonIntField(
        u,
        "total_tokens",
        completion.usage.prompt_tokens + completion.usage.completion_tokens
    );
}

/// 新模型 (o1/o3/o4/gpt-5 等) 只接受 max_completion_tokens 字段, 旧模型使用 max_tokens
static bool modelUsesMaxCompletionTokens(std::string_view model) {
    if (model.find("gpt-5") != std::string_view::npos) {
        return true;
    }
    // o1-preview / o3-mini / o4-mini 等以 o1/o3/o4 开头的推理模型
    return model.starts_with("o1") || model.starts_with("o3") || model.starts_with("o4");
}

/// agentxx 内部配置字段 (无对应 API 语义), 透传 extra_config 时必须过滤:
/// 原样发送给上游会触发部分模型 (如 gpt-5.6-luna) HTTP 400 报错
static bool isInternalExtraConfigField(std::string_view key) {
    return key == "preserve_thinking";
}

std::string OpenAIProvider::mapStopReason(std::string_view finishReason) {
    if (finishReason == "stop") {
        return "end_turn";
    }
    if (finishReason == "length") {
        return "max_tokens";
    }
    if (finishReason == "tool_calls" || finishReason == "function_call") {
        return "tool_use";
    }
    if (finishReason == "content_filter") {
        return "content_filter";
    }
    if (finishReason == "refusal") {
        return "refusal";
    }
    return "unknown";
}

std::string OpenAIProvider::extractApiError(const std::string& body) {
    // 解析/提取失败 (非法 JSON、字段类型异常) 时回退返回原 body
    return agentxx::util::catchError<std::string>(
        [&body]() -> std::string {
            auto j = neograph::json::parse(body);
            if (j.is_object() && j.contains("error")) {
                auto e = j["error"];
                if (e.is_object()) {
                    std::string msg = e.value("message", std::string{});
                    if (e.contains("code") && !e["code"].is_null()) {
                        std::string code = e["code"].is_string() ? e["code"].get<std::string>()
                                                                 : e["code"].dump();
                        if (!code.empty()) {
                            msg += fmt::format(" (code: {})", code);
                        }
                    }
                    if (!msg.empty()) {
                        return msg;
                    }
                } else if (e.is_string()) {
                    return e.get<std::string>();
                }
            }
            // 部分网关使用顶层 {"message": "..."} (无 error 包裹)
            if (j.is_object() && j.contains("message") && j["message"].is_string()) {
                auto msg = j["message"].get<std::string>();
                if (!msg.empty()) {
                    return msg;
                }
            }
            return body;
        },
        [&body](std::string) {
            return body;
        }
    );
}

namespace {

// 生成唯一的 tool_call id: 毫秒时间戳 + 32 位随机数
// - 无需与已有 id 比较, 碰撞概率 ~2^-32 (同一毫秒内), 跨毫秒必然不同
// - 相比按下标回填 call_{i}, 不会与 LLM 返回的 call_N 形式 id 冲突
std::string makeUniqueToolCallId(size_t i = 0) {
    thread_local std::mt19937_64 rng{
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
    };
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
    )
                        .count();
    return fmt::format("call_{}_{}_{:08x}", ts, i, static_cast<uint32_t>(rng()));
}

} // namespace

void OpenAIProvider::fillMissingToolCallIds(neograph::ChatCompletion& completion) {
    // 缺失 id 用时间戳+随机数回填, 天然唯一, 无需与已有 id 比较
    size_t index = 0;
    for (auto& tc : completion.message.tool_calls) {
        if (tc.id.empty()) {
            tc.id = makeUniqueToolCallId(++index);
        }
    }
}

neograph::json OpenAIProvider::buildBody(const neograph::CompletionParams& params) const {
    neograph::json body;
    body["model"]    = params.model.empty() ? config_.modelName : params.model;
    body["messages"] = neograph::messages_to_json(params.messages);

    if (!config_.sendThinking) {
        const auto&    src     = body["messages"];
        neograph::json cleaned = neograph::json::array();
        for (const auto& val : src) {
            neograph::json obj = neograph::json::object();
            for (const auto& [k, v] : val.items()) {
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

    // 发送输出 token 上限
    // - 新模型 (o1/o3/o4/gpt-5 等) 只接受 max_completion_tokens 字段, 按模型名自动切换
    if (params.max_tokens > 0) {
        const std::string& model = params.model.empty() ? config_.modelName : params.model;
        if (modelUsesMaxCompletionTokens(model)) {
            body["max_completion_tokens"] = params.max_tokens;
        } else {
            body["max_tokens"] = params.max_tokens;
        }
    }

    if (config_.extra_config.is_object()) {
        for (const auto& [key, val] : config_.extra_config.items()) {
            if (body.contains(key)) {
                continue;
            }
            // 过滤 agentxx 内部字段, 避免透传给上游导致 400
            if (isInternalExtraConfigField(key)) {
                continue;
            }
            // 避免同时出现互斥的输出 token 上限字段
            if ((key == "max_tokens" || key == "max_output_tokens" || key == "max_completion_tokens"
                )
                && (body.contains("max_output_tokens") || body.contains("max_completion_tokens")
                    || body.contains("max_tokens"))) {
                continue;
            }
            body[key] = val;
        }
    }

    if (!params.extra_fields.empty()) {
        for (const auto& [key, val] : params.extra_fields.items()) {
            body[key] = val;
        }
    }

    return body;
}

neograph::json OpenAIProvider::buildResponsesBody(const neograph::CompletionParams& params) const {
    neograph::json body;
    body["model"] = params.model.empty() ? config_.modelName : params.model;

    // Codex/Responses API 默认行为: 不落盘
    // (reasoning 思考配置不再硬编码, 通过 extra_config / params.extra_fields 的
    //  "reasoning" 字段控制: {"effort": "none|minimal|low|medium|high|xhigh",
    //   "summary": "detailed|auto|concise"})
    body["store"] = false;

    // 需要回传/展示思考内容时, 请求 reasoning 摘要:
    //   include 取官方值 "reasoning.summary_text" (流式事件
    //   response.reasoning_summary_text.delta / 非流式 output 的 reasoning_summary item);
    //   "reasoning.summary" 不是合法 include 值, 会导致 API 400
    if (config_.sendThinking) {
        body["include"] = neograph::json::array({"reasoning.summary_text"});
    }

    // system 消息 → instructions; 其余 → input 数组 (含 function_call / function_call_output)
    std::string instructions;
    auto        input = neograph::json::array();
    for (const auto& msg : params.messages) {
        if (msg.role == "system") {
            if (!instructions.empty()) {
                instructions += "\n";
            }
            instructions += msg.content;
            continue;
        }
        if (msg.role == "user") {
            neograph::json item;
            item["role"] = "user";
            if (!msg.image_urls.empty() || !msg.audio_urls.empty() || !msg.video_urls.empty()) {
                neograph::json parts = neograph::json::array();
                if (!msg.content.empty()) {
                    parts.push_back({
                        {"type", "input_text"},
                        {"text", msg.content }
                    });
                }
                for (const auto& url : msg.image_urls) {
                    // Responses API 的 input_image: image_url 为字符串 + 可选 detail
                    parts.push_back({
                        {"type",      "input_image"},
                        {"image_url", url          },
                        {"detail",    "auto"       }
                    });
                }
                // Responses API 的 input_audio 只接受 base64 data + format,
                // data URL 解析后转换; HTTP URL 以 url 扩展字段透传 (兼容网关)
                for (const auto& url : msg.audio_urls) {
                    if (auto parsed = neograph::parse_data_url(url)) {
                        parts.push_back({
                            {"type",        "input_audio"                                 },
                            {"input_audio",
                             {{"data", parsed->second},
                              {"format", neograph::media_format_from_mime(parsed->first)}}},
                        });
                    } else {
                        parts.push_back({
                            {"type",        "input_audio" },
                            {"input_audio", {{"url", url}}},
                        });
                    }
                }
                // Responses API 的 input_video: video_url 接受 HTTP URL 或 data URL,
                // format 由 data URL 的 media type 推导; HTTP URL 无 mime 信息时省略 format
                for (const auto& url : msg.video_urls) {
                    neograph::json video = neograph::json::object();
                    video["type"]        = "input_video";
                    video["video_url"]   = url;
                    if (auto parsed = neograph::parse_data_url(url)) {
                        video["format"] = neograph::media_format_from_mime(parsed->first);
                    }
                    parts.push_back(std::move(video));
                }
                item["content"] = std::move(parts);
            } else {
                item["content"] = msg.content;
            }
            input.push_back(std::move(item));
        } else if (msg.role == "assistant") {
            if (!msg.content.empty()) {
                neograph::json content = neograph::json::array();
                content.push_back({
                    {"type", "output_text"},
                    {"text", msg.content  }
                });
                neograph::json item;
                item["role"]    = "assistant";
                item["content"] = std::move(content);
                input.push_back(std::move(item));
            }
            // 回传历史 thinking 内容 (仅 sendThinking 开启时):
            // Responses API 的 reasoning item 采用 summary 形式,
            // 缺失原始 id 时网关按摘要处理; 不回传完整 reasoning 文本
            if (config_.sendThinking && !msg.reasoning_content.empty()) {
                input.push_back({
                    {"type",    "reasoning"},
                    {"summary",
                     neograph::json::array(
                         {{{"type", "summary_text"}, {"text", msg.reasoning_content}}}
                     )                     },
                });
            }
            for (const auto& tc : msg.tool_calls) {
                input.push_back({
                    {"type",      "function_call"},
                    {"call_id",   tc.id          },
                    {"name",      tc.name        },
                    {"arguments", tc.arguments   }
                });
            }
        } else if (msg.role == "tool") {
            input.push_back({
                {"type",    "function_call_output"},
                {"call_id", msg.tool_call_id      },
                {"output",  msg.content           }
            });
        }
    }
    body["input"] = std::move(input);
    if (!instructions.empty()) {
        body["instructions"] = instructions;
    }

    // Responses API 的 tool 定义: {type:"function", name, description, parameters}
    if (!params.tools.empty()) {
        neograph::json tools = neograph::json::array();
        for (const auto& tool : params.tools) {
            neograph::json t;
            t["type"]        = "function";
            t["name"]        = tool.name;
            t["description"] = tool.description;
            t["parameters"]  = tool.parameters;
            tools.push_back(std::move(t));
        }
        body["tools"]       = std::move(tools);
        body["tool_choice"] = "auto";
    }

    if (params.max_tokens > 0) {
        body["max_output_tokens"] = params.max_tokens;
    }

    if (config_.extra_config.is_object()) {
        for (const auto& [key, val] : config_.extra_config.items()) {
            if (body.contains(key)) {
                continue;
            }
            // 过滤 agentxx 内部字段, 避免透传给上游导致 400
            if (isInternalExtraConfigField(key)) {
                continue;
            }
            body[key] = val;
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
    applyHeaders(headers);

    auto resp = co_await HttpClient::postAsync(
        apiUrl(),
        bodyStr,
        "application/json",
        headers,
        HttpClient::RequestConfig{
            .connectTimeout           = std::chrono::seconds{config_.connectTimeoutSeconds},
            .readChunkTimeout         = std::chrono::seconds{config_.readChunkTimeoutSeconds},
            .sslVerify                = config_.sslVerify,
            .keepAlive                = true,
            .maxConcurrentConnections = config_.maxConcurrentConnections,
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

    // 部分网关返回 201/202 等其它 2xx 状态码也视为成功
    if (r.status / 100 != 2) {
        throw std::runtime_error(
            fmt::format("API error (HTTP {}): {}", r.status, extractApiError(r.body))
        );
    }

    // 网关可能在 200 响应中返回 HTML 错误页/截断的 JSON, 解析失败需给出可读错误
    auto respJson = agentxx::util::catchError<neograph::json>(
        [&r]() -> neograph::json {
            return neograph::json::parse(r.body);
        },
        [&r](std::string errInfo) -> neograph::json {
            throw std::runtime_error(fmt::format(
                "API error (HTTP {}): invalid JSON response ({}): {}",
                r.status,
                errInfo,
                std::string_view{r.body}.substr(0, 512)
            ));
        }
    );

    // 校验响应形状: 缺失 choices 时给出可读错误, 而不是让 .at() 抛出晦涩的 json 异常
    if (!respJson.is_object() || !respJson.contains("choices") || !respJson["choices"].is_array()
        || respJson["choices"].empty()) {
        throw std::runtime_error(fmt::format(
            "API error (HTTP {}): malformed response, missing choices: {}",
            r.status,
            extractApiError(r.body)
        ));
    }
    auto choice = respJson["choices"][0];
    if (!choice.is_object() || !choice.contains("message") || !choice["message"].is_object()) {
        throw std::runtime_error(fmt::format(
            "API error (HTTP {}): malformed response, missing message in choice[0]",
            r.status
        ));
    }

    neograph::ChatCompletion completion;
    completion.message = neograph::parse_response_message(choice);

    // finish_reason → stop_reason 归一化 (部分网关返回非字符串类型, 仅接受字符串)
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
        completion.stop_reason = mapStopReason(choice["finish_reason"].get<std::string>());
    }

    if (completion.message.reasoning_content.empty()) {
        extractThinkTags(completion.message.content, completion.message.reasoning_content);
    }

    if (completion.message.reasoning_content.empty()) {
        if (choice.contains("reasoning_content") && choice["reasoning_content"].is_string()) {
            completion.message.reasoning_content = choice["reasoning_content"].get<std::string>();
        } else if (choice.contains("thinking") && choice["thinking"].is_string()) {
            completion.message.reasoning_content = choice["thinking"].get<std::string>();
        }
    }

    if (respJson.contains("usage") && respJson["usage"].is_object()) {
        auto u                             = respJson["usage"];
        completion.usage.prompt_tokens     = jsonIntField(u, "prompt_tokens");
        completion.usage.completion_tokens = jsonIntField(u, "completion_tokens");
        completion.usage.total_tokens      = jsonIntField(u, "total_tokens");
    }

    // 非流式 tool_calls 缺失 id 时同样回填 call_N (与流式路径一致)
    fillMissingToolCallIds(completion);

    co_return completion;
}

asio::awaitable<neograph::ChatCompletion>
    OpenAIProvider::completeAsyncResponses(const neograph::CompletionParams& params) {
    using namespace agentxx::util;

    auto bodyJson = buildResponsesBody(params);
    auto bodyStr  = bodyJson.dump();

    HeaderMap headers;
    applyHeaders(headers);

    auto resp = co_await HttpClient::postAsync(
        apiUrl(),
        bodyStr,
        "application/json",
        headers,
        HttpClient::RequestConfig{
            .connectTimeout           = std::chrono::seconds{config_.connectTimeoutSeconds},
            .readChunkTimeout         = std::chrono::seconds{config_.readChunkTimeoutSeconds},
            .sslVerify                = config_.sslVerify,
            .keepAlive                = true,
            .maxConcurrentConnections = config_.maxConcurrentConnections,
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

    // 部分网关返回 201/202 等其它 2xx 状态码也视为成功
    if (r.status / 100 != 2) {
        throw std::runtime_error(
            fmt::format("API error (HTTP {}): {}", r.status, extractApiError(r.body))
        );
    }

    auto respJson = agentxx::util::catchError<neograph::json>(
        [&r]() -> neograph::json {
            return neograph::json::parse(r.body);
        },
        [&r](std::string errInfo) -> neograph::json {
            throw std::runtime_error(fmt::format(
                "API error (HTTP {}): invalid JSON response ({}): {}",
                r.status,
                errInfo,
                r.body.substr(0, 512)
            ));
        }
    );

    // Responses API 错误可能以 200 + status="failed"/顶层 error 对象的形式返回
    if (respJson.is_object()) {
        auto respStatus = jsonStrField(respJson, "status");
        if (respStatus == "failed"
            || (respJson.contains("error") && !respJson["error"].is_null())) {
            throw std::runtime_error(
                fmt::format("API error (HTTP {}): {}", r.status, extractApiError(r.body))
            );
        }
    }

    neograph::ChatCompletion completion;
    completion.message.role = "assistant";

    bool hasToolCall = false;
    if (respJson.contains("output") && respJson["output"].is_array()) {
        for (const auto& item : respJson["output"]) {
            if (!item.is_object()) {
                continue;
            }
            auto type = jsonStrField(item, "type");
            if (type == "message") {
                if (item.contains("content") && item["content"].is_array()) {
                    for (const auto& part : item["content"]) {
                        auto ptype = jsonStrField(part, "type");
                        if (ptype == "output_text") {
                            completion.message.content += jsonStrField(part, "text");
                        } else if (ptype == "refusal") {
                            // 模型拒绝回答: 将 refusal 文本透出, 避免静默丢失
                            completion.message.content += jsonStrField(part, "refusal");
                        }
                    }
                }
            } else if (type == "function_call") {
                hasToolCall = true;
                neograph::ToolCall tc;
                tc.id = jsonStrField(item, "call_id");
                if (tc.id.empty()) {
                    tc.id = jsonStrField(item, "id");
                }
                tc.name      = jsonStrField(item, "name");
                tc.arguments = jsonStrField(item, "arguments");
                completion.message.tool_calls.push_back(std::move(tc));
            } else if (type == "reasoning") {
                if (item.contains("content") && item["content"].is_array()) {
                    for (const auto& part : item["content"]) {
                        if (jsonStrField(part, "type") == "reasoning_text") {
                            completion.message.reasoning_content += jsonStrField(part, "text");
                        }
                    }
                }
            } else if (type == "reasoning_summary") {
                // 摘要型推理: summary 数组元素 {type:"summary_text", text}
                if (item.contains("summary") && item["summary"].is_array()) {
                    for (const auto& part : item["summary"]) {
                        if (jsonStrField(part, "type") == "summary_text") {
                            completion.message.reasoning_content += jsonStrField(part, "text");
                        }
                    }
                }
            }
        }
    }

    // Responses API usage: input_tokens / output_tokens / total_tokens
    // (兼容部分网关沿用 Chat Completions 的 prompt_tokens/completion_tokens 命名)
    if (respJson.contains("usage") && respJson["usage"].is_object()) {
        parseResponsesUsage(respJson["usage"], completion);
    }

    fillMissingToolCallIds(completion);

    if (completion.message.reasoning_content.empty()) {
        extractThinkTags(completion.message.content, completion.message.reasoning_content);
    }

    completion.stop_reason = hasToolCall ? "tool_use" : "end_turn";

    co_return completion;
}

asio::awaitable<neograph::ChatCompletion> OpenAIProvider::doStream(
    const neograph::CompletionParams&  params,
    const neograph::json&              body,
    neograph::FormatDataStreamCallback on_chunk
) {
    XX_LOGT("OpenAIProvider::doStream START");
    using namespace agentxx::util;

    auto bodyStr = body.dump();

    HeaderMap headers;
    applyHeaders(headers);

    neograph::ChatCompletion completion;
    completion.message.role = "assistant";
    std::string                       fullContent;
    std::string                       fullThinking;
    std::map<int, neograph::ToolCall> tcMap;
    std::string                       lineBuffer;
    // 是否收到 OpenAI SSE 结束标记 "data: [DONE]", 用于检测流截断
    bool doneReceived = false;

    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
            co_await HttpClient::requestSseAsync(
                "POST",
                apiUrl(),
                bodyStr,
                "application/json",
                headers,
                HttpClient::RequestConfig{
                    .connectTimeout   = std::chrono::seconds{config_.connectTimeoutSeconds},
                    .readChunkTimeout = std::chrono::seconds{config_.readChunkTimeoutSeconds},
                    .sslVerify        = config_.sslVerify,
                    .keepAlive        = true,
                    .maxConcurrentConnections = config_.maxConcurrentConnections,
                },
                // 返回 true 通知 http 层流已结束 (收到 [DONE]): 立即断开连接停止读取,
                // 避免对端 keep-alive 不关闭时白等 readChunkTimeout
                [&](std::string_view chunk) -> bool {
                    lineBuffer += chunk;
                    if (processSseBuffer(
                            lineBuffer,
                            completion,
                            fullContent,
                            fullThinking,
                            tcMap,
                            on_chunk
                        )) {
                        doneReceived = true;
                        return true;
                    }
                    return false;
                }
            );

            XX_LOGT("OpenAIProvider::doStream [HttpClient::requestSseAsync] DONE");
            co_return true;
        },
        [&](std::string errinfo) -> asio::awaitable<bool> {
            XX_LOGT(
                "OpenAIProvider::doStream [HttpClient::requestSseAsync] Exception: {}",
                errinfo
            );
            // 已收到 [DONE] 说明业务数据已全部送达, 连接层在收尾阶段的错误
            // (如 ssl stream_truncated: 对端未发 close_notify 就关闭连接) 不应使请求失败
            if (!doneReceived) {
                throw std::runtime_error{errinfo};
            }
            XX_LOGW("LLM stream transport error after [DONE], ignored");
            co_return false;
        },
        /*onRethrow=*/nullptr,
        /*cancelToken=*/params.cancel_token
    );

    if (!lineBuffer.empty()) {
        if (processSseBuffer(
                lineBuffer,
                completion,
                fullContent,
                fullThinking,
                tcMap,
                on_chunk,
                /*finalFlush=*/true
            )) {
            doneReceived = true;
        }
    }

    // 未收到 [DONE] 即视为流被截断: 长连接被中间代理/网关中断、或对端提前关闭
    // connection-close 定长的响应时, HTTP 层可能仍判定"完整", 必须在 SSE 协议层检测,
    // 否则会把截断的响应静默当作正常结果返回
    if (!doneReceived) {
        XX_LOGT("OpenAIProvider::doStream throw exception: !doneReceived");
        throw std::runtime_error(fmt::format(
            "SSE stream truncated: missing [DONE] marker | model={} content_chars={}",
            params.model.empty() ? config_.modelName : params.model,
            fullContent.size()
        ));
    }

    if (fullThinking.empty()) {
        extractThinkTags(fullContent, fullThinking);
    }
    completion.message.content           = fullContent;
    completion.message.reasoning_content = fullThinking;
    // 回填缺失的 tool_call id: 时间戳+随机数生成, 天然唯一, 无需与已有 id 比较
    size_t index = 0;
    for (auto& [idx, tc] : tcMap) {
        if (tc.id.empty()) {
            tc.id = makeUniqueToolCallId(++index);
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

    XX_LOGT("OpenAIProvider::doStream END");
    co_return completion;
}

asio::awaitable<neograph::ChatCompletion> OpenAIProvider::doStreamResponses(
    const neograph::CompletionParams&  params,
    const neograph::json&              body,
    neograph::FormatDataStreamCallback on_chunk
) {
    using namespace agentxx::util;

    auto bodyStr = body.dump();

    HeaderMap headers;
    applyHeaders(headers);

    neograph::ChatCompletion completion;
    completion.message.role = "assistant";
    std::string                       fullContent;
    std::string                       fullThinking;
    std::map<int, neograph::ToolCall> tcMap;
    std::string                       lineBuffer;
    std::string                       apiError;
    // 是否收到 Responses API 结束事件 "response.completed", 用于检测流截断
    bool doneReceived = false;

    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
            co_await HttpClient::requestSseAsync(
                "POST",
                apiUrl(),
                bodyStr,
                "application/json",
                headers,
                HttpClient::RequestConfig{
                    .connectTimeout   = std::chrono::seconds{config_.connectTimeoutSeconds},
                    .readChunkTimeout = std::chrono::seconds{config_.readChunkTimeoutSeconds},
                    .sslVerify        = config_.sslVerify,
                    .keepAlive        = true,
                    .maxConcurrentConnections = config_.maxConcurrentConnections,
                },
                // 返回 true 通知 http 层流已结束 (收到 response.completed): 立即断开
                [&](std::string_view chunk) -> bool {
                    lineBuffer += chunk;
                    if (processResponsesSseBuffer(
                            lineBuffer,
                            completion,
                            fullContent,
                            fullThinking,
                            tcMap,
                            on_chunk,
                            /*finalFlush=*/false,
                            &apiError
                        )) {
                        doneReceived = true;
                        return true;
                    }
                    return false;
                }
            );

            XX_LOGT("OpenAIProvider::doStreamResponses [HttpClient::requestSseAsync] DONE");
            co_return true;
        },
        [&](std::string errinfo) -> asio::awaitable<bool> {
            XX_LOGT(
                "OpenAIProvider::doStreamResponses [HttpClient::requestSseAsync] Exception: {}",
                errinfo
            );
            // 已收到 response.completed 说明业务数据已全部送达, 连接层在收尾阶段的错误
            // (如 ssl stream_truncated) 不应使请求失败
            if (!doneReceived) {
                throw std::runtime_error{errinfo};
            }
            XX_LOGW("LLM stream transport error after response.completed, ignored");
            co_return false;
        },
        /*onRethrow=*/nullptr,
        /*cancelToken=*/params.cancel_token
    );

    if (!lineBuffer.empty()) {
        if (processResponsesSseBuffer(
                lineBuffer,
                completion,
                fullContent,
                fullThinking,
                tcMap,
                on_chunk,
                /*finalFlush=*/true,
                &apiError
            )) {
            doneReceived = true;
        }
    }

    // 服务端通过 response.failed / error 事件上报错误
    if (!apiError.empty()) {
        throw std::runtime_error(fmt::format("Responses API stream error: {}", apiError));
    }

    if (!doneReceived) {
        throw std::runtime_error(fmt::format(
            "SSE stream truncated: missing response.completed | model={} content_chars={}",
            params.model.empty() ? config_.modelName : params.model,
            fullContent.size()
        ));
    }

    if (fullThinking.empty()) {
        extractThinkTags(fullContent, fullThinking);
    }
    completion.message.content           = fullContent;
    completion.message.reasoning_content = fullThinking;
    // 回填缺失的 tool_call id: 时间戳+随机数生成, 天然唯一, 无需与已有 id 比较
    size_t index = 0;
    for (auto& [idx, tc] : tcMap) {
        if (tc.id.empty()) {
            tc.id = makeUniqueToolCallId(++index);
        }
        completion.message.tool_calls.push_back(std::move(tc));
    }
    completion.stop_reason = tcMap.empty() ? "end_turn" : "tool_use";

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

bool OpenAIProvider::processSseBuffer(
    std::string&                       buf,
    neograph::ChatCompletion&          completion,
    std::string&                       fullContent,
    std::string&                       fullThinking,
    std::map<int, neograph::ToolCall>& tcMap,
    neograph::FormatDataStreamCallback on_chunk,
    bool                               finalFlush
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

bool OpenAIProvider::processSseLine(
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

    // 部分网关会在行尾附加空白, 容忍后再判断结束标记
    while (!payload.empty() && (payload.back() == ' ' || payload.back() == '\t')) {
        payload.pop_back();
    }
    if (payload == "[DONE]") {
        return true;
    }

    // 畸形 data 行 (部分代理/网关会注入非 JSON 内容) 应跳过而不是中断整个流
    // (解析失败返回 null json, 后续 contains() 检查自然跳过)
    auto j = agentxx::util::catchError<neograph::json>(
        [&payload] {
            return neograph::json::parse(payload);
        },
        [](std::string) {
            return neograph::json{};
        }
    );

    if (j.contains("usage") && j["usage"].is_object()) {
        auto u                             = j["usage"];
        completion.usage.prompt_tokens     = jsonIntField(u, "prompt_tokens");
        completion.usage.completion_tokens = jsonIntField(u, "completion_tokens");
        completion.usage.total_tokens      = jsonIntField(
            u,
            "total_tokens",
            completion.usage.prompt_tokens + completion.usage.completion_tokens
        );
    }

    if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) {
        return false;
    }
    auto choice0 = j["choices"][0];
    // 畸形 chunk (choices[0] 非对象) 应跳过而不是让 operator[] 抛异常中断整个流
    if (!choice0.is_object()) {
        return false;
    }

    // 捕获 finish_reason → stop_reason (最后一个携带的 chunk 生效);
    // 部分网关返回非字符串 (null/数字), 仅接受字符串
    if (choice0.contains("finish_reason") && choice0["finish_reason"].is_string()) {
        auto fr = choice0["finish_reason"].get<std::string>();
        if (!fr.empty()) {
            completion.stop_reason = mapStopReason(fr);
        }
    }

    if (!choice0.contains("delta") || !choice0["delta"].is_object()) {
        return false;
    }
    auto delta = choice0["delta"];

    // content 仅接受字符串 (个别网关发送数字/数组等非法类型时跳过, 避免 get 抛异常)
    if (delta.contains("content") && delta["content"].is_string()) {
        std::string token = delta["content"].get<std::string>();
        if (!token.empty()) {
            fullContent += token;
            if (on_chunk) {
                on_chunk(neograph::ChatStreamChunk{neograph::ChatStreamChunk::TYPE_CONTENT, token});
            }
        }
    }

    if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
        auto token = delta["reasoning_content"].get<std::string>();
        if (!token.empty()) {
            fullThinking += token;
            if (on_chunk) {
                on_chunk(neograph::ChatStreamChunk{neograph::ChatStreamChunk::TYPE_THINKING, token}
                );
            }
        }
    } else if (delta.contains("thinking") && delta["thinking"].is_string()) {
        auto token = delta["thinking"].get<std::string>();
        if (!token.empty()) {
            fullThinking += token;
            if (on_chunk) {
                on_chunk(neograph::ChatStreamChunk{neograph::ChatStreamChunk::TYPE_THINKING, token}
                );
            }
        }
    } else if (delta.contains("reasoning") && delta["reasoning"].is_string()) {
        // Vercel AI Gateway / 部分网关使用 delta.reasoning 流式输出推理内容
        auto token = delta["reasoning"].get<std::string>();
        if (!token.empty()) {
            fullThinking += token;
            if (on_chunk) {
                on_chunk(neograph::ChatStreamChunk{neograph::ChatStreamChunk::TYPE_THINKING, token}
                );
            }
        }
    }

    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
        for (const auto& tc : delta["tool_calls"]) {
            if (!tc.is_object()) {
                continue;
            }
            // index 通常为数字, 个别网关发送字符串数字, 两者都兼容
            int idx = 0;
            if (tc.contains("index")) {
                if (tc["index"].is_number_integer()) {
                    idx = tc["index"].get<int>();
                } else if (tc["index"].is_string()) {
                    std::string idxStr = tc["index"].get<std::string>();
                    auto [ptr, ec]
                        = std::from_chars(idxStr.data(), idxStr.data() + idxStr.size(), idx);
                    if (ec != std::errc{}) {
                        idx = 0;
                    }
                }
            }
            if (tc.contains("id")) {
                // 部分提供商返回数字 id, 统一转为字符串 (与下游 tool_call_id 匹配)
                if (tc["id"].is_string()) {
                    tcMap[idx].id = tc["id"].get<std::string>();
                } else if (!tc["id"].is_null()) {
                    tcMap[idx].id = tc["id"].dump();
                }
            }
            if (tc.contains("function") && tc["function"].is_object()) {
                if (tc["function"].contains("name") && tc["function"]["name"].is_string()) {
                    tcMap[idx].name += tc["function"]["name"].get<std::string>();
                }
                if (tc["function"].contains("arguments")
                    && tc["function"]["arguments"].is_string()) {
                    tcMap[idx].arguments += tc["function"]["arguments"].get<std::string>();
                }
            }
        }
    }
    return false;
}

bool OpenAIProvider::processResponsesSseBuffer(
    std::string&                       buf,
    neograph::ChatCompletion&          completion,
    std::string&                       fullContent,
    std::string&                       fullThinking,
    std::map<int, neograph::ToolCall>& tcMap,
    neograph::FormatDataStreamCallback on_chunk,
    bool                               finalFlush,
    std::string*                       errOut
) {
    bool   done = false;
    size_t pos;
    while ((pos = buf.find('\n')) != std::string::npos) {
        std::string line = buf.substr(0, pos);
        buf.erase(0, pos + 1);
        done |= processResponsesSseLine(
            line,
            completion,
            fullContent,
            fullThinking,
            tcMap,
            on_chunk,
            errOut
        );
        // 收到错误事件后停止解析后续行
        if (errOut && !errOut->empty()) {
            break;
        }
    }
    if (finalFlush && !buf.empty()) {
        std::string line = std::move(buf);
        buf.clear();
        done |= processResponsesSseLine(
            line,
            completion,
            fullContent,
            fullThinking,
            tcMap,
            on_chunk,
            errOut
        );
    }
    return done;
}

bool OpenAIProvider::processResponsesSseLine(
    std::string_view                   line_in,
    neograph::ChatCompletion&          completion,
    std::string&                       fullContent,
    std::string&                       fullThinking,
    std::map<int, neograph::ToolCall>& tcMap,
    neograph::FormatDataStreamCallback on_chunk,
    std::string*                       errOut
) {
    std::string line{line_in};
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    // 只关心 data: 行 (event: 行会被跳过)
    if (line.rfind("data:", 0) != 0) {
        return false;
    }
    std::string payload = line.substr(5);
    if (!payload.empty() && payload.front() == ' ') {
        payload.erase(0, 1);
    }
    while (!payload.empty() && (payload.back() == ' ' || payload.back() == '\t')) {
        payload.pop_back();
    }
    // 部分兼容服务仍使用 [DONE] 作为结束标记
    if (payload == "[DONE]") {
        return true;
    }

    // 畸形 data 行应跳过而不是中断整个流
    // (解析失败返回 null json, 后续 contains()/jsonStrField 检查自然跳过)
    const auto j = agentxx::util::catchError<neograph::json>(
        [&payload] {
            return neograph::json::parse(payload);
        },
        [](std::string) {
            return neograph::json{};
        }
    );

    // usage: 官方 Responses API 在结束事件 (response.completed/response.incomplete) 中
    // 将 usage 嵌套在 response 对象内, 即
    // {"type":"response.completed","response":{...,"usage":{...}}}; 部分网关则在事件顶层直接携带
    // usage, 两者都兼容
    if (j.contains("usage") && j["usage"].is_object()) {
        parseResponsesUsage(j["usage"], completion);
    } else if (j.contains("response") && j["response"].is_object()
               && j["response"].contains("usage")) {
        parseResponsesUsage(j["response"]["usage"], completion);
    }

    auto type = jsonStrField(j, "type");

    // 结束标记: response.completed; response.incomplete 表示输出被 max_output_tokens
    // 截断, 同样代表流正常结束 (内容已送达完毕), 视为结束避免误报 truncated
    if (type == "response.completed" || type == "response.incomplete") {
        return true;
    }

    // 错误事件
    if (type == "response.failed" || type == "error") {
        if (errOut) {
            if (j.contains("message") && j["message"].is_string()) {
                *errOut = j["message"].get<std::string>();
            } else if (j.contains("error") && j["error"].is_object()) {
                *errOut = j["error"].value("message", j.dump());
            } else {
                *errOut = j.dump();
            }
        }
        return false;
    }

    // 可见文本增量
    if (type == "response.output_text.delta") {
        if (j.contains("delta") && j["delta"].is_string()) {
            std::string token = j["delta"].get<std::string>();
            if (!token.empty()) {
                fullContent += token;
                if (on_chunk) {
                    on_chunk(
                        neograph::ChatStreamChunk{neograph::ChatStreamChunk::TYPE_CONTENT, token}
                    );
                }
            }
        }
        return false;
    }

    // 推理文本增量 (reasoning_text 与 summary 都视为 thinking)
    if (type == "response.reasoning_text.delta"
        || type == "response.reasoning_summary_text.delta") {
        if (j.contains("delta") && j["delta"].is_string()) {
            std::string token = j["delta"].get<std::string>();
            if (!token.empty()) {
                fullThinking += token;
                if (on_chunk) {
                    on_chunk(
                        neograph::ChatStreamChunk{neograph::ChatStreamChunk::TYPE_THINKING, token}
                    );
                }
            }
        }
        return false;
    }

    // function_call 项开始: 记录 call_id / name
    if (type == "response.output_item.added") {
        if (j.contains("item") && j["item"].is_object()) {
            auto item = j["item"];
            if (jsonStrField(item, "type") == "function_call") {
                int         idx = safeOutputIndex(j);
                auto&       tc  = tcMap[idx];
                std::string id  = jsonStrField(item, "call_id");
                if (id.empty()) {
                    id = jsonStrField(item, "id");
                }
                tc.id   = std::move(id);
                tc.name = jsonStrField(item, "name");
            }
        }
        return false;
    }

    // function_call arguments 增量
    if (type == "response.function_call_arguments.delta") {
        int idx = safeOutputIndex(j);
        if (j.contains("delta") && j["delta"].is_string()) {
            tcMap[idx].arguments += j["delta"].get<std::string>();
        }
        return false;
    }

    // function_call arguments 完成: 携带完整快照, 直接覆盖 (兼容丢帧/乱序)
    if (type == "response.function_call_arguments.done") {
        int idx = safeOutputIndex(j);
        if (j.contains("arguments") && j["arguments"].is_string()) {
            tcMap[idx].arguments = j["arguments"].get<std::string>();
        }
        return false;
    }

    return false;
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

    /// 解析 XML 风格的 <function=name>args</function> tool call
    /// (llama.cpp 等本地模型常见输出, 参数缺失时按空对象 {} 处理, 保证下游 json::parse 可用)
    /// - 从 xmlStr 中提取所有 <function=...> 片段
    /// - xmlStr 需与原始文本长度一致的 lowercase 版本 (lower) 同步查找 (标签不区分大小写)
    auto tryExtractXml = [&](std::string_view xmlStr, std::string_view lower) -> bool {
        size_t from  = 0;
        bool   found = false;
        while (from < xmlStr.size()) {
            auto open = lower.find("<function=", from);
            if (open == std::string::npos) {
                break;
            }
            auto gt = xmlStr.find('>', open);
            if (gt == std::string::npos) {
                break;
            }
            std::string name = trim(xmlStr.substr(open + 10, gt - open - 10));
            if (name.empty()) {
                from = gt + 1;
                continue;
            }
            auto        closeTag = lower.find("</function>", gt + 1);
            size_t      bodyEnd  = closeTag == std::string::npos ? xmlStr.size() : closeTag;
            std::string body     = trim(xmlStr.substr(gt + 1, bodyEnd - gt - 1));

            neograph::ToolCall tc;
            tc.name = std::move(name);
            if (body.empty()) {
                // 模型只输出了函数名: 按空参数对象处理, 避免下游 json::parse 抛错
                tc.arguments = "{}";
            } else {
                auto j = agentxx::util::catchError<neograph::json>(
                    [&body] {
                        return neograph::json::parse(body);
                    },
                    [](std::string) {
                        return neograph::json{};
                    }
                );
                if (j.is_object()) {
                    tc.arguments = j.dump();
                } else if (j.is_string()) {
                    tc.arguments = j.get<std::string>();
                } else {
                    // 非 JSON 内容原样保留, 由下游解析报错兜底
                    tc.arguments = std::move(body);
                }
            }
            tc.id = fmt::format("extr_{}", toolCalls.size());
            toolCalls.push_back(std::move(tc));
            found = true;
            from  = closeTag == std::string::npos ? xmlStr.size() : closeTag + 11;
        }
        return found;
    };

    auto tryExtract = [&](std::string_view jsonStr) -> bool {
        std::string trimmed = trim(jsonStr);
        if (trimmed.empty()) {
            return false;
        }
        auto j = agentxx::util::catchError<neograph::json>(
            [&trimmed] {
                return neograph::json::parse(trimmed);
            },
            [](std::string) {
                return neograph::json{};
            }
        );
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
            tc.id = fmt::format("extr_{}", toolCalls.size());
            toolCalls.push_back(std::move(tc));
            return true;
        }
        return false;
    };

    std::string cleaned;
    size_t      pos      = 0;
    int         numFound = 0;

    /// Phase 0: 提取 <tool_call>...</tool_call> 包裹的 tool call
    /// (llama.cpp 等本地模型常在 thinking/content 末尾输出该 XML 风格格式)
    {
        std::string lower = agentxx::util::toLower(content);
        size_t      p     = 0;
        while (p < content.size()) {
            auto open = lower.find("<tool_call", p);
            if (open == std::string::npos) {
                cleaned += content.substr(p);
                break;
            }
            auto gt = content.find('>', open);
            if (gt == std::string::npos) {
                cleaned += content.substr(p);
                break;
            }
            auto   close    = lower.find("</tool_call>", gt + 1);
            size_t blockEnd = close == std::string::npos ? content.size() : close;
            auto   block    = std::string_view(content).substr(gt + 1, blockEnd - gt - 1);
            bool   extracted
                = tryExtractXml(block, std::string_view(lower).substr(gt + 1, blockEnd - gt - 1));
            if (!extracted && !trim(block).empty()) {
                // <tool_call> 内直接是 JSON (未使用 <function=...> 包裹)
                extracted = tryExtract(block);
            }

            // 始终保留 block 之前的文本
            cleaned += content.substr(p, open - p);
            if (extracted) {
                ++numFound;
            } else {
                // 提取失败: 保留 block 原文 (含前后标签)
                cleaned += content.substr(open, blockEnd + 12 - open);
            }
            p = blockEnd == content.size() ? content.size() : blockEnd + 12;
        }
    }

    /// Phase 0.5: 未用 <tool_call> 包裹的裸 <function=...> 标签
    if (numFound == 0) {
        // Phase 0 未命中时 cleaned 可能为空, 直接以原文继续扫描
        if (cleaned.empty()) {
            cleaned = content;
        }
        std::string lower = agentxx::util::toLower(cleaned);
        std::string tmp;
        size_t      p = 0;
        while (p < cleaned.size()) {
            auto open = lower.find("<function=", p);
            if (open == std::string::npos) {
                tmp += cleaned.substr(p);
                break;
            }
            auto gt = cleaned.find('>', open);
            if (gt == std::string::npos) {
                tmp += cleaned.substr(p);
                break;
            }
            std::string name  = trim(std::string_view(cleaned).substr(open + 10, gt - open - 10));
            tmp              += cleaned.substr(p, open - p);
            if (name.empty()) {
                // 保留空名标签原文, 避免文本丢失
                tmp += cleaned.substr(open, gt + 1 - open);
                p    = gt + 1;
                continue;
            }
            auto   close   = lower.find("</function>", gt + 1);
            size_t bodyEnd = close == std::string::npos ? cleaned.size() : close;
            // 复用 tryExtractXml 解析: 拼成完整 <function=name>body</function> 片段
            std::string piece = fmt::format("{}</function>", cleaned.substr(open, bodyEnd - open));
            if (tryExtractXml(piece, agentxx::util::toLower(piece))) {
                ++numFound;
            } else {
                tmp += piece;
            }
            p = bodyEnd == cleaned.size() ? cleaned.size() : close + 11;
        }
        cleaned = std::move(tmp);
    }

    /// Phase 1/2 均基于原始 content 扫描: 若 XML 阶段已提取成功 (cleaned 已被改写),
    /// 再运行会重复累加文本, 直接收尾返回
    if (numFound > 0) {
        content = trim(cleaned);
        return;
    }

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
            cleaned += fmt::format("```json\n{}\n```", jsonStr);
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
            // 必须用已清理的前缀覆盖 content: 此前已处理过的闭合 think 标签只存在于 cleaned 中,
            // 若用 content.erase(start) 会把原始字符串里已提取的标签残留在 content 中
            content = cleaned;
            return;
        }
        thinking += content.substr(start + 7, end - start - 7);
        pos       = end + 8;
    }
    content = cleaned;
}

} // namespace server
} // namespace agentxx
