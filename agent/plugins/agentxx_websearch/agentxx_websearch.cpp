/// agentxx_websearch —— 网络访问工具插件
#include "agentxx_websearch_plugin.h"
#include "utilxx_base/json.h"
#include "websearch_impl.h"
#include <string>

using namespace agentxx_websearch_plugin;
using namespace agentxx::plugin;

namespace {

constexpr std::string_view kNameSearch = "agentxx_web_search";
constexpr std::string_view kNameFetch  = "agentxx_web_fetch";

constexpr std::string_view kDepictSearch =
    R"_(Perform a web search. Returns a markdown-formatted list of results.
Use `agentxx_web_fetch` afterwards to retrieve full page content from a result.)_";
constexpr std::string_view kDepictFetch = R"_(Perform an HTTP GET request and return the response.
With `format` = `markdown` (default) the page is converted to Markdown; with `format` = `raw` the raw response body is returned (for JSON/text APIs, or when the conversion is not wanted).
Commonly used after `agentxx_web_search` to read a specific page.)_";

constexpr std::string_view kHeaderArgDesc =
    R"_(Custom HTTP request headers to send, as a JSON object of header name to value.
Example: {"X-Api-Key": "xxx", "User-Agent": "agentxx"})_";

constexpr std::string_view kFetchUrlDesc = R"_(Absolute HTTP/HTTPS URL to fetch.

When resolving relative links found in the returned Markdown, combine them with this `url`:
- Page `http://example.com/help/`:
  - `model/delete/` (no leading /) → `http://example.com/help/model/delete/`
  - `./model/create/` (leading .) → `http://example.com/help/model/create/`
  - `../model/create/` (leading ..) → `http://example.com/model/create/`
  - `/model/view/` (leading /) → `http://example.com/model/view/`
- Page `http://example.com/help/what.html`:
  - `model/delete/` (no leading /) → strip filename, append → `http://example.com/help/model/delete/`
)_";

constexpr std::string_view kFetchFormatDesc = R"_(Default `markdown`. What to return:
`markdown`: the page content converted to Markdown (best for HTML pages).
`raw`: the response body exactly as received (best for JSON/text APIs, or when the conversion is not wanted).)_";

constexpr std::string_view kFetchTimeoutDesc = "Default `30` seconds. Request timeout in seconds.";

constexpr std::string_view kSearchQueryDesc = "The search query string.";

constexpr std::string_view kSearchTimeoutDesc = "Default `20` seconds. Request timeout in seconds.";

constexpr int32_t kAutoSummary = AGENTXX_PLUGIN_TOOL_FLAG_AUTO_SUMMARY;

} // namespace

struct WebsearchPluginCtx : public PluginBase {
    bool              convert_html2markdown = true;
    std::string       search_api_url;
    ModelSearchConfig model_cfg;
    bool              use_model_search = false;
};

/// 把一次请求交给主程序网络服务 (`<应用名>.net` 的 `http.fetch`)
///
/// 工具实现体跑在主程序工作线程上, 所以用阻塞等结果的形态 (`netFetchBlocking`);
/// 结果里的 `ok` 是"请求跑完了", 状态码与正文由各自执行体判断。
static HttpResult netHttpExecute(WebsearchPluginCtx& ctx, const HttpRequestSpec& spec) {
    HttpResult out;
    if (!ctx.iface.net) {
        out.error = "Host network capability (`agentxx.net`) is unavailable";
        return out;
    }
    agentxx::plugin::NetHttpOptions options;
    options.method    = spec.method;
    options.json      = spec.jsonBody;
    options.headers   = spec.headersJson.empty() ? std::string{"{}"} : spec.headersJson;
    options.format    = spec.format;
    options.timeoutMs = spec.timeoutSeconds > 0 ? spec.timeoutSeconds * 1000 : 0;

    const auto result
        = netFetchBlocking(ctx.host, ctx.iface.net, ctx.iface.scheduler, spec.url, options);
    if (!result.accepted()) {
        out.error = fmt::format("net call failed: {}", result.error());
        return out;
    }
    out.status = result.status();
    if (!result.ok()) {
        out.error = result.error();
        return out;
    }
    out.ok   = true;
    out.body = result.str("body");
    return out;
}

