/// - 从 libagentxx src/tools/web_search 拆分: 同名工具同行为
///     agentxx_web_search / agentxx_web_fetch (原 web_fetch 与 web_fetch_markdown
///     已合并, 由 `format` 参数选择 raw 或 markdown)
/// - 头文件-only: 插件入口与测试共同包含, 保证插件行为与测试覆盖一致
/// - 依赖: 只依赖 cxx_utilxx_base (Json / 字符串编码转换); 网络经 [HttpFn] 注入 ——
///   插件入口用主程序 `<应用名>.net` 表 (`netFetchBlocking`), 测试注入本地实现,
///   因此本文件不建连接、不依赖 asio
/// - 执行模型: 三个执行体都是**同步函数**, 由插件入口注册为 `blocking_tool`
///   (跑在主程序工作线程上, 阻塞安全)。这取代了原来的 `polled_tool` + HttpClient
///   协程: 等待不再是插件自己的 reactor 就绪事件, 而是主程序网络服务的回调
/// - 取消语义: 阶段边界轮询 cancel_flag (多请求路径的请求间生效);
///   单条请求的中断依赖网络服务的超时
/// - html→markdown 转换由主程序网络服务完成 (`format: markdown`), 本文件不参与
/// - 模型搜索经 OpenAI 兼容 chat/completions 非流式请求实现 (与原 lib 版本一致:
///   system+user 两条消息, temperature=0, 取回 choices[0].message.content 文本)
#pragma once

#include "utilxx_base/json.h"
#include "utilxx_base/string_util.h"
#include <charconv>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace agentxx_websearch_plugin {

/// 模型搜索配置 (原 ModelWebSearchTool 所需的最小字段集;
/// 由插件入口从主程序 agentxx.agent.model 接口表 JSON 填充)
struct ModelSearchConfig {
    std::string                        baseUrl;
    std::string                        apiKey                  = "EMPTY";
    std::string                        modelName               = "Agentxx";
    int                                readChunkTimeoutSeconds = 100;
    std::map<std::string, std::string> extraHeaders{};
};

/// 一次 HTTP 请求的结果
struct HttpResult {
    bool        ok     = false; ///< 请求是否跑完 (连不上 / 超时 / 取消时为 false)
    int64_t     status = 0;     ///< HTTP 状态码 (请求没跑完时为 0)
    std::string body;           ///< 响应体 (`format` = markdown 时已是 Markdown)
    std::string error;          ///< `ok == false` 时的原因
};

/// 状态码是否是 2xx
inline bool isSuccessStatus(int64_t status) {
    return status >= 200 && status < 300;
}

/// 一次 HTTP 请求的参数 (与具体网络实现无关)
struct HttpRequestSpec {
    std::string url;
    std::string method      = "GET";
    std::string body;                  ///< 请求体原文
    std::string jsonBody;              ///< 请求体 JSON 文本 (与 [body] 二选一)
    std::string headersJson = "{}";    ///< 请求头 JSON 对象文本
    std::string format;                ///< "" (原样) 或 "markdown" (HTML 转 Markdown)
    int         timeoutSeconds = 0;    ///< <= 0 = 用实现自己的默认值
};

/// HTTP 执行函数
///
/// - 插件入口: 经主程序 `<应用名>.net` 表的 `http.fetch` 实现 (见
///   `agentxx::plugin::netFetchBlocking`);
/// - 测试: 注入本地实现, 不依赖网络。
using HttpFn = std::function<HttpResult(const HttpRequestSpec&)>;

