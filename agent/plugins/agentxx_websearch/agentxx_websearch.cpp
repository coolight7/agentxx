// agentxx_websearch —— 网络访问工具插件
// - 从 libagentxx src/tools/web_search 拆分独立 (同名同行为):
//   - agentxx_web_fetch            (原 WebFetchUrlTool)
//   - agentxx_web_fetch_markdown   (原 WebFetchUrlMarkdownTool)
//   - agentxx_web_search           (原 WebSearchTool / ModelWebSearchTool)
// - 搜索配置 (websearchApiUrl / convertHtml2markdown / websearchModel) 经宿主
//   agentxx.agent.model 接口表读取 (原 lib AgentConfig 同名字段, yaml 配置不变);
//   两者均未配置时不注册 agentxx_web_search (与原 lib 行为一致)
// - 统一异步操作模型 (poll 寄生驱动): 工作协程在实例 PollLoop 上 spawn, 由
//   宿主 io 线程非阻塞步进, 与内置工具同线程交错执行; HTTP 等待期不占任何
//   宿主线程池线程 (原局部 io_context 同步驱动模式已移除)
// - 业务逻辑在 websearch_impl.h (纯函数, 测试直测同一实现)
#include "agentxx_websearch_plugin.h"
#include "websearch_impl.h"
#include <cstring>
#include <string>
#include <vector>

using namespace agentxx_websearch_plugin;

namespace {

constexpr auto kNameSearch     = "agentxx_web_search";
constexpr auto kNameFetch      = "agentxx_web_fetch";
constexpr auto kNameFetchMd    = "agentxx_web_fetch_markdown";

constexpr auto kDepictSearch =
    R"(Perform a web search. Returns a markdown-formatted list of results.
Use `agentxx_web_fetch_markdown` afterwards to retrieve full page content from a result.)";
constexpr auto kDepictFetch    = "Perform an HTTP GET request and return the raw response body.";
constexpr auto kDepictFetchMd =
    R"(Perform an HTTP GET request and return the page content converted to Markdown.
Commonly used after `agentxx_web_search` to read a specific page.)";

/// 参数说明兜底 (正常情况下由宿主 toolPrompt 提供完整文案)
/// fallback 兼容字面量与运行期拼接的说明文本 (timeout 描述按默认值动态生成)
std::string argDesc(const ToolPromptText& p, const char* key, std::string_view fallback) {
    auto it = p.args.find(key);
    if (it != p.args.end() && !it->second.empty()) {
        return it->second;
    }
    return std::string{fallback};
}

const char* kHeaderArgDesc =
    R"(Custom HTTP request headers to send, as a JSON object of header name to value.
Example: {"X-Api-Key": "xxx", "User-Agent": "agentxx"})";
const char* kTimeoutDesc = "Default `{}` seconds. Request timeout in seconds.";

/// timeout 参数说明按默认值动态生成 (与 lib prompt 文案一致)
std::string timeoutDesc(int def) {
    return fmt::format(fmt::runtime(kTimeoutDesc), def);
}

/// poll 寄生驱动工具注册 (schema/描述存储于本实例 ctx->storage; 宿主注册时拷贝)
/// - 统一异步操作模型: 工作协程在实例 PollLoop 上 spawn, 宿主 io 线程经
///   pollOnce 非阻塞步进 —— 与内置工具同线程交错执行, HTTP 等待期零线程占用
void registerPolledTool(
    PluginCtx*                        ctx,
    const char*                       name,
    const char*                       defaultDepict,
    const std::string&                schema,
    agentxx::plugin::PolledWorkFn     execute,
    int                               flags = 0
) {
    auto&       storage = ctx->storage;
    std::string depict  = readToolPrompt(ctx->host, ctx->iface, name).depict;
    if (depict.empty()) {
        depict = defaultDepict;
    }
    storage.push_back(std::move(depict));
    storage.push_back(schema);

    // 寄生驱动垫片适配器: 实例内嵌存储 (ctx 持有, 随实例销毁释放; 多实例契约)
    auto shim = std::make_unique<agentxx::plugin::PolledToolShim>();

    // 注意: 本文件先包含了 websearch_impl.h (其引入 util/log.h 重定义
    // XX_LOG* 宏为库版签名), 故此处直接调用 pluginLog, 不经 XX_LOGW 宏
    if (agentxx::plugin::register_polled_tool(
            ctx->host,
            agentxx_plugin_sv(name, std::strlen(name)),
            agentxx_plugin_sv(
                storage[storage.size() - 2].data(),
                storage[storage.size() - 2].size()
            ),
            agentxx_plugin_sv(storage.back().data(), storage.back().size()),
            ctx->pollLoop,
            execute,
            ctx, ///< 回调经 PolledJob.userData 恢复本实例上下文
            shim.get(),
            /*timeoutMs=*/0,
            flags
        ) != 0) {
        pluginLog(ctx, 3, fmt::format("agentxx_websearch: register tool {} failed", name));
        return;
    }
    ctx->polled_shims.push_back(std::move(shim));
}