/// 注册事务 (start 的实际内容): 读取主程序配置并注册三个工具。
///
/// 两个工具的实现体都是**同步**函数: 网络经主程序 `<应用名>.net` 表的
/// `http.fetch` (异步受理 + 工作线程上等结果), 因此注册为 `blocking_tool` ——
/// 等待期间只占一个工作线程, 不在主程序 io 线程上阻塞。
static int32_t websearchSetup(WebsearchPluginCtx& ctx, const agentxx::plugin::ScopeRef& ref) {
    if (ctx.iface.model && ctx.iface.model->get_config) {
        auto           modelRef = ref.raw();
        PluginxxString json{nullptr, 0};
        ctx.iface.model->get_config(ctx.host, &modelRef, &json);
        if (json.data) {
            std::string cfgJson(json.data, static_cast<size_t>(json.size));
            PluginString::free(ctx.host, &json);
            try {
                auto cfg                  = utilxx_base::Json::parse(cfgJson);
                ctx.convert_html2markdown = cfg.value("websearchConvertHtml2markdown", true);
                ctx.search_api_url        = cfg.value("websearchApiUrl", std::string{});
                if (cfg.contains("websearchModel") && cfg["websearchModel"].is_object()) {
                    const auto& m           = cfg["websearchModel"];
                    ctx.model_cfg.baseUrl   = m.value("baseUrl", std::string{});
                    ctx.model_cfg.apiKey    = m.value("apiKey", std::string{"EMPTY"});
                    ctx.model_cfg.modelName = m.value("modelName", std::string{"Agentxx"});
                    ctx.model_cfg.readChunkTimeoutSeconds = m.value("readChunkTimeoutSeconds", 100);
                    if (!ctx.model_cfg.baseUrl.empty()) {
                        ctx.use_model_search = true;
                    }
                }
            } catch (...) {
            }
        }
    }

    // 1. fetch (fetch 与 fetch_markdown: 由 `format` 选择返回形态)
    auto fetchSchema
        = ctx.schema(kNameFetch)
              .string("url", kFetchUrlDesc, /*required=*/true)
              .enumString("format", kFetchFormatDesc, {"markdown", "raw"}, false, "markdown")
              .number("timeout", kFetchTimeoutDesc, false, 30.0)
              .object("header", kHeaderArgDesc)
              .build();

    blocking_tool(
        ctx,
        ref,
        kNameFetch,
        kDepictFetch,
        fetchSchema,
        [](WebsearchPluginCtx&      c,
           std::string_view         args_json,
           const PluginxxCancelToken* cancel_token) -> ToolResult {
            if (pluginxx_cancel_is_requested(cancel_token)) {
                throw CanceledException("web_fetch Canceled");
            }
            ArgReader args(args_json);
            auto      http = [&c](const HttpRequestSpec& spec) {
                return netHttpExecute(c, spec);
            };
            auto result = webFetchExecute(http, args.raw());
            if (pluginxx_cancel_is_requested(cancel_token)) {
                throw CanceledException("web_fetch Canceled");
            }
            return ToolResult::ok(std::move(result));
        },
        0,
        kAutoSummary
    );

    // 2. search
    if (ctx.use_model_search || !ctx.search_api_url.empty()) {
        auto searchSchema = ctx.schema(kNameSearch)
                                .string("query", kSearchQueryDesc, /*required=*/true)
                                .number("timeout", kSearchTimeoutDesc, false, 20.0)
                                .object("header", kHeaderArgDesc)
                                .build();

        blocking_tool(
            ctx,
            ref,
            kNameSearch,
            kDepictSearch,
            searchSchema,
            [](WebsearchPluginCtx&      c,
               std::string_view         args_json,
               const PluginxxCancelToken* cancel_token) -> ToolResult {
                if (pluginxx_cancel_is_requested(cancel_token)) {
                    throw CanceledException("web_search Canceled");
                }
                ArgReader   args(args_json);
                auto        http = [&c](const HttpRequestSpec& spec) {
                    return netHttpExecute(c, spec);
                };
                std::string result;
                if (c.use_model_search) {
                    ModelSearchConfig mcfg = c.model_cfg;
                    result                 = modelWebSearchExecute(http, args.raw(), mcfg);
                } else {
                    result = webSearchExecute(
                        http,
                        args.raw(),
                        c.search_api_url,
                        c.convert_html2markdown
                    );
                }
                if (pluginxx_cancel_is_requested(cancel_token)) {
                    throw CanceledException("web_search Canceled");
                }
                return ToolResult::ok(std::move(result));
            },
            0,
            kAutoSummary
        );
    }

    return 0;
}

static void* websearchStart(
    WebsearchPluginCtx&           ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*
) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    (void)ctx;
    return nullptr;
}

static void* websearchMount(
    WebsearchPluginCtx&                ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error
) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "agentxx_websearch mount: notify required"
            );
        }
        return nullptr;
    }
    const agentxx::plugin::AgentRef ref(*spec);
    if (websearchSetup(ctx, ref) != 0) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "agentxx_websearch mount: registration failed"
            );
        }
        return nullptr;
    }
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void*
    websearchUnmount(WebsearchPluginCtx&, void*, const PluginxxOperatorNotify* notify, PluginxxString*) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void*
    websearchStop(WebsearchPluginCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_EXPORT(
    WebsearchPluginCtx,
    "agentxx_websearch",
    "1.0.0",
    "Web access tools: HTTP fetch (raw or Markdown) and web search",
    websearchStart,
    websearchStop,
    websearchMount,
    websearchUnmount
);

struct WebsearchClientCtx : public ClientPluginBase {};

/// client 侧 start: 注册工具的折叠头模版 (stop 无需撤销, 主程序负责摘除)
static void*
    websearchClientStart(WebsearchClientCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    ctx.registerTemplate(kNameSearch, "Search", "query");
    ctx.registerTemplate(kNameFetch, "Fetch", "url");
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void*
    websearchClientStop(WebsearchClientCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_CLIENT_EXPORT(
    WebsearchClientCtx,
    "agentxx_websearch",
    "1.0.0",
    "Websearch tools specialized UI renderer",
    websearchClientStart,
    websearchClientStop
);
