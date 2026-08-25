// agentxx_websearch —— 网络访问工具插件
// - 从 libagentxx src/tools/web_search 拆分独立 (同名同行为):
//   - agentxx_web_fetch            (原 WebFetchUrlTool)
//   - agentxx_web_fetch_markdown   (原 WebFetchUrlMarkdownTool)
//   - agentxx_web_search           (原 WebSearchTool / ModelWebSearchTool)
// - 搜索配置 (websearchApiUrl / convertHtml2markdown / websearchModel) 经宿主
//   agentxx.agent.model 接口表读取 (原 lib AgentConfig 同名字段, yaml 配置不变);
//   两者均未配置时不注册 agentxx_web_search (与原 lib 行为一致)
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

/// 注册常规工具 (schema/描述存储于插件侧静态区; 宿主注册时拷贝)
/// - 统一异步操作模型: 经阻塞委托垫片注册 (offload 池线程执行, io 线程
///   只等完成通知); execute 签名追加 cancel_flag 形参 (本插件忽略)
void registerTool(
    const char*        name,
    const char*        defaultDepict,
    const std::string& schema,
    char* (*execute)(
        void*                   user_data,
        AgentxxPluginStringView args_json,
        AgentxxPluginStringView thread_id,
        AgentxxPluginStringView tool_call_id,
        volatile int*           cancel_flag,
        char**                  error_out
    ),
    int flags = 0
) {
    static std::vector<std::string> g_storage;
    std::string                     depict = readToolPrompt(name).depict;
    if (depict.empty()) {
        depict = defaultDepict;
    }
    g_storage.push_back(std::move(depict));
    g_storage.push_back(schema);

    AgentxxSyncToolSpec spec{};
    spec.name        = agentxx_plugin_sv(name, std::strlen(name));
    spec.description = agentxx_plugin_sv(
        g_storage[g_storage.size() - 2].data(),
        g_storage[g_storage.size() - 2].size()
    );
    spec.parameters_json = agentxx_plugin_sv(g_storage.back().data(), g_storage.back().size());
    spec.user_data       = nullptr;
    spec.flags           = flags;
    spec.execute         = execute;
    if (agentxx_register_sync_tool(g_host, &spec) != 0) {
        XX_LOGW("agentxx_websearch: register tool {} failed", name);
    }
}

