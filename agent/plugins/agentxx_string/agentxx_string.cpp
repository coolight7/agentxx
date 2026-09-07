/// agentxx_string —— 字符串处理工具插件
#include "agentxx_string_plugin.h"
#include "string_impl.h"
#include <string>

using namespace agentxx_string_plugin;
using namespace agentxx::plugin;

namespace {

constexpr std::string_view kNameHtml2Md = "agentxx_string_html_to_markdown";
constexpr std::string_view kNameRegexp  = "agentxx_string_regexp";

constexpr std::string_view kDepictHtml2Md = "Convert HTML content to Markdown format.";
constexpr std::string_view kDepictRegexp =
    R"(Search, replace, or remove text using regular expressions.
Operates on in-memory text content (not files).)";

} // namespace

struct StringPluginCtx : public PluginBase {};

AGENTXX_PLUGIN_AGENT_EXPORT(
    StringPluginCtx,
    "agentxx_string",
    "1.0.0",
    "String tools: regex operations and html to markdown conversion",
    [](StringPluginCtx& ctx) -> int32_t {
        // 1. html_to_markdown
        auto html2mdSchema = ctx.schema(kNameHtml2Md)
            .string("content", "The HTML string to convert.", /*required=*/true)
            .build();

        blocking_tool(
            ctx,
            kNameHtml2Md,
            kDepictHtml2Md,
            html2mdSchema,
            [](std::string_view args_json) -> std::string {
                ArgReader args(args_json);
                return htmlToMarkdownExecute(args.raw());
            }
        );

        // 2. regexp
        auto regexpSchema = ctx.schema(kNameRegexp)
            .string("content", "The input text to operate on.", /*required=*/true)
            .stringArray("exps", "Array of regex patterns. A match succeeds if ANY pattern matches.", /*required=*/true)
            .enumString("opt", R"(Operation mode:
`search`: Return all match results.
`replace`: Replace matches with `replace_str` and return the resulting text.
`remove`: Remove all matches and return the resulting text.)",
                        {"search", "replace", "remove"}, /*required=*/true)
            .string("replace_str", "Default: empty string. The replacement string used when `opt` is `replace`.", false, "")
            .build();

        blocking_tool(
            ctx,
            kNameRegexp,
            kDepictRegexp,
            regexpSchema,
            [](std::string_view args_json) -> std::string {
                ArgReader args(args_json);
                return regexpExecute(args.raw());
            }
        );

        return 0;
    }
);
