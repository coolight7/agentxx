/// agentxx_websearch 插件 —— 工具实现 (纯函数, 不含 C ABI 胶水)
/// - 从 libagentxx src/tools/web_search 拆分: 同名工具同行为
///     agentxx_web_search / agentxx_web_fetch / agentxx_web_fetch_markdown
/// - 头文件-only: 插件入口与测试共同包含, 保证插件行为与测试覆盖一致
/// - 依赖: agentxx_util (HttpClient / 字符串编码转换)
/// - 统一异步操作模型 (poll 寄生驱动): 执行体为协程 (*ExecuteAsync), 在插件
///   实例的 PollLoop (无线程寄生事件循环) 上 spawn, 由宿主 io 线程经 pollOnce
///   非阻塞步进 —— 与内置工具同线程交错执行; HttpClient 为协程接口直接
///   co_await, 不再经局部 io_context 同步驱动 (原 runSync 模式已移除)
/// - 取消语义: 协程内阶段边界轮询 cancel_flag (多请求路径的请求间生效);
///   单请求中断依赖 chunk 超时 (HttpClient 暂未暴露外部 cancellation slot)
/// - html→markdown 转换 (cmark-gfm) 为同步 CPU 段, 典型页面 <10ms; 超大页面
///   可能触发宿主看门狗告警 (>100ms WARN), 属可接受范围 (与原内置工具一致)
/// - 模型搜索经 OpenAI 兼容 chat/completions 非流式请求实现 (原 lib 版本走
///   OpenAIProvider, 行为一致: system+user 两条消息, temperature=0,
///   取回 choices[0].message.content 文本)
#pragma once

#include "agentxx/util/http_client.h"
#include "agentxx/util/string_util.h"
#include "asio/awaitable.hpp"
#include <charconv>
#include <chrono>
#include <expected>
#include <map>
#include <optional>
#include <string>

