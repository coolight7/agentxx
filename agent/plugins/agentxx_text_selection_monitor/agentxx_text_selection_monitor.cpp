/// agentxx_text_selection_monitor —— 系统级文本选择事件流插件
#include "fmt/format.h"
#include "text_selection_monitor.h"
#include "text_selection_monitor_plugin.h"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

using namespace agentxx_text_selection_monitor_plugin;
using namespace agentxx::plugin;

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

struct TextSelectionPluginCtx;

struct TextSelectionHolder {
    bool start(int debounceMs);
    void stop();

    agentxx_text_selection_monitor_plugin::TextSelectionMonitor monitor_;
    TextSelectionPluginCtx*                                     ctx = nullptr;
};

struct TextSelectionPluginCtx : public PluginBase {
    TextSelectionHolder holder;
    ~TextSelectionPluginCtx() override {
        holder.stop();
    }
};

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
                    ctx->jsonEscape(evt.text),
                    ctx->jsonEscape(sourceName(evt.source)),
                    tsMs
                );
                auto topicSv = PluginStringView::fromCstr(
                    "agentxx_text_selection_monitor.selection"
                );
                auto payloadSv
                    = PluginStringView::from(payload.data(), payload.size());
                ctx->iface.events->publish(ctx->host, &topicSv, &payloadSv);
            } catch (...) {
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

AGENTXX_PLUGIN_AGENT_EXPORT(
    TextSelectionPluginCtx,
    "agentxx_text_selection_monitor",
    "1.0.0",
    "System-wide text selection monitor event stream",
    [](TextSelectionPluginCtx& ctx) -> int32_t {
        ctx.holder.ctx = &ctx;

        auto schema = ctx.schema("agentxx_text_selection_monitor")
            .enumString("command", "Operation command: start listening, stop listening, or query running status.",
                        {"start", "stop", "status"}, /*required=*/true)
            .integer("debounce_ms", "Debounce interval in milliseconds (default: 150). Only applies to start command.")
            .build();

        blocking_tool(
            ctx,
            "agentxx_text_selection_monitor",
            "Monitor system-wide text selection events. Supports start, stop, and status query.",
            schema,
            [](TextSelectionPluginCtx& c, std::string_view args_json) -> std::string {
                ArgReader args(args_json);
                auto command = args.require<std::string>("command");
                if (!args.ok()) return args.errorMessage();

                TextSelectionHolder& holder = c.holder;

                if (command == "start") {
                    int64_t debounceMs = args.value("debounce_ms", int64_t{0});
                    bool ok = holder.start(static_cast<int>(debounceMs));
                    return fmt::format(R"({{"ok":{},"running":true}})", ok ? "true" : "false");
                }

                if (command == "stop") {
                    holder.stop();
                    return R"({"ok":true,"running":false})";
                }

                if (command == "status") {
                    bool running = holder.monitor_.isRunning();
                    return fmt::format(R"({{"ok":true,"running":{}}})", running ? "true" : "false");
                }

                return R"({"ok":false,"error":"unknown command"})";
            }
        );

        return 0;
    }
);
