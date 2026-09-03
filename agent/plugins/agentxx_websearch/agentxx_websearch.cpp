// agentxx_websearch —— 网络访问工具插件
#include "agentxx_websearch_plugin.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "websearch_impl.h"
#include <string>

using namespace agentxx_websearch_plugin;

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

const char* kHeaderArgDesc =
    R"(Custom HTTP request headers to send, as a JSON object of header name to value.
Example: {"X-Api-Key": "xxx", "User-Agent": "agentxx"})";
const char* kTimeoutDesc = "Default `{}` seconds. Request timeout in seconds.";

std::string timeoutDesc(int def) {
    return fmt::format(fmt::runtime(kTimeoutDesc), def);
}

std::string schemaFetch(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameFetch);
    return neograph::json{
        {"type", "object"},
        {
         "properties", {{
                 "url",
                 {
                     {"type", "string"},
                     {
                         "description",
                         agentxx::plugin::
                             toolPromptArgDesc(p, "url", "Absolute HTTP/HTTPS URL to fetch."),
                     },
                 },
             },
             {
                 "timeout",
                 {
                     {"type", "number"},
                     {"default", 30},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(p, "timeout", timeoutDesc(30)),
                     },
                 },
             },
             {
                 "header",
                 {
                     {"type", "object"},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(p, "header", kHeaderArgDesc),
                     },
                 },
             }},
         },
        {"required", neograph::json::array({"url"})}
    }.dump();
}

std::string schemaFetchMd(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameFetchMd);
    return neograph::json{
        {"type", "object"},
        {
         "properties", {{
                 "url",
                 {
                     {"type", "string"},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "url",
                             "Absolute HTTP/HTTPS URL to fetch.\n\n"
                             "When resolving relative links found in the returned Markdown, combine them with this `url`:\n"
                             "- Page `http://example.com/help/`:\n"
                             "  - `model/delete/` (no leading /) → `http://example.com/help/model/delete/`\n"
                             "  - `./model/create/` (leading .) → `http://example.com/help/model/create/`\n"
                             "  - `../model/create/` (leading ..) → `http://example.com/model/create/`\n"
                             "  - `/model/view/` (leading /) → `http://example.com/model/view/`\n"
                             "- Page `http://example.com/help/what.html`:\n"
                             "  - `model/delete/` (no leading /) → strip filename, append → `http://example.com/help/model/delete/`\n"
                         ),
                     },
                 },
             },
             {
                 "timeout",
                 {
                     {"type", "number"},
                     {"default", 15},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(p, "timeout", timeoutDesc(15)),
                     },
                 },
             },
             {
                 "header",
                 {
                     {"type", "object"},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(p, "header", kHeaderArgDesc),
                     },
                 },
             }},
         },
        {"required", neograph::json::array({"url"})}
    }.dump();
}