namespace detail {

/// 解析 tool 参数中的 `timeout` (秒)
/// - 支持数字或数字字符串 (部分模型会传字符串)
/// - 非法/缺失时返回默认值; 返回值 <= 0 表示未指定, 使用默认配置
inline int parseTimeoutArg(const utilxx_base::Json& args, int defaultSeconds) {
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

/// 解析 tool 参数中的 `header` 参数为请求头 JSON 对象文本, 支持三种格式
/// (声明为对象; 另外两种保留以容忍模型偶发的写法):
/// - JSON 对象: {"User-Agent": "xx", "X-Api-Key": "v"} (非字符串值转为文本)
/// - JSON 字符串数组: ["User-Agent: xx", "X-Api-Key: v"]
/// - JSON 字符串: "User-Agent: xx" (多行时按行解析 "Name: value")
inline utilxx_base::Json parseHeaderArg(const utilxx_base::Json& args) {
    auto headers = utilxx_base::Json::object();
    if (!args.is_object()) {
        return headers;
    }
    const auto headerVal = args["header"];

    // 解析单行 "Name: value"
    auto addHeaderLine = [&headers](std::string line) {
        line = utilxx_base::removeBetweenSpace(line);
        if (line.empty()) {
            return;
        }
        const auto pos = line.find(':');
        if (pos == std::string::npos) {
            return;
        }
        auto name = utilxx_base::removeBetweenSpace(line.substr(0, pos));
        if (name.empty()) {
            return;
        }
        auto value      = utilxx_base::removeBetweenSpace(line.substr(pos + 1));
        headers[name] = value;
    };

    if (headerVal.is_object()) {
        for (const auto& [k, v] : headerVal.items()) {
            if (v.is_string()) {
                headers[k] = v.get<std::string>();
            } else if (!v.is_null()) {
                headers[k] = v.dump();
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

/// 结果体按需截断 (错误信息里带响应片段)
inline std::string snippet(const std::string& text, size_t limit = 512) {
    if (text.size() <= limit) {
        return text;
    }
    std::string out = text.substr(0, limit);
    out += "...";
    return out;
}

} // namespace detail

// =====================================================================
// 各工具执行体 (同步; 失败抛出异常, 由插件的工具边界转成失败状态)
// =====================================================================

/// agentxx_web_fetch 执行体 (原 web_fetch 与 web_fetch_markdown 合并):
/// - `format` = `markdown` (默认): 页面转 Markdown
/// - `format` = `raw`: 原始响应体 (状态非 2xx / 响应体为空按错误处理)
inline std::string webFetchExecute(const HttpFn& http, const utilxx_base::Json& arguments) {
    std::string url = arguments.value("url", std::string{});
    if (url.empty()) {
        throw std::invalid_argument{"Arg `url` is empty"};
    }
    // 返回形态: 缺省 markdown (与合并前的 fetch_markdown 行为一致)
    auto format = arguments.value("format", std::string{});
    if (format.empty()) {
        format = "markdown";
    }
    if (format != "markdown" && format != "raw") {
        throw std::invalid_argument{
            fmt::format(R"_(Arg `format`({}) is invalid: use `markdown` or `raw`.)_", format)
        };
    }

    HttpRequestSpec spec;
    spec.url            = std::move(url);
    spec.method         = "GET";
    spec.headersJson    = detail::parseHeaderArg(arguments).dump();
    spec.format         = (format == "markdown") ? "markdown" : std::string{};
    spec.timeoutSeconds = detail::parseTimeoutArg(arguments, 30);

    if (!http) {
        throw std::runtime_error{"Host network capability is unavailable"};
    }
    const auto result = http(spec);
    if (!result.ok) {
        throw std::runtime_error(result.error.empty() ? "[unknown]" : result.error);
    }
    if (format == "markdown") {
        if (!isSuccessStatus(result.status)) {
            throw std::runtime_error{
                fmt::format("Http GET failed, status {}: {}", result.status, detail::snippet(result.body))
            };
        }
        if (result.body.empty()) {
            throw std::runtime_error{"Request succeeded, but the converted result is empty"};
        }
        return result.body;
    }

    if (!isSuccessStatus(result.status)) {
        // 请求成功但状态非 2xx (如 429/403/500): 拼接状态码与错误响应体
        throw std::runtime_error{
            fmt::format("Http GET failed, status {}: {}", result.status, detail::snippet(result.body))
        };
    }
    auto data = result.body;
    if (data.empty()) {
        throw std::runtime_error{"Http GET succeeded, but the response body is empty"};
    }
    utilxx_base::autoConvertToUtf8(data);
    return data;
}

/// agentxx_web_search 执行体 —— API URL 路径 (原 WebSearchTool::execute_async)
/// - searchApiUrl 含 `{}` 占位符 (fmt::runtime), URL 编码后填入
/// - convertHtml2markdown: true 时要求 Markdown 形态结果, false 时返回原始响应体
inline std::string webSearchExecute(
    const HttpFn&            http,
    const utilxx_base::Json& arguments,
    std::string_view         searchApiUrl,
    bool                     convertHtml2markdown
) {
    std::string query = arguments.value("query", std::string{});
    if (query.empty()) {
        throw std::invalid_argument{"Arg `query` is empty"};
    }
    // URL 编码 (原 HttpClient::urlEncode 的同一套规则: 只保留 unreserved 字符)
    std::string encodedQuery;
    encodedQuery.reserve(query.size() * 3);
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (const unsigned char c : query) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                                || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'
                                || c == '~';
        if (unreserved) {
            encodedQuery.push_back(static_cast<char>(c));
            continue;
        }
        encodedQuery.push_back('%');
        encodedQuery.push_back(kHex[(c >> 4) & 0x0F]);
        encodedQuery.push_back(kHex[c & 0x0F]);
    }
    std::string searchUrl = fmt::format(fmt::runtime(searchApiUrl), encodedQuery);

    HttpRequestSpec spec;
    spec.url            = std::move(searchUrl);
    spec.method         = "GET";
    spec.headersJson    = detail::parseHeaderArg(arguments).dump();
    spec.format         = convertHtml2markdown ? "markdown" : std::string{};
    spec.timeoutSeconds = detail::parseTimeoutArg(arguments, 20);

    if (!http) {
        throw std::runtime_error{"Host network capability is unavailable"};
    }
    const auto result = http(spec);
    if (!result.ok) {
        throw std::runtime_error(result.error.empty() ? "[unknown]" : result.error);
    }
    if (!isSuccessStatus(result.status)) {
        // 请求成功但返回非 2xx (如 429/403/500): 拼接状态码与错误响应体
        throw std::runtime_error{
            fmt::format("HTTP status {}: {}", result.status, detail::snippet(result.body))
        };
    }
    if (result.body.empty()) {
        throw std::runtime_error{"The search request succeeded, but the result is empty"};
    }
    return result.body;
}

/// agentxx_web_search 执行体 —— 模型搜索路径 (原 ModelWebSearchTool::execute_async)
/// - 经 OpenAI 兼容 chat/completions 非流式请求实现
inline std::string modelWebSearchExecute(
    const HttpFn&            http,
    const utilxx_base::Json& arguments,
    const ModelSearchConfig& modelCfg
) {
    std::string query = arguments.value("query", std::string{});
    if (query.empty()) {
        throw std::invalid_argument{"Arg `query` is empty"};
    }

    // 统一的 timeout / header 参数: 复制一份模型配置并应用覆盖 (超时 + 自定义请求头)
    auto       cfg     = modelCfg;
    auto       headers = detail::parseHeaderArg(arguments);
    const int  timeout = detail::parseTimeoutArg(arguments, 60);
    if (headers.is_object()) {
        for (const auto& [k, v] : headers.items()) {
            if (v.is_string() && !v.get<std::string>().empty()) {
                cfg.extraHeaders[std::string{k}] = v.get<std::string>();
            }
        }
    }
    if (timeout > 0) {
        cfg.readChunkTimeoutSeconds = timeout;
    }

    // 构造 chat/completions 请求体: system+user 两条消息, temperature=0
    // (与原 OpenAIProvider 调用参数一致)
    utilxx_base::Json body = utilxx_base::Json::object();
    body["model"]          = cfg.modelName;
    body["temperature"]    = 0.0f;
    body["messages"]       = utilxx_base::Json::array({
        utilxx_base::Json{
                          {"role", "system"},
                          {"content",
                   "You are a web search assistant. Search the internet "
                         "for the user's query and provide comprehensive, "
                         "accurate results with sources. Respond in the same "
                         "language as the query."},
                          },
        utilxx_base::Json{
                          {"role", "user"},
                          {"content", query},
                          },
    });

    utilxx_base::Json requestHeaders = utilxx_base::Json::object();
    requestHeaders["Authorization"]  = fmt::format("Bearer {}", cfg.apiKey);
    for (const auto& [k, v] : cfg.extraHeaders) {
        requestHeaders[k] = v;
    }

    HttpRequestSpec spec;
    spec.url            = fmt::format("{}/chat/completions", cfg.baseUrl);
    spec.method         = "POST";
    spec.jsonBody       = body.dump();
    spec.headersJson    = requestHeaders.dump();
    spec.timeoutSeconds = cfg.readChunkTimeoutSeconds > 0 ? cfg.readChunkTimeoutSeconds : 100;

    if (!http) {
        throw std::runtime_error{"Host network capability is unavailable"};
    }
    const auto result = http(spec);
    if (!result.ok) {
        throw std::runtime_error(result.error.empty() ? "[unknown]" : result.error);
    }
    if (!isSuccessStatus(result.status)) {
        throw std::runtime_error{
            fmt::format("HTTP status {}: {}", result.status, detail::snippet(result.body))
        };
    }
    try {
        auto parsed  = utilxx_base::Json::parse(result.body);
        auto content = parsed["choices"][0]["message"]["content"].get<std::string>();
        if (content.empty()) {
            throw std::runtime_error{"The model search succeeded, but the result is empty"};
        }
        return content;
    } catch (const std::runtime_error&) {
        throw;
    } catch (...) {
        throw std::runtime_error("Model web search: unexpected response format");
    }
}

} // namespace agentxx_websearch_plugin
