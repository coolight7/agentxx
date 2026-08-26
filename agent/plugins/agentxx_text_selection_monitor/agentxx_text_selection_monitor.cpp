// agentxx_text_selection_monitor —— 系统级文本选择事件流插件
// - 从 libagentxx src/expand/text_selection_monitor 拆分独立:
//   TextSelectionMonitor (Windows UIAutomation + WinEvent 钩子 + CDP/剪贴板兜底)
//   迁移为本插件内部实现
// - 注册工具: agentxx_text_selection_monitor (start/stop/status/set_debounce)
// - 捕获到的选中文本经 publish 事件推送 (topic
//   "agentxx_text_selection_monitor.selection", JSON: text/source/timestamp_ms)
// - 非 Windows 平台为 no-op (start 返回失败), 工具仍可查询状态
#include "fmt/format.h"
#include "text_selection_monitor.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "text_selection_monitor_plugin.h"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

using namespace agentxx_text_selection_monitor_plugin;

namespace {

/// 文本来源枚举 → 字符串
const char* sourceName(agentxx_text_selection_monitor_plugin::TextSource s) {
    switch (s) {
        case agentxx_text_selection_monitor_plugin::TextSource::Unknown:
            return "unknown";
        case agentxx_text_selection_monitor_plugin::TextSource::TextPattern:
            return "text_pattern";
        case agentxx_text_selection_monitor_plugin::TextSource::TextChildPattern:
            return "text_child_pattern";
        case agentxx_text_selection_monitor_plugin::TextSource::ValuePattern:
            return "value_pattern";
        case agentxx_text_selection_monitor_plugin::TextSource::EmGetSel:
            return "em_get_sel";
        case agentxx_text_selection_monitor_plugin::TextSource::AccessibleObject:
            return "accessible_object";
        case agentxx_text_selection_monitor_plugin::TextSource::WmGetText:
            return "wm_get_text";
        case agentxx_text_selection_monitor_plugin::TextSource::DevTools:
            return "devtools";
        case agentxx_text_selection_monitor_plugin::TextSource::FlutterAccessibility:
            return "flutter_accessibility";
        case agentxx_text_selection_monitor_plugin::TextSource::Clipboard:
            return "clipboard";
    }
    return "unknown";
}

struct PluginCtx;


/// 每实例上下文 (多实例契约: 零可变全局; 前置声明见下)
struct PluginCtx;

/// 文本选择监听 (TextSelectionMonitor 内部自管线程; 原函数级 static 单例
/// 在多实例下会把首实例的事件发到首实例宿主 —— 已修: monitor 移入每实例
/// PluginCtx, 监听回调捕获本实例裸指针; 线程存活期 ⊆ 实例存活期)
struct TextSelectionHolder {
    bool start(int debounceMs); ///< 定义于 PluginCtx 完整声明后
    void stop();

