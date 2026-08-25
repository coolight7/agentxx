// agentxx_string —— 字符串处理工具插件
// - 从 libagentxx src/tools/string 拆分独立 (同名同行为):
//   - agentxx_string_html_to_markdown (原 StringHtml2MarkdownTool)
//   - agentxx_string_regexp           (原 StringRegexpTool)
// - 业务逻辑在 string_impl.h (纯函数, 测试直测同一实现)
// - 工具 schema 与描述经宿主 toolPrompt 生成 (lib AgentPrompt 保留条目,
//   用户 yaml 覆盖继续生效; 缺失时回退下方内置默认文本)
#include "agentxx_string_plugin.h"
#include "string_impl.h"
#include <cstring>
#include <string>

using namespace agentxx_string_plugin;

namespace {

constexpr auto kNameHtml2Md = "agentxx_string_html_to_markdown";
constexpr auto kNameRegexp  = "agentxx_string_regexp";

/// 宿主 toolPrompt 缺失时的兜底 depict (完整默认文本保留在 lib AgentPrompt)
constexpr auto kDepictHtml2Md = "Convert HTML content to Markdown format.";
constexpr auto kDepictRegexp =
    R"(Search, replace, or remove text using regular expressions.
Operates on in-memory text content (not files).)";

/// 参数说明兜底 (同上, 正常情况下由宿主 toolPrompt 提供完整文案)
std::string argDesc(const ToolPromptText& p, const char* key, const char* fallback) {
    auto it = p.args.find(key);
    if (it != p.args.end() && !it->second.empty()) {
        return it->second;
    }
    return fallback;
}

/// 注册常规工具 (schema/描述存储于本实例 ctx->storage; spec 字符串字段以
/// string_view 传入, 宿主注册时拷贝); execute 由调用方提供 (静态函数, 无捕获)
/// - 统一异步操作模型: 经阻塞委托垫片注册 (offload 池线程执行, io 线程
///   只等完成通知); execute 签名追加 cancel_flag 形参 (本插件忽略)
void registerTool(
    PluginCtx*         ctx,
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
    auto&       storage = ctx->storage;
    std::string depict  = readToolPrompt(ctx->host, ctx->iface, name).depict;
    if (depict.empty()) {
        depict = defaultDepict;
    }
    storage.push_back(std::move(depict));
    storage.push_back(schema);

    // 垫片适配器: 实例内嵌存储 (ctx 持有, 随实例销毁释放; 多实例契约)
    auto shim = std::make_unique<AgentxxSyncToolShim>();

    AgentxxSyncToolSpec spec{};
    spec.name        = agentxx_plugin_sv(name, std::strlen(name));
    spec.description = agentxx_plugin_sv(
        storage[storage.size() - 2].data(),
        storage[storage.size() - 2].size()
    );
    spec.parameters_json = agentxx_plugin_sv(storage.back().data(), storage.back().size());
    spec.user_data       = ctx; ///< 回调经 user_data 恢复本实例上下文
    spec.flags           = flags;
    spec.execute         = execute;
    // 注意: 本文件先于本注释包含了 string_impl.h (其引入 util/log.h 重定义
    // XX_LOG* 宏为库版签名), 故此处直接调用 pluginLog, 不经 XX_LOGW 宏
    if (agentxx_register_sync_tool(ctx->host, &spec, shim.get()) != 0) {
        pluginLog(ctx, 3, fmt::format("agentxx_string: register tool {} failed", name));
        return;
    }
    ctx->sync_tool_shims.push_back(std::move(shim));
}

