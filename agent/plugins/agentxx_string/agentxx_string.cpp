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
    R"_(Search, replace, or remove text using regular expressions.
Operates on in-memory text content (not files).)_";

constexpr std::string_view kExpsDesc
    = R"_(Array of regex patterns. A match succeeds if ANY pattern matches.
Every pattern is applied and the result is the union of their matches (overlapping matches are merged, so one span is reported once).)_";
constexpr std::string_view kOptDesc = R"_(Operation mode:
`search`: Return all match results.
`replace`: Replace matches with `replace_str` and return the resulting text.
`remove`: Remove all matches and return the resulting text.)_";
constexpr std::string_view kReplaceStrDesc
    = "Default: empty string. The replacement string used when `opt` is `replace`.";
constexpr std::string_view kIgnoreCaseDesc
    = "Default `false`. If `true`, matching is case-insensitive (like `grep -i`).";

constexpr int32_t kAutoSummary = AGENTXX_PLUGIN_TOOL_FLAG_AUTO_SUMMARY;

/// 连续相同调用重复检查: 达阈值时由主程序询问用户确认后再继续
/// (见 [XXToolBase::repeatCallCheck] 与 AGENTXX_PLUGIN_TOOL_FLAG_REPEAT_CALL_CHECK)
constexpr int32_t kRepeatCheck = AGENTXX_PLUGIN_TOOL_FLAG_REPEAT_CALL_CHECK;

} // namespace

struct StringPluginCtx : public PluginBase {};

/// 注册事务 (mount 的实际内容; 每个 agent 各自一份)。
static pluginxx::Task<void> stringSetupAsync(StringPluginCtx& ctx, ScopeRef ref) {
    // 1. html_to_markdown
    auto html2mdSchema = ctx.schema(kNameHtml2Md)
                             .string("content", "The HTML string to convert.", /*required=*/true)
                             .build();

    co_await blocking_tool(
        ctx,
        ref,
        kNameHtml2Md,
        kDepictHtml2Md,
        html2mdSchema,
        [](std::string_view args_json, const PluginxxCancelToken* cancel_token) -> std::string {
            if (pluginxx_cancel_is_requested(cancel_token)) {
                throw agentxx::plugin::CanceledException("html2markdown Canceled");
            }
            ArgReader args(args_json);
            auto      out = htmlToMarkdownExecute(args.raw());
            if (pluginxx_cancel_is_requested(cancel_token)) {
                throw agentxx::plugin::CanceledException("html2markdown Canceled");
            }
            return out;
        },
        0,
        kAutoSummary | kRepeatCheck
    );

    // 2. regexp
    auto regexpSchema = ctx.schema(kNameRegexp)
                            .string("content", "The input text to operate on.", /*required=*/true)
                            .stringArray(
                                "exps",
                                kExpsDesc,
                                /*required=*/true
                            )
                            .enumString(
                                "opt",
                                kOptDesc,
                                {"search", "replace", "remove"},
                                /*required=*/true
                            )
                            .string("replace_str", kReplaceStrDesc, false, "")
                            .boolean("ignore_case", kIgnoreCaseDesc, false, false)
                            .build();

    co_await blocking_tool(
        ctx,
        ref,
        kNameRegexp,
        kDepictRegexp,
        regexpSchema,
        [](std::string_view args_json, const PluginxxCancelToken* cancel_token) -> std::string {
            if (pluginxx_cancel_is_requested(cancel_token)) {
                throw agentxx::plugin::CanceledException("regexp Canceled");
            }
            ArgReader args(args_json);
            auto      out = regexpExecute(args.raw());
            if (pluginxx_cancel_is_requested(cancel_token)) {
                throw agentxx::plugin::CanceledException("regexp Canceled");
            }
            return out;
        },
        0,
        kAutoSummary
    );

    co_return;
}

static void*
    stringStart(StringPluginCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    // 无进程级工作 (无连接/线程/定时器)。
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    (void)ctx;
    return nullptr;
}

static void* stringMount(
    StringPluginCtx&                   ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error
) {
    if (!notify) {
        if (error) {
            PluginString::set(ctx.host, error, "agentxx_string mount: notify required");
        }
        return nullptr;
    }
    auto later = finishLater(notify);
    if (!ctx.iface.tasks || !ctx.iface.tasks->register_task) {
        later.failed("agentxx_string mount: host has no pluginxx.tasks (spawn unavailable)");
        return later.provider();
    }
    const AgentRef agentRef(*spec);
    // 注册事务交给协程: 每个注册都是"提交 + co_await", 全部生效后再放行挂载
    ctx.spawn([later, agentRef](StringPluginCtx& c, OpCtl&) -> pluginxx::Task<void> {
        try {
            co_await stringSetupAsync(c, agentRef);
            later.ok();
        } catch (const std::exception& e) {
            later.failed(e.what());
        }
        co_return;
    });
    return later.provider();
}

static void* stringUnmount(
    StringPluginCtx&,
    void*,
    const PluginxxOperatorNotify* notify,
    PluginxxString*
) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* stringStop(StringPluginCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_EXPORT(
    StringPluginCtx,
    "agentxx_string",
    "1.0.0",
    "String tools: regex operations and html to markdown conversion",
    stringStart,
    stringStop,
    stringMount,
    stringUnmount
);
