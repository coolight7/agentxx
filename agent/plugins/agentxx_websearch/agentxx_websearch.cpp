/// agentxx_websearch —— 网络访问工具插件
#include "agentxx_websearch_plugin.h"
#include "utilxx_base/json.h"
#include "websearch_impl.h"
#include <condition_variable>
#include <mutex>
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

/// 在工作线程上等一次网络调用的结果 (回调 + 条件变量)
///
/// - 主程序把完成回调派发在**主程序 io 线程**上, 所以在这里等不会自锁;
/// - 在主程序 io 线程上等会把整个主程序卡住, 这种情况直接按错误返回;
/// - 工具实现体本来就是阻塞形态 (`tool`), 所以可以这么等。
///   协程里不要用它的思路 (那是 `co_await` 的事)。
static agentxx::plugin::NetResult netCallWait(
    const PluginxxHost*           host,
    const PluginxxNetIface*       net,
    const PluginxxSchedulerIface* scheduler,
    std::string_view              op,
    std::string_view              argsJson
) {
    agentxx::plugin::NetResult result;
    if (!host || !net || !net->call || op.empty()) {
        return result;
    }
    if (scheduler && scheduler->is_io_thread && scheduler->is_io_thread(host) != 0) {
        result.code = PLUGINXX_RESULT_ERR_STATE;
        result.json = R"({"ok":false,"error":"wrong_thread"})";
        return result;
    }

    struct WaitState {
        std::mutex              mutex;
        std::condition_variable cv;
        bool                    done   = false;
        int32_t                 status = PLUGINXX_OPERATOR_FAILED;
        std::string             payload;
    } state;

    PluginxxOperatorCallback cb
        = [](void* ud, int32_t status, const PluginxxStringView* payload) {
        auto* s = static_cast<WaitState*>(ud);
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->done   = true;
            s->status = status;
            if (payload && payload->data && payload->size > 0) {
                s->payload.assign(payload->data, static_cast<size_t>(payload->size));
            }
        }
        s->cv.notify_one();
    };

    const auto     opSv   = agentxx::plugin::PluginStringView::from(op.data(), op.size());
    const auto     argsSv = agentxx::plugin::PluginStringView::from(argsJson.data(), argsJson.size());
    PluginxxString err{nullptr, 0};
    int64_t        requestId = 0;
    auto*          handle = net->call(host, &opSv, &argsSv, &requestId, cb, &state, &err);
    if (handle == nullptr) {
        result.code = PLUGINXX_RESULT_ERR_STATE;
        if (err.data) {
            result.json.assign(err.data, static_cast<size_t>(err.size));
            agentxx::plugin::PluginString::free(host, &err);
        }
        return result;
    }
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        state.cv.wait(lock, [&state]() {
            return state.done;
        });
    }
    result.code = PLUGINXX_RESULT_OK;
    if (state.status == PLUGINXX_OPERATOR_OK) {
        result.json = std::move(state.payload);
    } else if (state.status == PLUGINXX_OPERATOR_CANCELED) {
        result.json = R"({"ok":false,"error":"canceled"})";
    } else {
        result.json = state.payload.empty() ? std::string{R"({"ok":false,"error":"failed"})"}
                                            : std::move(state.payload);
    }
    return result;
}

} // namespace

struct WebsearchPluginCtx : public PluginBase {
    bool              convert_html2markdown = true;
    std::string       search_api_url;
    ModelSearchConfig model_cfg;
    bool              use_model_search = false;
};

/// 把一次请求交给主程序网络服务 (`<应用名>.net` 的 `http.fetch`)
///
/// 工具实现体跑在主程序工作线程上, 所以在那里等结果 ([netCallWait]);
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

    const auto result = netCallWait(
        ctx.host,
        ctx.iface.net,
        ctx.iface.scheduler,
        pluginxx::net_op::httpFetch,
        options.toArgsJson(spec.url)
    );
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
/// `http.fetch` (异步受理 + 工作线程上等结果), 因此注册为 `tool` ——
/// 等待期间只占一个工作线程, 不在主程序 io 线程上阻塞。
static asio::awaitable<void> websearchSetupAsync(WebsearchPluginCtx& ctx, ScopeRef ref) {
    if (ctx.iface.model && ctx.iface.model->get_config) {
        auto        cfgRes  = co_await agentxx::plugin::modelConfigJsonAsync(ctx, ref.ptr());
        std::string cfgJson = cfgRes.ok() ? std::string{cfgRes.text()} : std::string{};
        if (!cfgJson.empty()) {
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
        = (co_await schemaOfAsync(ctx, kNameFetch))
              .string("url", kFetchUrlDesc, /*required=*/true)
              .enumString("format", kFetchFormatDesc, {"markdown", "raw"}, false, "markdown")
              .number("timeout", kFetchTimeoutDesc, false, 30.0)
              .object("header", kHeaderArgDesc)
              .build();

    co_await toolAsync(
        ctx,
        ref,
        kNameFetch,
        kDepictFetch,
        fetchSchema,
        agentxx::plugin::onWorkerAsync([](WebsearchPluginCtx&      c,
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
        }),
        0,
        kAutoSummary
    );

    // 2. search
    if (ctx.use_model_search || !ctx.search_api_url.empty()) {
        auto searchSchema = (co_await schemaOfAsync(ctx, kNameSearch))
                                .string("query", kSearchQueryDesc, /*required=*/true)
                                .number("timeout", kSearchTimeoutDesc, false, 20.0)
                                .object("header", kHeaderArgDesc)
                                .build();

        co_await toolAsync(
            ctx,
            ref,
            kNameSearch,
            kDepictSearch,
            searchSchema,
            agentxx::plugin::onWorkerAsync([](WebsearchPluginCtx&      c,
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
            }),
            0,
            kAutoSummary
        );
    }

    co_return;
}

static void websearchStart( 
    WebsearchPluginCtx&           ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*
) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    (void)ctx;
    return;
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
    auto later = finishLater(notify);
    if (!ctx.iface.tasks || !ctx.iface.tasks->register_task) {
        later.failed("agentxx_websearch mount: host has no pluginxx.tasks (spawn unavailable)");
        return later.provider();
    }
    const AgentRef agentRef(*spec);
    // 注册事务交给协程: 每个注册都是"提交 + co_await", 全部生效后再放行挂载
    ctx.spawn([later, agentRef](WebsearchPluginCtx& c, OpCtl&) -> asio::awaitable<void> {
        try {
            co_await websearchSetupAsync(c, agentRef);
            later.ok();
        } catch (const std::exception& e) {
            later.failed(e.what());
        }
        co_return;
    });
    return later.provider();
}

static void websearchUnmount(
    WebsearchPluginCtx&, void*, const PluginxxOperatorNotify* notify, PluginxxString*) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return;
}