/// C ABI execute 包装: 解析参数 JSON → 调用实现 → 结果 strdup (异常不外泄);
/// user_data 即本实例 PluginCtx (多实例安全: 结果分配走本实例宿主堆)
template<auto ExecFn>
char* wrapExecute(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView thread_id,
    AgentxxPluginStringView tool_call_id,
    volatile int*           cancel_flag,
    char**                  error_out
) {
    (void)thread_id;
    (void)tool_call_id;
    (void)cancel_flag;
    auto* ctx = static_cast<PluginCtx*>(user_data);
    try {
        std::string argsStr(args_json.data ? args_json.data : "", args_json.size);
        auto        arguments = argsStr.empty() ? neograph::json::object()
                                                : neograph::json::parse(argsStr);
        auto result = ExecFn(arguments);
        return pluginStrdup(ctx ? ctx->host : nullptr, result.c_str());
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = pluginStrdup(ctx ? ctx->host : nullptr, ex.what());
        }
        return nullptr;
    } catch (...) {
        if (error_out) {
            *error_out = pluginStrdup(ctx ? ctx->host : nullptr, "unknown exception");
        }
        return nullptr;
    }
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
            AGENTXX_SV("agentxx_string"),
            AGENTXX_SV("1.0.0"),
            AGENTXX_SV("String tools: html to markdown conversion and regexp search/replace/remove"),
        };
        return &info;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT int agentxx_plugin_create(const AgentxxHost* host, void** plugin_ctx) {
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
        // 每实例上下文 (多实例安全): 成功经 *plugin_ctx 交付宿主, destroy 释放
        auto ctx   = std::make_unique<PluginCtx>();
        ctx->host  = host;
        ctx->iface = agentxx::plugin::AgentIfaces::query(host);
        raw        = ctx.get();

        // ---- agentxx_string_html_to_markdown ----
        {
            ToolPromptText p   = readToolPrompt(ctx->host, ctx->iface, kNameHtml2Md);
            std::string schema = neograph::json{
                {"type", "object"},
                {"properties",
                 {
                     {"content",
                      {
                          {"type", "string"},
                          {"description", argDesc(p, "content", "The HTML string to convert.")},
                      }},
                 }},
                {"required", neograph::json::array({"content"})},
            }
                                  .dump();
            registerTool(
                ctx.get(),
                kNameHtml2Md,
                kDepictHtml2Md,
                schema,
                &wrapExecute<agentxx_string_plugin::htmlToMarkdownExecute>,
                AGENTXX_TOOL_FLAG_AUTO_SUMMARY
            );
        }

        // ---- agentxx_string_regexp ----
        {
            ToolPromptText p   = readToolPrompt(ctx->host, ctx->iface, kNameRegexp);
            std::string schema = neograph::json{
                {"type", "object"},
                {"properties",
                 {
                     {"content",
                      {
                          {"type", "string"},
                          {"description", argDesc(p, "content", "The input text to operate on.")},
                      }},
                     {"exps",
                      {
                          {"type", "array"},
                          {"items", neograph::json{{"type", "string"}}},
                          {"description",
                           argDesc(p,
                                   "exps",
                                   "Array of regex patterns. A match succeeds if ANY pattern matches.")},
                      }},
                     {"opt",
                      {
                          {"type", "string"},
                          {"enum", neograph::json::array({"search", "replace", "remove"})},
                          {"description",
                           argDesc(p,
                                   "opt",
                                   "Operation mode:\n`search`: Return all match results.\n`replace`: Replace matches with `replace_str` and return the resulting text.\n`remove`: Remove all matches and return the resulting text.")},
                      }},
                     {"replace_str",
                      {
                          {"type", "string"},
                          {"description",
                           argDesc(p,
                                   "replace_str",
                                   "Default: empty string. The replacement string used when `opt` is `replace`.")},
                      }},
                 }},
                {"required", neograph::json::array({"content", "exps", "opt"})},
            }
                                  .dump();
            registerTool(
                ctx.get(),
                kNameRegexp,
                kDepictRegexp,
                schema,
                &wrapExecute<agentxx_string_plugin::regexpExecute>,
                AGENTXX_TOOL_FLAG_AUTO_SUMMARY
            );
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