std::string schemaSearch(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameSearch);
    return neograph::json{
        {"type", "object"},
        {
         "properties", {{
                 "query",
                 {
                     {"type", "string"},
                     {"minLength", 1},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "query",
                             "Natural language search query. Should be a semantically rich description of the ideal page, not just keywords."
                         ),
                     },
                 },
             },
             {
                 "numResults",
                 {
                     {"type", "number"},
                     {"default", 5},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "numResults",
                             "Number of search results to return (default: 5)."
                         ),
                     },
                 },
             }},
         },
        {"required", neograph::json::array({"query"})}
    }.dump();
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
            static const AgentxxPluginInfo info{
                AGENTXX_PLUGIN_API_VERSION, 0,
                agentxx::plugin::PluginStringView::fromCstr("agentxx_websearch"),
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "Web search & fetch tools: web_search, web_fetch, web_fetch_markdown"
                ),
            };
            return &info;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    PluginCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* msg) noexcept {
            ctxGuardLogger(raw)(msg);
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx = std::make_unique<PluginCtx>();
            ctx->init(host);
            raw = ctx.get();

            if (ctx->iface.model && ctx->iface.model->get_config) {
                AgentxxPluginString json{nullptr, 0};
                ctx->iface.model->get_config(ctx->host, &json);
                if (json.data) {
                    std::string cfgJson(json.data, static_cast<size_t>(json.size));
                    agentxx::plugin::PluginString::free(ctx->host, &json);
                    try {
                        auto cfg = neograph::json::parse(cfgJson);
                        ctx->convert_html2markdown
                            = cfg.value("websearchConvertHtml2markdown", true);
                        ctx->search_api_url = cfg.value("websearchApiUrl", std::string{});
                        if (cfg.contains("websearchModel") && cfg["websearchModel"].is_object()) {
                            const auto& m            = cfg["websearchModel"];
                            ctx->model_cfg.baseUrl   = m.value("baseUrl", std::string{});
                            ctx->model_cfg.apiKey    = m.value("apiKey", std::string{"EMPTY"});
                            ctx->model_cfg.modelName = m.value("modelName", std::string{"Agentxx"});
                            ctx->model_cfg.readChunkTimeoutSeconds
                                = m.value("readChunkTimeoutSeconds", 100);
                            if (!ctx->model_cfg.baseUrl.empty()) {
                                ctx->use_model_search = true;
                            }
                        }
                    } catch (...) {
                    }
                }
            }

            if (!ctx->iface.tools || !ctx->iface.tools->register_tool) {
                return -1;
            }

            // 1. agentxx_web_fetch（阻塞池 + 临时 io_context 同步驱动异步 HttpClient）
            agentxx::plugin::blocking_tool(
                *ctx,
                kNameFetch,
                kDepictFetch,
                schemaFetch(ctx.get()),
                [](PluginCtx&, std::string_view args_json) -> std::string {
                    auto               arguments = args_json.empty() ? neograph::json::object()
                                                                     : neograph::json::parse(args_json);
                    asio::io_context   io;
                    std::string        result;
                    std::exception_ptr ep;
                    asio::co_spawn(
                        io,
                        [&]() -> asio::awaitable<void> {
                            try {
                                result = co_await webFetchExecuteAsync(arguments);
                            } catch (...) {
                                ep = std::current_exception();
                            }
                        },
                        asio::detached
                    );
                    io.run();
                    if (ep) {
                        std::rethrow_exception(ep);
                    }
                    return result;
                }
            );

            // 2. agentxx_web_fetch_markdown
            agentxx::plugin::blocking_tool(
                *ctx,
                kNameFetchMd,
                kDepictFetchMd,
                schemaFetchMd(ctx.get()),
                [](PluginCtx&, std::string_view args_json) -> std::string {
                    auto               arguments = args_json.empty() ? neograph::json::object()
                                                                     : neograph::json::parse(args_json);
                    asio::io_context   io;
                    std::string        result;
                    std::exception_ptr ep;
                    asio::co_spawn(
                        io,
                        [&]() -> asio::awaitable<void> {
                            try {
                                result = co_await webFetchMarkdownExecuteAsync(arguments);
                            } catch (...) {
                                ep = std::current_exception();
                            }
                        },
                        asio::detached
                    );
                    io.run();
                    if (ep) {
                        std::rethrow_exception(ep);
                    }
                    return result;
                }
            );

            // 3. agentxx_web_search（同上，模型/ API 搜索）
            if (ctx->use_model_search || !ctx->search_api_url.empty()) {
                agentxx::plugin::blocking_tool(
                    *ctx,
                    kNameSearch,
                    kDepictSearch,
                    schemaSearch(ctx.get()),
                    [](PluginCtx& c, std::string_view args_json) -> std::string {
                        auto               arguments = args_json.empty() ? neograph::json::object()
                                                                         : neograph::json::parse(args_json);
                        asio::io_context   io;
                        std::string        result;
                        std::exception_ptr ep;
                        asio::co_spawn(
                            io,
                            [&]() -> asio::awaitable<void> {
                                try {
                                    if (c.use_model_search) {
                                        ModelSearchConfig mcfg;
                                        mcfg.baseUrl   = c.model_cfg.baseUrl;
                                        mcfg.apiKey    = c.model_cfg.apiKey;
                                        mcfg.modelName = c.model_cfg.modelName;
                                        mcfg.readChunkTimeoutSeconds
                                            = c.model_cfg.readChunkTimeoutSeconds;
                                        result
                                            = co_await modelWebSearchExecuteAsync(arguments, mcfg);
                                    } else {
                                        result = co_await webSearchExecuteAsync(
                                            arguments,
                                            c.search_api_url,
                                            c.convert_html2markdown
                                        );
                                    }
                                } catch (...) {
                                    ep = std::current_exception();
                                }
                            },
                            asio::detached
                        );
                        io.run();
                        if (ep) {
                            std::rethrow_exception(ep);
                        }
                        return result;
                    }
                );
            }

            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(ctxGuardLogger(ctx), [&] {
        delete ctx;
    });
}