    agentxx_text_selection_monitor_plugin::TextSelectionMonitor monitor_;
    PluginCtx*                                                  ctx = nullptr; ///< 归属实例
};

/// 每实例上下文 (完整定义 — Holder 之后以避免循环依赖)
struct PluginCtx {
    const AgentxxHost*           host  = nullptr;
    agentxx::plugin::AgentIfaces iface {};
    TextSelectionHolder          holder; ///< 监听器 (随实例生死)
    AgentxxSyncToolShim          shim {};///< 垫片适配器 (随实例生死)
    /// XX_LOG* 宏路由 Sink 闭包存储 (create 时装配并发布到 g_log_sink;
    /// destroy 时若全局指针仍指向此处则清除 —— 见 text_selection_monitor_plugin.h 注释)
    agentxx_text_selection_monitor_plugin::PluginLogSink log_sink;
};

/// 转义字符串为 JSON 字面量 (经宿主 vtable json_escape; host 取自本实例 ctx)
std::string jsonEscape(const PluginCtx& ctx, const std::string& s) {
    if (!ctx.host || !ctx.iface.json || !ctx.iface.json->json_escape || s.empty()) {
        return "\"\"";
    }
    char* esc = ctx.iface.json->json_escape(ctx.host, agentxx_plugin_sv(s.data(), s.size()));
    if (!esc) {
        return "\"\"";
    }
    std::string out{esc};
    ctx.host->vtable->free(esc);
    return out;
}

bool TextSelectionHolder::start(int debounceMs) {
    if (monitor_.isRunning()) {
        return false;
    }
    if (debounceMs > 0) {
        monitor_.setDebounceMs(debounceMs);
    }
        // 异常守卫: 监听回调运行在监视线程, 异常逃逸会 terminate 进程
        monitor_.addListener([ctx = this->ctx](const agentxx_text_selection_monitor_plugin::TextSelectionEvent& evt) {
            try {
                if (!ctx || !ctx->host || !ctx->iface.events || !ctx->iface.events->publish) {
                    return;
                }
                auto tsMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                evt.timestamp.time_since_epoch()
                )
                                .count();
                std::string payload = fmt::format(
                    R"({{"text":{},"source":{},"timestamp_ms":{}}})",
                    jsonEscape(*ctx, evt.text),
                    jsonEscape(*ctx, sourceName(evt.source)),
                    tsMs
                );
                ctx->iface.events->publish(
                    ctx->host,
                    AGENTXX_SV("agentxx_text_selection_monitor.selection"),
                    agentxx_plugin_sv(payload.data(), payload.size())
                );
            } catch (...) {
                pluginLog(ctx ? ctx->host : nullptr,
                          ctx ? ctx->iface.log : nullptr,
                          4,
                          "selection event publish");
            }
        });
        return monitor_.start();
    }


void TextSelectionHolder::stop() {
    monitor_.stop();
    monitor_.removeAllListeners();
}

/// 工具执行: command = start|stop|status (阻塞委托型; offload 池线程调用)
char* textSelectionExecute(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView thread_id,
    AgentxxPluginStringView tool_call_id,
    volatile int*           cancel_flag,
    char**                  error_out
) {
    auto* ctx    = static_cast<PluginCtx*>(user_data);
    auto* host   = ctx ? ctx->host : nullptr; ///< 多实例契约: 结果串走本实例宿主堆
    (void)thread_id;
    (void)tool_call_id;
    (void)cancel_flag;
    try {
        std::string argsStr{args_json.data ? args_json.data : "{}", args_json.size};
        SimpleJson  args(argsStr.empty() ? "{}" : argsStr);
        if (!args.ok()) {
            throw std::runtime_error("invalid args json");
        }

        std::string command;
        jsonGetString(args.doc().at_pointer("/command"), command);

        TextSelectionHolder& holder = ctx->holder;

        if (command == "start") {
            int64_t debounceMs = 0;
            jsonGetInt(args.doc().at_pointer("/debounce_ms"), debounceMs);
            bool ok = holder.start(static_cast<int>(debounceMs));
            return pluginStrdup(host,
                fmt::format(R"({{"ok":{},"running":true}})", ok ? "true" : "false").c_str()
            );
        }

        if (command == "stop") {
            holder.stop();
            return pluginStrdup(host, R"({"ok":true,"running":false})");
        }

        if (command == "status") {
            bool running = holder.monitor_.isRunning();
            return pluginStrdup(
                host, fmt::format(R"({{"ok":true,"running":{}}})", running ? "true" : "false").c_str()
            );
        }

        return pluginStrdup(host, R"({"ok":false,"error":"unknown command"})");
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = pluginStrdup(host, ex.what());
        }
        return nullptr;
    } catch (...) {
        if (error_out) {
            *error_out = pluginStrdup(host, "unknown exception");
        }
        return nullptr;
    }
}

} // namespace