/// 参数 JSON 解析 (公共): PolledJob.args → neograph::json (空按 {} 处理)
inline neograph::json parseJobArgs(const agentxx::plugin::PolledJob& job) {
    auto sv = job.argsView();
    std::string argsStr(sv.data ? sv.data : "{}", sv.size);
    return argsStr.empty() ? neograph::json::object() : neograph::json::parse(argsStr);
}

/// agentxx_web_search 工作协程 (配置取自 user_data 恢复的本实例 PluginCtx;
/// 多实例契约 —— 各实例持有各自配置; 异常由适配器兜底转 OP_FAILED)
static asio::awaitable<agentxx::plugin::PolledOutcome>
    searchWork(agentxx::plugin::PolledJob& job) {
    auto*          ctx       = static_cast<PluginCtx*>(job.userData);
    auto           arguments = parseJobArgs(job);
    if (ctx && ctx->use_model_search) {
        agentxx_websearch_plugin::ModelSearchConfig mc;
        mc.baseUrl                 = ctx->model_cfg.baseUrl;
        mc.apiKey                  = ctx->model_cfg.apiKey;
        mc.modelName               = ctx->model_cfg.modelName;
        mc.readChunkTimeoutSeconds = ctx->model_cfg.readChunkTimeoutSeconds;
        auto result = co_await agentxx_websearch_plugin::modelWebSearchExecuteAsync(arguments, mc);
        co_return agentxx::plugin::PolledOutcome::ok(std::move(result));
    }
    auto result = co_await agentxx_websearch_plugin::webSearchExecuteAsync(
        arguments,
        ctx ? ctx->search_api_url : std::string{},
        ctx ? ctx->convert_html2markdown : true
    );
    co_return agentxx::plugin::PolledOutcome::ok(std::move(result));
}

/// agentxx_web_fetch 工作协程
static asio::awaitable<agentxx::plugin::PolledOutcome>
    fetchWork(agentxx::plugin::PolledJob& job) {
    auto result = co_await agentxx_websearch_plugin::webFetchExecuteAsync(parseJobArgs(job));
    co_return agentxx::plugin::PolledOutcome::ok(std::move(result));
}