namespace agentxx_websearch_plugin {

/// 模型搜索配置 (原 ModelWebSearchTool 所需的最小字段集;
/// 由插件入口从宿主 agentxx.agent.model 接口表 JSON 填充)
struct ModelSearchConfig {
    std::string                        baseUrl;
    std::string                        apiKey                  = "EMPTY";
    std::string                        modelName               = "Agentxx";
    int                                readChunkTimeoutSeconds = 100;
    std::map<std::string, std::string> extraHeaders{};
};

namespace detail {

/// 解析 tool 参数中的 `timeout` (秒)
/// - 支持数字或数字字符串 (部分模型会传字符串)
/// - 非法/缺失时返回默认值; 返回值 <= 0 表示未指定, 使用默认配置
inline int parseTimeoutArg(const neograph::json& args, int defaultSeconds) {
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
inline agentxx::util::HeaderMap parseHeaderArg(const neograph::json& args) {
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

inline agentxx::util::HttpClient::RequestConfig
    makeConfig(int timeout, std::chrono::seconds defaultTimeout) {
    auto config             = agentxx::util::HttpClient::RequestConfig{};
    config.connectTimeout   = defaultTimeout;
    config.readChunkTimeout = (timeout > 0) ? std::chrono::seconds{timeout} : defaultTimeout;
    return config;
}

} // namespace detail

// =====================================================================
// 各工具执行体
// - 返回结果文本; 失败抛出异常 (由 C ABI 边界捕获转 error_out),
//   与原 lib 工具"异常即工具错误"语义一致
// =====================================================================

/// agentxx_web_fetch 执行体 (原 WebFetchUrlTool::execute_async)
inline asio::awaitable<std::string> webFetchExecuteAsync(const neograph::json& arguments) {
    auto url = arguments.value("url", std::string{});
    if (url.empty()) {
        co_return R"({"error":"Arg `url` is empty"})";
    }

    // 统一的 timeout / header 参数: 支持自定义请求头与请求超时
    const auto headers = detail::parseHeaderArg(arguments);
    const int  timeout = detail::parseTimeoutArg(arguments, 30);

    auto resp = co_await agentxx::util::HttpClient::getAsync(
        url,
        headers,
        detail::makeConfig(timeout, std::chrono::seconds{30})
    );
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

/// agentxx_web_fetch_markdown 执行体 (原 WebFetchUrlMarkdownTool::execute_async)
inline asio::awaitable<std::string> webFetchMarkdownExecuteAsync(const neograph::json& arguments) {
    std::string url = arguments.value("url", std::string{});
    if (url.empty()) {
        co_return R"({"error":"Arg `url` is empty"})";
    }

    // 统一的 timeout / header 参数: 支持自定义请求头与请求超时
    const auto headers = detail::parseHeaderArg(arguments);
    const int  timeout = detail::parseTimeoutArg(arguments, 15);

    auto resp = co_await agentxx::util::HttpClient::fetchMarkdown(
        url,
        headers,
        detail::makeConfig(timeout, std::chrono::seconds{15})
    );
    if (resp.has_value()) {
        auto& data = resp.value();
        if (data.empty()) {
            co_return R"({"error": "Request Success, but got empty result."})";
        }
        co_return data;
    }
    throw std::runtime_error(resp.error_or("[unknown]"));
}

/// agentxx_web_search 执行体 —— API URL 路径 (原 WebSearchTool::execute_async)
/// - searchApiUrl 含 `{}` 占位符 (fmt::runtime), URL 编码后的 query 填入
inline asio::awaitable<std::string> webSearchExecuteAsync(
    const neograph::json& arguments,
    std::string_view      searchApiUrl,
    bool                  convertHtml2markdown
) {
    std::string query = arguments.value("query", std::string{});
    if (query.empty()) {
        co_return R"({"error":"Arg `query` is empty"})";
    }
    auto search_url
        = fmt::format(fmt::runtime(searchApiUrl), agentxx::util::HttpClient::urlEncode(query));

    // 统一的 timeout / header 参数: 支持自定义请求头与请求超时
    const auto headers = detail::parseHeaderArg(arguments);
    const int  timeout = detail::parseTimeoutArg(arguments, 15);
    auto       config  = detail::makeConfig(timeout, std::chrono::seconds{15});

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

/// agentxx_web_search 执行体 —— 模型搜索路径 (原 ModelWebSearchTool::execute_async)
/// - 经 OpenAI 兼容 chat/completions 非流式请求实现 (见文件头注释)
inline asio::awaitable<std::string>
    modelWebSearchExecuteAsync(const neograph::json& arguments, const ModelSearchConfig& modelCfg) {
    std::string query = arguments.value("query", std::string{});
    if (query.empty()) {
        co_return R"({"error":"Arg `query` is empty"})";
    }

    // 统一的 timeout / header 参数: 复制一份模型配置并应用覆盖 (超时 + 自定义请求头)
    auto       cfg     = modelCfg;
    const auto headers = detail::parseHeaderArg(arguments);
    const int  timeout = detail::parseTimeoutArg(arguments, 60);
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

    // 构造 chat/completions 请求体: system+user 两条消息, temperature=0
    // (与原 OpenAIProvider 调用参数一致)
    neograph::json body = neograph::json::object();
    body["model"]       = cfg.modelName;
    body["temperature"] = 0.0f;
    body["messages"]    = neograph::json::array({
        neograph::json{
                       {"role", "system"},
                       {"content",
                "You are a web search assistant. Search the internet "
                   "for the user's query and provide comprehensive, "
                   "accurate results with sources. Respond in the same "
                   "language as the query."},
                       },
        neograph::json{
                       {"role", "user"},
                       {"content", query},
                       },
    });

    auto extraHeaders = agentxx::util::HeaderMap{};
    extraHeaders.set("Authorization", fmt::format("Bearer {}", cfg.apiKey));
    for (const auto& [k, v] : cfg.extraHeaders) {
        extraHeaders.set(k, v);
    }

    auto defaultSec = cfg.readChunkTimeoutSeconds > 0 ? cfg.readChunkTimeoutSeconds : 100;
    auto config = detail::makeConfig(cfg.readChunkTimeoutSeconds, std::chrono::seconds{defaultSec});

    auto resp = co_await agentxx::util::HttpClient::postAsync(
        fmt::format("{}/chat/completions", cfg.baseUrl),
        body,
        extraHeaders,
        config
    );
    if (!resp.has_value()) {
        throw std::runtime_error(resp.error_or("[unknown]"));
    }
    auto& respVal = resp.value();
    if (false == agentxx::util::HttpClient::respIsSucc(respVal)) {
        auto bodySnippet = respVal.body.substr(0, 512);
        throw std::runtime_error(fmt::format("HTTP status {}: {}", respVal.status, bodySnippet));
    }
    try {
        auto parsed  = neograph::json::parse(respVal.body);
        auto content = parsed["choices"][0]["message"]["content"].get<std::string>();
        if (content.empty()) {
            co_return R"({"error": "Model web search returned empty result."})";
        }
        co_return content;
    } catch (...) {
        throw std::runtime_error("Model web search: unexpected response format");
    }
}

} // namespace agentxx_websearch_plugin