// =====================================================================
// 插件入口 (C ABI)
// =====================================================================

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL; 本边界为纯静态元数据 → 空操作日志
    return agentxx::plugin_guard::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
        static const AgentxxPluginInfo info{
            AGENTXX_PLUGIN_API_VERSION,
            AGENTXX_SV("agentxx_text_selection_monitor"),
            AGENTXX_SV("1.0.0"),
            AGENTXX_SV("System-wide text selection event stream: start/stop/status "
                       "(Windows; other platforms no-op)"),
        };
        return &info;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_create(const AgentxxHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: 异常返回 -1 (创建失败); 日志闭包捕获局部裸指针
    auto ctx   = std::make_unique<PluginCtx>();
    PluginCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
        [&raw](const char* m) noexcept {
            pluginLog(raw ? raw->host : nullptr, raw ? raw->iface.log : nullptr, 4, m ? m : "");
        },
        -1,
        [&]() -> int {
        if (!host || !host->vtable || !plugin_ctx) {
            return -1;
        }
        ctx->host  = host;
        ctx->iface = agentxx::plugin::AgentIfaces::query(host);
        raw        = ctx.get();
        // 装配 XX_LOG* 宏路由 Sink (text_selection_monitor.cpp 经 g_log_sink 转发宿主日志)
        ctx->log_sink = agentxx_text_selection_monitor_plugin::PluginLogSink(
            [host, logIf = ctx->iface.log](int level, const std::string& msg) {
                agentxx_text_selection_monitor_plugin::pluginLog(host, logIf, level, msg);
            }
        );
        agentxx_text_selection_monitor_plugin::g_log_sink.store(&ctx->log_sink,
                                                                std::memory_order_release);
        ctx->holder.ctx = ctx.get(); ///< 监听回调经此读本实例宿主

        static const std::string kSchema = R"({
        "type": "object",
        "properties": {
            "command": {"type": "string", "enum": ["start", "stop", "status"]},
            "debounce_ms": {"type": "integer", "description": "Debounce interval for start, default 300"}
        },
        "required": ["command"]
    })";

        AgentxxSyncToolSpec spec{};
        spec.name = AGENTXX_SV("agentxx_text_selection_monitor");
        spec.description
            = AGENTXX_SV("Monitor text selections system-wide on Windows. start begins capturing; "
                         "selected text is published as plugin events "
                         "(agentxx_text_selection_monitor.selection).");
        spec.parameters_json = agentxx_plugin_sv(kSchema.data(), kSchema.size());
        spec.user_data       = ctx.get();
        spec.execute         = textSelectionExecute;
        if (agentxx_register_sync_tool(host, &spec, &ctx->shim) != 0) {
            pluginLog(ctx->host, ctx->iface.log, 3,
                      "agentxx_text_selection_monitor: register tool failed");
            return -1;
        }

        pluginLog(ctx->host, ctx->iface.log, 2, "agentxx_text_selection_monitor loaded (1 tool)");
        *plugin_ctx = ctx.release(); ///< 所有权移交宿主 (destroy 时取回归还)
        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    // C ABI 边界异常守卫: 销毁回调异常不得外泄
    agentxx::plugin_guard::guardCallVoid(
        [ctx](const char* m) noexcept {
            pluginLog(ctx ? ctx->host : nullptr, ctx ? ctx->iface.log : nullptr, 4, m ? m : "");
        },
        [&] {
        if (!ctx) {
            return;
        }
        ctx->holder.stop(); ///< 先停监视线程 (回调捕获 ctx, 必须先于 delete)
        if (ctx->host && ctx->iface.tools && ctx->iface.tools->unregister_tool)
            ctx->iface.tools->unregister_tool(ctx->host,
                                              AGENTXX_SV("agentxx_text_selection_monitor"));
        pluginLog(ctx->host, ctx->iface.log, 2, "agentxx_text_selection_monitor unloaded");
        // 全局宏路由指针仍指向本实例 Sink 时清除 (多实例下可能已指向后装配者)
        const agentxx_text_selection_monitor_plugin::PluginLogSink* expected
            = &ctx->log_sink;
        agentxx_text_selection_monitor_plugin::g_log_sink.compare_exchange_strong(
            expected,
            nullptr
        );
        delete ctx;
        });
}
