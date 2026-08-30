// agentxx_text_selection_monitor —— 系统级文本选择事件流插件
#include "fmt/format.h"
#include "text_selection_monitor.h"
#include "text_selection_monitor_plugin.h"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

using namespace agentxx_text_selection_monitor_plugin;

namespace {

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

struct TextSelectionHolder {
    bool start(int debounceMs);
    void stop();

    agentxx_text_selection_monitor_plugin::TextSelectionMonitor monitor_;
    PluginCtx*                                                  ctx = nullptr;
};

struct PluginCtx : public agentxx::kit::PluginBase {
    TextSelectionHolder                                  holder;
    agentxx_text_selection_monitor_plugin::PluginLogSink log_sink;
};

static auto ctxGuardLogger(PluginCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx) {
            ctx->log.error(msg ? msg : "");
        }
    };
}

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
    monitor_.addListener(
        [ctx = this->ctx](const agentxx_text_selection_monitor_plugin::TextSelectionEvent& evt) {
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
                    agentxx_plugin_sv_cstr("agentxx_text_selection_monitor.selection"),
                    agentxx_plugin_sv(payload.data(), payload.size())
                );
            } catch (...) {
                if (ctx) {
                    ctx->log.error("selection event publish failed");
                }
            }
        }
    );
    return monitor_.start();
}

void TextSelectionHolder::stop() {
    monitor_.stop();
    monitor_.removeAllListeners();
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        agentxx_plugin_sv_cstr("agentxx_text_selection_monitor"),
        agentxx_plugin_sv_cstr("1.0.0"),
        agentxx_plugin_sv_cstr("System-wide text selection monitor event stream"),
    };
    return &info;
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    PluginCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
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
            ctx->holder.ctx = ctx.get();
            raw             = ctx.get();

            ctx->log_sink = [raw = ctx.get()](int level, const std::string& msg) {
                if (raw) {
                    raw->log.log(level, msg);
                }
            };
            agentxx_text_selection_monitor_plugin::g_log_sink.store(
                &ctx->log_sink,
                std::memory_order_release
            );

            if (!ctx->iface.tools || !ctx->iface.tools->register_tool) {
                return -1;
            }

            agentxx::kit::blocking_tool(
                *ctx,
                "agentxx_text_selection_monitor",
                "Monitor system-wide text selection events. Supports start, stop, and status query.",
                R"({
  "type": "object",
  "properties": {
    "command": {
      "type": "string",
      "enum": ["start", "stop", "status"],
      "description": "Operation command: start listening, stop listening, or query running status."
    },
    "debounce_ms": {
      "type": "integer",
      "description": "Debounce interval in milliseconds (default: 150). Only applies to start command."
    }
  },
  "required": ["command"]
})",
                [](PluginCtx& c, std::string_view args_json) -> std::string {
                    std::string argsStr(
                        args_json.data() ? args_json.data() : "{}",
                        args_json.size()
                    );
                    SimpleJson args(argsStr.empty() ? "{}" : argsStr);
                    if (!args.ok()) {
                        throw std::runtime_error("invalid args json");
                    }

                    std::string command;
                    jsonGetString(args.doc().at_pointer("/command"), command);

                    TextSelectionHolder& holder = c.holder;

                    if (command == "start") {
                        int64_t debounceMs = 0;
                        jsonGetInt(args.doc().at_pointer("/debounce_ms"), debounceMs);
                        bool ok = holder.start(static_cast<int>(debounceMs));
                        return fmt::format(R"({{"ok":{},"running":true}})", ok ? "true" : "false");
                    }

                    if (command == "stop") {
                        holder.stop();
                        return R"({"ok":true,"running":false})";
                    }

                    if (command == "status") {
                        bool running = holder.monitor_.isRunning();
                        return fmt::format(
                            R"({{"ok":true,"running":{}}})",
                            running ? "true" : "false"
                        );
                    }

                    return R"({"ok":false,"error":"unknown command"})";
                }
            );

            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(ctxGuardLogger(ctx), [&] {
        if (ctx) {
            ctx->holder.stop();
            const auto* exp = &ctx->log_sink;
            agentxx_text_selection_monitor_plugin::g_log_sink.compare_exchange_strong(exp, nullptr);
            delete ctx;
        }
    });
}