/// C ABI execute 包装: 解析参数 JSON → 调用实现 → 结果 strdup (异常不外泄)
template<auto ExecFn>
char* wrapExecute(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView thread_id,
    AgentxxPluginStringView tool_call_id,
    volatile int*           cancel_flag,
    char**                  error_out
) {
    (void)user_data;
    (void)thread_id;
    (void)tool_call_id;
    (void)cancel_flag;
    try {
        std::string argsStr(args_json.data ? args_json.data : "", args_json.size);
        auto arguments = argsStr.empty() ? neograph::json::object() : neograph::json::parse(argsStr);
        auto result    = ExecFn(arguments);
        return pluginStrdup(result.c_str());
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = pluginStrdup(ex.what());
        }
        return nullptr;
    } catch (...) {
        if (error_out) {
            *error_out = pluginStrdup("unknown exception");
        }
        return nullptr;
    }
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL (宿主按"未导出"处理)
    XX_PGUARD_BEGIN
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("agentxx_websearch"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("Web tools: search (api url / model), fetch url raw/markdown"),
    };
    return &info;
    XX_PGUARD_END_RET(nullptr)
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: entry 内含 JSON schema 构建等可抛操作, 异常返回 -1
    XX_PGUARD_BEGIN
    if (!host || !host->vtable || !plugin_ctx) {
        return -1;
    }
    g_host      = host;
    g_if        = agentxx::plugin::AgentIfaces::query(host);
    *plugin_ctx = nullptr;

    // ---- agentxx_web_fetch ----
    {
        ToolPromptText p      = readToolPrompt(kNameFetch);
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
        registerTool(kNameFetch, kDepictFetch, schema, &wrapExecute<agentxx::websearch_plugin::webFetchExecute>, AGENTXX_TOOL_FLAG_AUTO_SUMMARY);
    }

    // ---- agentxx_web_fetch_markdown ----
    {
        ToolPromptText p      = readToolPrompt(kNameFetchMd);
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
        registerTool(
            kNameFetchMd,
            kDepictFetchMd,
            schema,
            &wrapExecute<agentxx::websearch_plugin::webFetchMarkdownExecute>,
            AGENTXX_TOOL_FLAG_AUTO_SUMMARY
        );
    }

    // ---- agentxx_web_search (配置驱动; 与原 lib 行为一致) ----
    // 从宿主 agentxx.agent.model 接口表读取搜索配置 (yaml 同名字段)
    if (g_if.model && g_if.model->get_config) {
        char*         json = g_if.model->get_config(g_host);
        std::string   cfgJson;
        neograph::json cfg;
        bool          hasCfg = false;
        if (json) {
            cfgJson = json;
            g_host->vtable->free(json);
            try {
                cfg    = neograph::json::parse(cfgJson);
                hasCfg = true;
            } catch (...) {
                hasCfg = false;
            }
        }

        // 静态上下文: 搜索方式与参数 (entry 时装配, execute 回调只读)
        static std::string     g_searchApiUrl;
        static bool            g_convertHtml2markdown = true;
        static agentxx::websearch_plugin::ModelSearchConfig g_modelCfg;
        static bool            g_useModelSearch = false;

        if (hasCfg) {
            if (cfg.contains("websearchModel") && cfg["websearchModel"].is_object()) {
                // 模型搜索优先 (与原 lib 判断顺序一致)
                const auto& w = cfg["websearchModel"];
                g_modelCfg.baseUrl = w.value("baseUrl", std::string{});
                g_modelCfg.apiKey  = w.value("apiKey", std::string{"EMPTY"});
                g_modelCfg.modelName = w.value("modelName", std::string{"Agentxx"});
                g_useModelSearch    = true;
            } else if (cfg.contains("websearchApiUrl") && cfg["websearchApiUrl"].is_string()
                       && false == cfg["websearchApiUrl"].get<std::string>().empty()) {
                g_searchApiUrl        = cfg["websearchApiUrl"].get<std::string>();
                g_convertHtml2markdown = cfg.value("websearchConvertHtml2markdown", true);
            }
        }

        if (g_useModelSearch || false == g_searchApiUrl.empty()) {
            ToolPromptText p      = readToolPrompt(kNameSearch);
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

            static const auto execSearch
                = [](void*                   user_data,
                     AgentxxPluginStringView args_json,
                     AgentxxPluginStringView thread_id,
                     AgentxxPluginStringView tool_call_id,
                     volatile int*           cancel_flag,
                     char**                  error_out) -> char* {
                (void)user_data;
                (void)thread_id;
                (void)tool_call_id;
                (void)cancel_flag;
                try {
                    std::string argsStr(args_json.data ? args_json.data : "", args_json.size);
                    auto arguments
                        = argsStr.empty() ? neograph::json::object() : neograph::json::parse(argsStr);
                    std::string result;
                    if (g_useModelSearch) {
                        result = agentxx::websearch_plugin::modelWebSearchExecute(
                            arguments,
                            g_modelCfg
                        );
                    } else {
                        result = agentxx::websearch_plugin::webSearchExecute(
                            arguments,
                            g_searchApiUrl,
                            g_convertHtml2markdown
                        );
                    }
                    return pluginStrdup(result.c_str());
                } catch (const std::exception& ex) {
                    if (error_out) {
                        *error_out = pluginStrdup(ex.what());
                    }
                    return nullptr;
                } catch (...) {
                    if (error_out) {
                        *error_out = pluginStrdup("unknown exception");
                    }
                    return nullptr;
                }
            };

            registerTool(kNameSearch, kDepictSearch, schema, execSearch, AGENTXX_TOOL_FLAG_AUTO_SUMMARY);
        } else {
            XX_LOGI(
                "agentxx_websearch: no websearch config (websearch_api_url / websearch_model), "
                "`agentxx_web_search` not registered"
            );
        }
    } else {
        XX_LOGW("agentxx_websearch: host model iface unavailable, `agentxx_web_search` not registered");
    }

    return 0;
    XX_PGUARD_END_RET(-1)
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_unload(void* plugin_ctx) {
    // C ABI 边界异常守卫: 卸载回调异常不得外泄
    XX_PGUARD_BEGIN
    (void)plugin_ctx;
    g_host = nullptr;
    g_if   = {};
    XX_PGUARD_END_VOID()
}
