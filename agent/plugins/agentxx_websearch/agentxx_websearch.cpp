/// agentxx_websearch —— 网络访问工具插件
#include "agentxx_websearch_plugin.h"
#include "asio/awaitable.hpp"
#include "websearch_impl.h"
#include <string>

using namespace agentxx_websearch_plugin;
using namespace agentxx::plugin;

namespace {

constexpr std::string_view kNameSearch  = "agentxx_web_search";
constexpr std::string_view kNameFetch   = "agentxx_web_fetch";
constexpr std::string_view kNameFetchMd = "agentxx_web_fetch_markdown";

constexpr std::string_view kDepictSearch =
    R"(Perform a web search. Returns a markdown-formatted list of results.
Use `agentxx_web_fetch_markdown` afterwards to retrieve full page content from a result.)";
constexpr std::string_view kDepictFetch
    = "Perform an HTTP GET request and return the raw response body.";
constexpr std::string_view kDepictFetchMd =
    R"(Perform an HTTP GET request and return the page content converted to Markdown.
Commonly used after `agentxx_web_search` to read a specific page.)";

constexpr const char* kHeaderArgDesc =
    R"(Custom HTTP request headers to send, as a JSON object of header name to value.
Example: {"X-Api-Key": "xxx", "User-Agent": "agentxx"})";

} // namespace

struct WebsearchPluginCtx : public PluginBase {
    bool              convert_html2markdown = true;
    std::string       search_api_url;
    ModelSearchConfig model_cfg;
    bool              use_model_search = false;
};

