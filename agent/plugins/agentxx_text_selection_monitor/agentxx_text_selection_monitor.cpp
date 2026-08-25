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
const char* sourceName(agentxx::expand::TextSource s) {
    switch (s) {
        case agentxx::expand::TextSource::Unknown:
            return "unknown";
        case agentxx::expand::TextSource::TextPattern:
            return "text_pattern";
        case agentxx::expand::TextSource::TextChildPattern:
            return "text_child_pattern";
        case agentxx::expand::TextSource::ValuePattern:
            return "value_pattern";
        case agentxx::expand::TextSource::EmGetSel:
            return "em_get_sel";
        case agentxx::expand::TextSource::AccessibleObject:
            return "accessible_object";
        case agentxx::expand::TextSource::WmGetText:
            return "wm_get_text";
        case agentxx::expand::TextSource::DevTools:
            return "devtools";
        case agentxx::expand::TextSource::FlutterAccessibility:
            return "flutter_accessibility";
        case agentxx::expand::TextSource::Clipboard:
            return "clipboard";
    }
    return "unknown";
}

/// 转义字符串为 JSON 字面量 (经宿主 vtable json_escape)
std::string jsonEscape(const std::string& s) {
    if (!g_host || !g_if.json || !g_if.json->json_escape || s.empty()) {
        return "\"\"";
    }
    char* esc = g_if.json->json_escape(g_host, agentxx_plugin_sv(s.data(), s.size()));
    if (!esc) {
        return "\"\"";
    }
    std::string out{esc};
    g_host->vtable->free(esc);
    return out;
}

/// 文本选择监听单例 (TextSelectionMonitor 内部自管线程; unload 时停止)
struct TextSelectionHolder {
    static TextSelectionHolder& instance() {
        static TextSelectionHolder holder;
        return holder;
    }

    bool start(int debounceMs) {
        if (monitor_.isRunning()) {
            return false;
        }
        if (debounceMs > 0) {
            monitor_.setDebounceMs(debounceMs);
        }
        monitor_.addListener([](const agentxx::expand::TextSelectionEvent& evt) {
            if (!g_host || !g_if.events || !g_if.events->publish) {
                return;
            }
            auto tsMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            evt.timestamp.time_since_epoch()
            )
                            .count();
            std::string payload = fmt::format(
                R"({{"text":{},"source":{},"timestamp_ms":{}}})",
                jsonEscape(evt.text),
                jsonEscape(sourceName(evt.source)),
                tsMs
            );
            g_if.events->publish(
                g_host,
                AGENTXX_SV("agentxx_text_selection_monitor.selection"),
                agentxx_plugin_sv(payload.data(), payload.size())
            );
        });
        return monitor_.start();
    }

    void stop() {
        monitor_.stop();
        monitor_.removeAllListeners();
    }

    agentxx::expand::TextSelectionMonitor monitor_;
};

/// 工具执行: command = start|stop|status (阻塞委托型; offload 池线程调用)
char* textSelectionExecute(
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
        std::string argsStr{args_json.data ? args_json.data : "{}", args_json.size};
        SimpleJson  args(argsStr.empty() ? "{}" : argsStr);
        if (!args.ok()) {
            throw std::runtime_error("invalid args json");
        }

        std::string command;
        jsonGetString(args.doc().at_pointer("/command"), command);

        auto& holder = TextSelectionHolder::instance();

        if (command == "start") {
            int64_t debounceMs = 0;
            jsonGetInt(args.doc().at_pointer("/debounce_ms"), debounceMs);
            bool ok = holder.start(static_cast<int>(debounceMs));
            return pluginStrdup(
                fmt::format(R"({{"ok":{},"running":true}})", ok ? "true" : "false").c_str()
            );
        }

        if (command == "stop") {
            holder.stop();
            return pluginStrdup(R"({"ok":true,"running":false})");
        }

        if (command == "status") {
            bool running = holder.monitor_.isRunning();
            return pluginStrdup(
                fmt::format(R"({{"ok":true,"running":{}}})", running ? "true" : "false").c_str()
            );
        }

        return pluginStrdup(R"({"ok":false,"error":"unknown command"})");
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

// =====================================================================
// 插件入口 (C ABI)
// =====================================================================

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("agentxx_text_selection_monitor"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("System-wide text selection event stream: start/stop/status "
                   "(Windows; other platforms no-op)"),
    };
    return &info;
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_entry(const AgentxxHost* host, void** /*plugin_ctx*/) {
    g_host = host;

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
    spec.execute         = textSelectionExecute;
    if (agentxx_register_sync_tool(host, &spec) != 0) {
        pluginLog(3, "agentxx_text_selection_monitor: register tool failed");
        return -1;
    }

    pluginLog(2, "agentxx_text_selection_monitor loaded (1 tool)");
    return 0;
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_unload(void* /*plugin_ctx*/) {
    TextSelectionHolder::instance().stop();
    if (g_host && g_host->vtable) {
        if (g_if.tools && g_if.tools->unregister_tool)
            g_if.tools->unregister_tool(g_host, AGENTXX_SV("agentxx_text_selection_monitor"));
    }
    pluginLog(2, "agentxx_text_selection_monitor unloaded");
}
