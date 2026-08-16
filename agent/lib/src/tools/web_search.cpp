#include "agentxx/tools/web_search.h"

#include "agentxx/protocol/openai_provider.h"
#include "agentxx/util/http_client.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include <charconv>
#include <chrono>
#include <optional>
#include <string>

namespace agentxx {
namespace tools {

namespace {

/// 解析 tool 参数中的 `timeout` (秒)
/// - 支持数字或数字字符串 (部分模型会传字符串)
/// - 非法/缺失时返回默认值; 返回值 <= 0 表示未指定, 使用默认配置
int parseTimeoutArg(const neograph::json& args, int defaultSeconds) {
    if (!args.is_object()) {
        return defaultSeconds;
    }
    const auto v = args["timeout"];
    if (v.is_number()) {
        return static_cast<int>(v.get<double>());
    }
    if (v.is_string()) {
        int         out = 0;
        std::string s   = v.get<std::string>();
        auto [ptr, ec]  = std::from_chars(s.data(), s.data() + s.size(), out);
        if (ec == std::errc{}) {
            return out;
        }
    }
    return defaultSeconds;
}

/// 解析 tool 参数中的 `header` 参数为 HTTP 请求头, 支持三种格式:
/// - JSON 对象: {"User-Agent": "xx", "X-Api-Key": "v"} (非字符串值转为文本)
/// - JSON 字符串数组: ["User-Agent: xx", "X-Api-Key: v"]
/// - JSON 字符串: "User-Agent: xx" (多行时按行解析 "Name: value")
agentxx::util::HeaderMap parseHeaderArg(const neograph::json& args) {
    agentxx::util::HeaderMap headers;
    if (!args.is_object()) {
        return headers;
    }
    const auto headerVal = args["header"];

    // 解析单行 "Name: value"
    auto addHeaderLine = [&headers](std::string line) {
        line = agentxx::util::removeBetweenSpace(line);
        if (line.empty()) {
            return;
        }
        const auto pos = line.find(':');
        if (pos == std::string::npos) {
            return;
        }
        auto name = agentxx::util::removeBetweenSpace(line.substr(0, pos));
        if (name.empty()) {
            return;
        }
        auto value = agentxx::util::removeBetweenSpace(line.substr(pos + 1));
        headers.set(name, value);
    };

    if (headerVal.is_object()) {
        for (const auto& [k, v] : headerVal.items()) {
            if (v.is_string()) {
                headers.set(k, v.get<std::string>());
            } else if (!v.is_null()) {
                headers.set(k, v.dump());
            }
        }
    } else if (headerVal.is_array()) {
        for (const auto& item : headerVal) {
            if (item.is_string()) {
                addHeaderLine(item.get<std::string>());
            }
        }
    } else if (headerVal.is_string()) {
        // 按行解析, 兼容 "\n" 与 "\r\n"
        const std::string str = headerVal.get<std::string>();
        size_t            pos = 0;
        while (pos <= str.size()) {
            const size_t     nl = str.find('\n', pos);
            std::string_view line;
            if (nl == std::string::npos) {
                line = std::string_view{str}.substr(pos);
                pos  = str.size() + 1;
            } else {
                line = std::string_view{str}.substr(pos, nl - pos);
                pos  = nl + 1;
            }
            addHeaderLine(std::string{line});
        }
    }
    return headers;
}

} // namespace

WebSearchTool::WebSearchTool(
    std::string_view                            in_searchApiUrl,
    bool                                        in_convertHtml2markdown,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("agentxx_web_search", in_agentContext, true, true),
    searchApiUrl(in_searchApiUrl),
    convertHtml2markdown(in_convertHtml2markdown) {}

neograph::ChatTool WebSearchTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {{
                     "query",
                     {
                         {"type", "string"},
                         {"description", prompt.getArg("query")},
                     },
                 },
                 {
                     "timeout",
                     {
                         {"type", "number"},
                         {"description", prompt.getArg("timeout")},
                     },
                 },
                 {
                     "header",
                     {
                         {"type", "object"},
                         {"description", prompt.getArg("header")},
                     },
                 }},
            }, {"required", neograph::json::array({"query"})},
                       },
    };
}