/// 注册事务 (start 的实际内容): 读取宿主配置并注册三个工具。
///
/// 三个工具的实现体都是 asio 协程 (`co_await HttpClient::*Async`), 等待的是
/// 插件本地 reactor 上的 socket 就绪事件, 因此注册为**声明式受控轮询**工具
/// (`polled_tool`): 网络等待不再占用宿主工作线程池, 同一实例的并发请求共享
/// 一个本地 reactor 与 HTTP keep-alive 连接池。
static int32_t websearchSetup(WebsearchPluginCtx& ctx) {
        if (ctx.iface.model && ctx.iface.model->get_config) {
            AgentxxPluginString json{nullptr, 0};
            ctx.iface.model->get_config(ctx.host, &json);
            if (json.data) {
                std::string cfgJson(json.data, static_cast<size_t>(json.size));
                PluginString::free(ctx.host, &json);
                try {
                    auto cfg                  = agentxx::util::Json::parse(cfgJson);
                    ctx.convert_html2markdown = cfg.value("websearchConvertHtml2markdown", true);
                    ctx.search_api_url        = cfg.value("websearchApiUrl", std::string{});
                    if (cfg.contains("websearchModel") && cfg["websearchModel"].is_object()) {
                        const auto& m           = cfg["websearchModel"];
                        ctx.model_cfg.baseUrl   = m.value("baseUrl", std::string{});
                        ctx.model_cfg.apiKey    = m.value("apiKey", std::string{"EMPTY"});
                        ctx.model_cfg.modelName = m.value("modelName", std::string{"Agentxx"});
                        ctx.model_cfg.readChunkTimeoutSeconds
                            = m.value("readChunkTimeoutSeconds", 100);
                        if (!ctx.model_cfg.baseUrl.empty()) {
                            ctx.use_model_search = true;
                        }
                    }
                } catch (...) {
                }
            }
        }

        // 1. fetch
        auto fetchSchema
            = ctx.schema(kNameFetch)
                  .string("url", "Absolute HTTP/HTTPS URL to fetch.", /*required=*/true)
                  .number(
                      "timeout",
                      "Default `30` seconds. Request timeout in seconds.",
                      false,
                      30.0
                  )
                  .array("header", kHeaderArgDesc, "object")
                  .build();

        polled_tool(
            ctx,
            kNameFetch,
            kDepictFetch,
            fetchSchema,
            [](WebsearchPluginCtx&,
               std::string_view args_json,
               std::string_view,
               std::string_view,
               const AgentxxPluginCancelToken* cancel_token) -> asio::awaitable<std::string> {
                if (agentxx_plugin_cancel_is_requested(cancel_token)) {
                    throw CancelledException("web_fetch cancelled");
                }
                ArgReader args(args_json);
                // args 是本协程帧的局部量: 其生命周期覆盖整个 co_await
                auto result = co_await webFetchExecuteAsync(args.raw());
                if (agentxx_plugin_cancel_is_requested(cancel_token)) {
                    throw CancelledException("web_fetch cancelled");
                }
                co_return result;
            }
        );

        // 2. fetch_markdown
        auto fetchMdSchema = ctx.schema(kNameFetchMd)
                                 .string(
                                     "url",
                                     R"(Absolute HTTP/HTTPS URL to fetch.

When resolving relative links found in the returned Markdown, combine them with this `url`:
- Page `http://example.com/help/`:
  - `model/delete/` (no leading /) → `http://example.com/help/model/delete/`
  - `./model/create/` (leading .) → `http://example.com/help/model/create/`
  - `../model/create/` (leading ..) → `http://example.com/model/create/`
  - `/model/view/` (leading /) → `http://example.com/model/view/`
- Page `http://example.com/help/what.html`:
  - `model/delete/` (no leading /) → strip filename, append → `http://example.com/help/model/delete/`
)",
                                     /*required=*/true
                                 )
                                 .number(
                                     "timeout",
                                     "Default `15` seconds. Request timeout in seconds.",
                                     false,
                                     15.0
                                 )
                                 .array("header", kHeaderArgDesc, "object")
                                 .build();

        polled_tool(
            ctx,
            kNameFetchMd,
            kDepictFetchMd,
            fetchMdSchema,
            [](WebsearchPluginCtx&,
               std::string_view args_json,
               std::string_view,
               std::string_view,
               const AgentxxPluginCancelToken* cancel_token) -> asio::awaitable<std::string> {
                if (agentxx_plugin_cancel_is_requested(cancel_token)) {
                    throw CancelledException("web_fetch_markdown cancelled");
                }
                ArgReader args(args_json);
                auto      result = co_await webFetchMarkdownExecuteAsync(args.raw());
                if (agentxx_plugin_cancel_is_requested(cancel_token)) {
                    throw CancelledException("web_fetch_markdown cancelled");
                }
                co_return result;
            }
        );

        // 3. search
        if (ctx.use_model_search || !ctx.search_api_url.empty()) {
            auto searchSchema = ctx.schema(kNameSearch)
                                    .string(
                                        "query",
                                        "The search query string to look up on the web.",
                                        /*required=*/true
                                    )
                                    .number(
                                        "timeout",
                                        "Default `20` seconds. Search request timeout in seconds.",
                                        false,
                                        20.0
                                    )
                                    .build();

            polled_tool(
                ctx,
                kNameSearch,
                kDepictSearch,
                searchSchema,
                [](WebsearchPluginCtx& c,
                   std::string_view args_json,
                   std::string_view,
                   std::string_view,
                   const AgentxxPluginCancelToken* cancel_token) -> asio::awaitable<std::string> {
                    if (agentxx_plugin_cancel_is_requested(cancel_token)) {
                        throw CancelledException("web_search cancelled");
                    }
                    ArgReader   args(args_json);
                    std::string result;
                    if (c.use_model_search) {
                        ModelSearchConfig mcfg = c.model_cfg;
                        result = co_await modelWebSearchExecuteAsync(args.raw(), mcfg);
                    } else {
                        result = co_await webSearchExecuteAsync(
                            args.raw(),
                            c.search_api_url,
                            c.convert_html2markdown
                        );
                    }
                    if (agentxx_plugin_cancel_is_requested(cancel_token)) {
                        throw CancelledException("web_search cancelled");
                    }
                    co_return result;
                }
            );
        }

    return 0;
}

static void* websearchStart(
    WebsearchPluginCtx& ctx, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString* error
) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host, error, "agentxx_websearch start: notify required"
            );
        }
        return nullptr;
    }
    if (websearchSetup(ctx) != 0) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host, error, "agentxx_websearch start: registration failed"
            );
        }
        return nullptr;
    }
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* websearchStop(
    WebsearchPluginCtx&, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*
) {
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_EXPORT(
    WebsearchPluginCtx,
    "agentxx_websearch",
    "1.0.0",
    "Web access tools: HTTP fetch, Markdown fetch, and web search",
    websearchStart,
    websearchStop
);

struct WebsearchClientCtx : public ClientPluginBase {};

/// client 侧 start: 注册 3 个工具的折叠头模版 (stop 无需撤销, 宿主负责摘除)
static void* websearchClientStart(
    WebsearchClientCtx& ctx, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*
) {
    ctx.registerTemplate(kNameSearch, "Search", "query");
    ctx.registerTemplate(kNameFetch, "Fetch", "url");
    ctx.registerTemplate(kNameFetchMd, "FetchMd", "url");
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* websearchClientStop(
    WebsearchClientCtx&, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*
) {
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
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
