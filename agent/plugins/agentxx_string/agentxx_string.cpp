/// agentxx_string —— 字符串处理工具插件
#include "agentxx_string_plugin.h"
#include "string_impl.h"
#include <string>

using namespace agentxx_string_plugin;

namespace {

constexpr std::string_view kNameHtml2Md = "agentxx_string_html_to_markdown";
constexpr std::string_view kNameRegexp  = "agentxx_string_regexp";

constexpr std::string_view kDepictHtml2Md = "Convert HTML content to Markdown format.";
constexpr std::string_view kDepictRegexp =
    R"(Search, replace, or remove text using regular expressions.
Operates on in-memory text content (not files).)";

std::string schemaHtml2Md(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameHtml2Md);
    return neograph::json{
        {"type", "object"},
        {
         "properties", {{
                "content",
                {
                    {"type", "string"},
                    {
                        "description",
                        agentxx::plugin::
                            toolPromptArgDesc(p, "content", "The HTML string to convert."),
                    },
                },
            }},
         },
        {"required", neograph::json::array({"content"})},
    }
        .dump();
}

std::string schemaRegexp(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameRegexp);
    return neograph::json{
        {"type", "object"},
        {
         "properties", {{
                 "content",
                 {
                     {"type", "string"},
                     {
                         "description",
                         agentxx::plugin::
                             toolPromptArgDesc(p, "content", "The input text to operate on."),
                     },
                 },
             },
             {
                 "exps",
                 {
                     {"type", "array"},
                     {"items", {{"type", "string"}}},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "exps",
                             "Array of regex patterns. A match succeeds if ANY pattern matches."
                         ),
                     },
                 },
             },
             {
                 "opt",
                 {
                     {"type", "string"},
                     {"enum", neograph::json::array({"search", "replace", "remove"})},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "opt",
                             "Operation mode:\n`search`: Return all match results.\n`replace`: Replace matches with `replace_str` and return the resulting text.\n`remove`: Remove all matches and return the resulting text.\n"
                         ),
                     },
                 },
             },
             {
                 "replace_str",
                 {
                     {"type", "string"},
                     {"default", ""},
                     {
                         "description",
                         agentxx::plugin::toolPromptArgDesc(
                             p,
                             "replace_str",
                             "Default: empty string. The replacement string used when `opt` is `replace`."
                         ),
                     },
                 },
             }},
         },
        {"required", neograph::json::array({"content", "exps", "opt"})}
    }.dump();
}

} // namespace

/// ---------------- 插件入口 / 销毁 ----------------

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
            static const AgentxxPluginInfo info{
                AGENTXX_PLUGIN_API_VERSION,
                0,
                agentxx::plugin::PluginStringView::fromCstr("agentxx_string"),
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "String tools: regex operations and html to markdown conversion"
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

            if (!ctx->iface.tools || !ctx->iface.tools->register_tool) {
                return -1;
            }

            // 1. agentxx_string_html_to_markdown (blocking_tool)
            agentxx::plugin::blocking_tool(
                *ctx,
                kNameHtml2Md,
                kDepictHtml2Md,
                schemaHtml2Md(ctx.get()),
                [](std::string_view args_json) -> std::string {
                    auto arguments = args_json.empty() ? neograph::json::object()
                                                       : neograph::json::parse(args_json);
                    return htmlToMarkdownExecute(arguments);
                }
            );

            // 2. agentxx_string_regexp (blocking_tool)
            agentxx::plugin::blocking_tool(
                *ctx,
                kNameRegexp,
                kDepictRegexp,
                schemaRegexp(ctx.get()),
                [](std::string_view args_json) -> std::string {
                    auto arguments = args_json.empty() ? neograph::json::object()
                                                       : neograph::json::parse(args_json);
                    return regexpExecute(arguments);
                }
            );

            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(ctxGuardLogger(ctx), [&] {
        if (ctx) {
            delete ctx;
        }
    });
}