asio::awaitable<std::string> WebSearchTool::execute_async(const neograph::json& arguments) {
    std::string query = arguments.value("query", std::string{});
    if (query.empty()) {
        co_return R"({"error":"Arg `query` is empty"})";
    }
    auto search_url
        = fmt::format(fmt::runtime(searchApiUrl), agentxx::util::HttpClient::urlEncode(query));

    // 统一的 timeout / header 参数: 支持自定义请求头与请求超时
    const auto headers = parseHeaderArg(arguments);
    const int  timeout = parseTimeoutArg(arguments, 15);
    auto       config  = agentxx::util::HttpClient::RequestConfig{};
    if (timeout > 0) {
        config.readChunkTimeout = std::chrono::seconds{timeout};
    }

    std::optional<std::string> out_resp_err;
    if (convertHtml2markdown) {
        // 转换 HTML 结果为 Markdown
        auto resp = co_await agentxx::util::HttpClient::fetchMarkdown(search_url, headers, config);
        out_resp_err = resp.error_or("unknown");
        if (resp.has_value()) {
            auto& data = resp.value();
            if (data.empty()) {
                co_return R"({"error": "Empty search result."})";
            }
            co_return data;
        }
    } else {
        // 返回原始响应体
        auto resp    = co_await agentxx::util::HttpClient::getAsync(search_url, headers, config);
        out_resp_err = resp.error_or("unknown");
        if (resp.has_value()) {
            auto& respVal = resp.value();
            if (agentxx::util::HttpClient::respIsSucc(respVal)) {
                auto& data = respVal.body;
                if (data.empty()) {
                    co_return R"({"error": "Empty search result."})";
                }
                co_return data;
            }
            // 请求成功但返回非 2xx (如 429/403/500): 拼接状态码与错误响应体
            // (resp.error_or 在有值时返回默认值 "unknown", 因此需手动构造错误信息)
            auto errBody = respVal.body;
            if (errBody.size() > 512) {
                errBody.resize(512);
                errBody += "...";
            }
            out_resp_err = fmt::format("HTTP status {}: {}", respVal.status, errBody);
        }
    }
    throw std::runtime_error(out_resp_err.value_or("[unknown]"));
}

WebFetchUrlTool::WebFetchUrlTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext) :
    XXToolBase("agentxx_web_fetch", in_agentContext, true, true) {}

neograph::ChatTool WebFetchUrlTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {{
                     "url",
                     {
                         {"type", "string"},
                         {"description", prompt.getArg("url")},
                     },
                 },
                 {
                     "timeout",
                     {
                         {"type", "number"},
                         {"description", prompt.getArg("timeout")},
                     },
                 },
                 {
                     "header",
                     {
                         {"type", "object"},
                         {"description", prompt.getArg("header")},
                     },
                 }},
            }, {"required", neograph::json::array({"url"})},
                       },
    };
}

asio::awaitable<std::string> WebFetchUrlTool::execute_async(const neograph::json& arguments) {
    auto url = arguments.value("url", std::string{});
    if (url.empty()) {
        co_return R"({"error":"Arg `url` is empty"})";
    }

    // 统一的 timeout / header 参数: 支持自定义请求头与请求超时
    const auto headers = parseHeaderArg(arguments);
    const int  timeout = parseTimeoutArg(arguments, 30);
    auto       config  = agentxx::util::HttpClient::RequestConfig{};
    if (timeout > 0) {
        config.readChunkTimeout = std::chrono::seconds{timeout};
    }

    auto resp = co_await agentxx::util::HttpClient::getAsync(url, headers, config);
    if (resp.has_value()) {
        if (false == agentxx::util::HttpClient::respIsSucc(resp.value())) {
            co_return fmt::format(
                R"({{"error":"web_fetch_url failed, status {}, error: {}"}})",
                resp.value().status,
                resp.error_or("[unknown]")
            );
        }

        auto& data = resp.value().body;
        if (data.empty()) {
            co_return R"({"error": "Http GET request Success, but got empty body."})";
        }
        if (agentxx::util::autoConvertToUtf8(data)) {
            co_return data;
        }
        co_return data;
    }
    throw std::runtime_error(resp.error_or("[unknown]"));
}