/// agentxx_web_fetch_markdown 工作协程
static asio::awaitable<agentxx::plugin::PolledOutcome>
    fetchMarkdownWork(agentxx::plugin::PolledJob& job) {
    auto result = co_await agentxx_websearch_plugin::webFetchMarkdownExecuteAsync(parseJobArgs(job));
    co_return agentxx::plugin::PolledOutcome::ok(std::move(result));
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL (宿主按"未导出"处理);
    // 本边界为纯静态元数据, 无实例上下文可捕获 → 空操作日志闭包
    return agentxx::plugin_guard::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
        static const AgentxxPluginInfo info{
            AGENTXX_PLUGIN_API_VERSION,
            AGENTXX_SV("agentxx_websearch"),
            AGENTXX_SV("1.0.0"),
            AGENTXX_SV("Web tools: search (api url / model), fetch url raw/markdown"),
        };
        return &info;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_create(const AgentxxHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: create 内含 JSON schema 构建等可抛操作, 异常返回 -1;
    // 守卫日志闭包捕获局部裸指针 (ctx 装配前置空 → 异常路径静默丢弃)
    PluginCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
        [&raw](const char* msg) noexcept { pluginLog(raw, 4, msg ? msg : ""); },
        -1,
        [&]() -> int {
        if (!host || !host->vtable || !plugin_ctx) {
            return -1;
        }
        auto ctx   = std::make_unique<PluginCtx>();
        ctx->host  = host;
        ctx->iface = agentxx::plugin::AgentIfaces::query(host);
        raw        = ctx.get();

        // ---- agentxx_web_fetch ----
        {
            ToolPromptText p      = readToolPrompt(ctx->host, ctx->iface, kNameFetch);
            std::string    schema = neograph::json{
                {"type", "object"},
                {"properties",
                 {
                     {"url",
                      {
                          {"type", "string"},
                          {"description", argDesc(p, "url", "Absolute HTTP/HTTPS URL to fetch.")},
                      }},
                     {"timeout",
                      {
                          {"type", "number"},
                          {"description", argDesc(p, "timeout", timeoutDesc(30))},
                      }},
                     {"header",
                      {
                          {"type", "object"},
                          {"description", argDesc(p, "header", kHeaderArgDesc)},
                      }},
                 }},
                {"required", neograph::json::array({"url"})},
            }
                                  .dump();
            registerPolledTool(ctx.get(), kNameFetch, kDepictFetch, schema, &fetchWork, AGENTXX_TOOL_FLAG_AUTO_SUMMARY);
        }

        // ---- agentxx_web_fetch_markdown ----
        {
            ToolPromptText p      = readToolPrompt(ctx->host, ctx->iface, kNameFetchMd);
            std::string    schema = neograph::json{
                {"type", "object"},
                {"properties",
                 {
                     {"url",
                      {
                          {"type", "string"},
                          {"description",
                           argDesc(p,
                                   "url",
                                   R"md(Absolute HTTP/HTTPS URL to fetch.

When resolving relative links found in the returned Markdown, combine them with this `url`:
- Page `http://example.com/help/`:
  - `model/delete/` (no leading /) → `http://example.com/help/model/delete/`
  - `./model/create/` (leading .) → `http://example.com/help/model/create/`
  - `../model/create/` (leading ..) → `http://example.com/model/create/`
  - `/model/view/` (leading /) → `http://example.com/model/view/`
- Page `http://example.com/help/what.html`:
  - `model/delete/` (no leading /) → strip filename, append → `http://example.com/help/model/delete/`
)md")},
                      }},
                     {"timeout",
                      {
                          {"type", "number"},
                          {"description", argDesc(p, "timeout", timeoutDesc(15))},
                      }},
                     {"header",
                      {
                          {"type", "object"},
                          {"description", argDesc(p, "header", kHeaderArgDesc)},
                      }},
                 }},
                {"required", neograph::json::array({"url"})},
            }
                                  .dump();
            registerPolledTool(
                ctx.get(),
                kNameFetchMd,
                kDepictFetchMd,
                schema,
                &fetchMarkdownWork,
                AGENTXX_TOOL_FLAG_AUTO_SUMMARY
            );
        }

        // ---- agentxx_web_search (配置驱动; 与原 lib 行为一致) ----
        // 从宿主 agentxx.agent.model 接口表读取搜索配置 (yaml 同名字段);
        // 配置存入本实例 ctx (原函数级 static 在多实例下会串配置 —— bug 已修)
        if (ctx->iface.model && ctx->iface.model->get_config) {
            char*          json   = ctx->iface.model->get_config(ctx->host);
            std::string    cfgJson;
            neograph::json cfg;
            bool           hasCfg = false;
            if (json) {
                cfgJson = json;
                ctx->host->vtable->free(json);
                try {
                    cfg    = neograph::json::parse(cfgJson);
                    hasCfg = true;
                } catch (...) {
                    hasCfg = false;
                }
            }

            auto& searchApiUrl        = ctx->search_api_url;
            auto& convertHtml2markdown = ctx->convert_html2markdown;
            auto& modelCfg            = ctx->model_cfg;
            auto& useModelSearch       = ctx->use_model_search;

            if (hasCfg) {
                if (cfg.contains("websearchModel") && cfg["websearchModel"].is_object()) {
                    // 模型搜索优先 (与原 lib 判断顺序一致)
                    const auto& w = cfg["websearchModel"];
                    modelCfg.baseUrl = w.value("baseUrl", std::string{});
                    modelCfg.apiKey  = w.value("apiKey", std::string{"EMPTY"});
                    modelCfg.modelName = w.value("modelName", std::string{"Agentxx"});
                    useModelSearch     = true;
                } else if (cfg.contains("websearchApiUrl") && cfg["websearchApiUrl"].is_string()
                           && false == cfg["websearchApiUrl"].get<std::string>().empty()) {
                    searchApiUrl          = cfg["websearchApiUrl"].get<std::string>();
                    convertHtml2markdown  = cfg.value("websearchConvertHtml2markdown", true);
                }
            }

            if (useModelSearch || false == searchApiUrl.empty()) {
                ToolPromptText p      = readToolPrompt(ctx->host, ctx->iface, kNameSearch);
                std::string    schema = neograph::json{
                    {"type", "object"},
                    {"properties",
                     {
                         {"query",
                          {
                              {"type", "string"},
                              {"description", argDesc(p, "query", "The search query string.")},
                          }},
                         {"timeout",
                          {
                              {"type", "number"},
                              {"description", argDesc(p, "timeout", timeoutDesc(15))},
                          }},
                         {"header",
                          {
                              {"type", "object"},
                              {"description", argDesc(p, "header", kHeaderArgDesc)},
                          }},
                     }},
                    {"required", neograph::json::array({"query"})},
                }
                                      .dump();

                registerPolledTool(ctx.get(), kNameSearch, kDepictSearch, schema, &searchWork, AGENTXX_TOOL_FLAG_AUTO_SUMMARY);
            } else {
                pluginLog(
                    ctx.get(),
                    2,
                    "agentxx_websearch: no websearch config (websearch_api_url / websearch_model), "
                    "`agentxx_web_search` not registered"
                );
            }
        } else {
            pluginLog(ctx.get(),
                      3,
                      "agentxx_websearch: host model iface unavailable, `agentxx_web_search` not registered");
        }

        *plugin_ctx = ctx.release(); ///< 所有权移交宿主 (destroy 时取回归还)
        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_destroy(void* plugin_ctx) {
    // C ABI 边界异常守卫: 销毁回调异常不得外泄
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(
        [ctx](const char* msg) noexcept { pluginLog(ctx, 4, msg ? msg : ""); },
        [&] { delete ctx; }); // 垫片适配器为 ctx 内嵌存储, 随 ctx 一并释放
}