static void websearchStop(
    WebsearchPluginCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return;
}

AGENTXX_AGENT_PLUGIN_EXPORT(
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

/// 工具行内容的生成函数 (声明式写法)
///
/// - `ud` = 参数里取哪个字段做那一行 (`query` / `url`)
/// - 生成函数跑在插件执行线程, 只读 `ctx_json` 里声明过的变量;
///   主程序把内容的纯文本当折叠头那一行, `blocks` 当展开体
static void PLUGINXX_CALL websearchRowGenerate(
    const PluginxxHost*           /*host*/,
    const PluginxxStringView*     ctx_json,
    void*                         ud,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               /*error_out*/
) {
    if (notify == nullptr || notify->done == nullptr) {
        return;
    }
    utilxx_base::Json ctx = utilxx_base::Json::object();
    if (ctx_json != nullptr && ctx_json->data != nullptr) {
        try {
            ctx = utilxx_base::Json::parse(
                std::string{ctx_json->data, static_cast<size_t>(ctx_json->size)}
            );
        } catch (...) {
            ctx = utilxx_base::Json::object();
        }
    }
    const auto* field = static_cast<const char*>(ud);
    utilxx_base::Json block;
    block["kind"] = "Text";
    block["text"] = ArgReader(ctx.value("agentxx.client.tool.args", std::string{}))
                         .value(field != nullptr ? field : "", "");
    utilxx_base::Json doc;
    doc["blocks"] = utilxx_base::Json::array({block});
    utilxx_base::Json body;
    body["json"]           = doc;
    const std::string        payload = body.dump();
    const PluginxxStringView view{payload.data(), payload.size()};
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &view);
}

/// 把一个工具的行渲染注册成声明项 + 内容
/// - 声明项: `type` = `tool` (短名 → `agentxx.client.tool`), data 给工具名与显示名
/// - 内容: 只声明的变量是这次调用的参数与折叠/展开标识
/// - 主程序不支持声明式界面时什么也不做 (界面退回默认渲染)
static void registerToolRow(
    WebsearchClientCtx& ctx,
    std::string_view    tool,
    std::string_view    displayName,
    const char*         field
) {
    if (ctx.host == nullptr || ctx.iface.ui == nullptr
        || ctx.iface.ui->register_ui_item == nullptr
        || ctx.iface.ui->register_ui_content == nullptr) {
        return;
    }
    const std::string data = utilxx_base::Json{
                                 {"tool", std::string{tool}},
                                 {"displayName", std::string{displayName}},
    }
                                 .dump();
    const std::string deps
        = R"_(["agentxx.client.tool.args","agentxx.client.tool.part"])_";

    PluginxxUiEntrySpec entry{};
    entry.version     = 1;
    entry.struct_size = sizeof(PluginxxUiEntrySpec);
    entry.item_id     = PluginxxStringView{tool.data(), tool.size()};
    entry.type        = PluginxxStringView{"tool", 4};
    entry.data_json   = PluginxxStringView{data.data(), data.size()};
    if (ctx.iface.ui->register_ui_item(ctx.host, &entry, nullptr, nullptr, nullptr) == nullptr) {
        return;
    }

    PluginxxUiContentSpec content{};
    content.version     = 1;
    content.struct_size = sizeof(PluginxxUiContentSpec);
    content.id          = PluginxxStringView{tool.data(), tool.size()};
    content.slot        = PluginxxStringView{"tool", 4};
    content.title       = PluginxxStringView{displayName.data(), displayName.size()};
    content.deps_json   = PluginxxStringView{deps.data(), deps.size()};
    content.generate    = &websearchRowGenerate;
    content.user_data   = const_cast<char*>(field);
    ctx.iface.ui->register_ui_content(ctx.host, &content, nullptr, nullptr, nullptr);
}

/// client 侧 start: 注册工具行 (显示名 + 折叠内容都来自同一份声明)
/// - 折叠内容是一行纯文本 (参数里的 `query` / `url`), **不带**分隔符 " · "
///   (头行格式由界面统一拼装)
static void websearchClientStart(
    WebsearchClientCtx&           ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*
) {
    registerToolRow(ctx, kNameSearch, "Search", "query");
    registerToolRow(ctx, kNameFetch, "Fetch", "url");
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return;
}

static void    websearchClientStop(WebsearchClientCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return;
}

AGENTXX_CLIENT_PLUGIN_EXPORT(
    WebsearchClientCtx,
    "agentxx_websearch",
    "1.0.0",
    "Websearch tools specialized UI renderer",
    websearchClientStart,
    websearchClientStop
);