WebFetchUrlMarkdownTool::WebFetchUrlMarkdownTool(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("agentxx_web_fetch_markdown", in_agentContext, true, true) {}

neograph::ChatTool WebFetchUrlMarkdownTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {{
                     "url",
                     {
                         {"type", "string"},
                         {"description", prompt.getArg("url")},
                     },
                 },
                 {
                     "timeout",
                     {
                         {"type", "number"},
                         {"description", prompt.getArg("timeout")},
                     },
                 },
                 {
                     "header",
                     {
                         {"type", "object"},
                         {"description", prompt.getArg("header")},
                     },
                 }},
            }, {"required", neograph::json::array({"url"})},
                       },
    };
}

asio::awaitable<std::string> WebFetchUrlMarkdownTool::execute_async(const neograph::json& arguments
) {
    std::string url = arguments.value("url", std::string{});
    if (url.empty()) {
        co_return R"({"error":"Arg `url` is empty"})";
    }

    // 统一的 timeout / header 参数: 支持自定义请求头与请求超时
    const auto headers = parseHeaderArg(arguments);
    const int  timeout = parseTimeoutArg(arguments, 15);
    auto       config  = agentxx::util::HttpClient::RequestConfig{};
    if (timeout > 0) {
        config.readChunkTimeout = std::chrono::seconds{timeout};
    }

    auto resp = co_await agentxx::util::HttpClient::fetchMarkdown(url, headers, config);
    if (resp.has_value()) {
        auto& data = resp.value();
        if (data.empty()) {
            co_return R"({"error": "Request Success, but got empty result."})";
        }
        co_return data;
    }
    throw std::runtime_error(resp.error_or("[unknown]"));
}

ModelWebSearchTool::ModelWebSearchTool(
    const agentxx::agent::ModelConfig&          in_modelCfg,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    XXToolBase("agentxx_web_search", in_agentContext, true, true),
    modelCfg(in_modelCfg) {}

neograph::ChatTool ModelWebSearchTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    return {
        get_name(),
        prompt.depict,
        neograph::json{
                       {"type", "object"},
                       {
                "properties",
                {{
                     "query",
                     {
                         {"type", "string"},
                         {"description", prompt.getArg("query")},
                     },
                 },
                 {
                     "timeout",
                     {
                         {"type", "number"},
                         {"description", prompt.getArg("timeout")},
                     },
                 },
                 {
                     "header",
                     {
                         {"type", "object"},
                         {"description", prompt.getArg("header")},
                     },
                 }},
            }, {"required", neograph::json::array({"query"})},
                       },
    };
}

asio::awaitable<std::string> ModelWebSearchTool::execute_async(const neograph::json& arguments) {
    std::string query = arguments.value("query", std::string{});
    if (query.empty()) {
        co_return R"({"error":"Arg `query` is empty"})";
    }

    // 统一的 timeout / header 参数: 复制一份模型配置并应用覆盖 (超时 + 自定义请求头)
    auto       cfg     = modelCfg;
    const auto headers = parseHeaderArg(arguments);
    const int  timeout = parseTimeoutArg(arguments, 60);
    if (!headers.empty()) {
        for (const auto& [k, v] : headers.data) {
            if (!v.empty()) {
                cfg.extraHeaders[k] = v[0];
            }
        }
    }
    if (timeout > 0) {
        cfg.readChunkTimeoutSeconds = timeout;
    }
    // 每次执行创建独立的 provider, 避免覆盖配置在多次调用间相互影响
    auto provider = agentxx::server::OpenAIProvider::create(cfg);

    // 构造消息，请求模型进行网络搜索
    neograph::CompletionParams params;
    params.model       = ""; // 使用 provider 默认模型
    params.temperature = 0.0f;
    params.messages    = {
        neograph::ChatMessage{
                              .role    = "system",
                              .content = "You are a web search assistant. Search the internet "
                          "for the user's query and provide comprehensive, "
                          "accurate results with sources. Respond in the same "
                          "language as the query.", },
        neograph::ChatMessage{
                              .role    = "user",
                              .content = query,
                              },
    };

    auto  completion = co_await provider->invoke(params, nullptr);
    auto& content    = completion.message.content;
    if (content.empty()) {
        co_return R"({"error": "Model web search returned empty result."})";
    }
    co_return content;
}

} // namespace tools
} // namespace agentxx
