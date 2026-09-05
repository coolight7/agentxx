/// agentxx_computer_use —— 计算机控制插件 (Windows)
#include "computer_use_plugin.h"
#include "fmt/format.h"
#include <cstring>
#include <neograph/json.h>
#include <string>

namespace agentxx_computer_use_plugin {
std::string uiControlExecute(agentxx_computer_use_plugin::SimpleJson& arguments);

struct PluginCtx : public agentxx::plugin::PluginBase {};

static auto ctxGuardLogger(PluginCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx) {
            ctx->log.error(msg ? msg : "");
        }
    };
}

static const char* kUiControlDefaultDepict
    = "Send mouse, keyboard, and scroll commands in a single batch (Windows). "
      "Commands are executed sequentially in order with optional delays. "
      "Coordinate system: absolute desktop coordinates across all monitors (x/y in pixels); "
      "screen (0,0) is primary monitor top-left. "
      "Text input supports full Unicode, newlines (\\n), emojis, and multiline text. "
      "Shortcut execution automatically presses modifier keys, presses target key, and releases in reverse order.";

static std::string makeUiControlSchema() {
    neograph::json schema                                  = neograph::json::object();
    schema["type"]                                         = "object";
    schema["required"]                                     = neograph::json::array({"actions"});
    schema["properties"]                                   = neograph::json::object();
    schema["properties"]["actions"]                        = neograph::json::object();
    schema["properties"]["actions"]["type"]                = "array";
    schema["properties"]["actions"]["items"]               = neograph::json::object();
    schema["properties"]["actions"]["items"]["type"]       = "object";
    schema["properties"]["actions"]["items"]["required"]   = neograph::json::array({"action"});
    schema["properties"]["actions"]["items"]["properties"] = neograph::json::object();
    schema["properties"]["actions"]["items"]["properties"]["action"]      = neograph::json({
        {"type", "string"},
        {"enum",
         neograph::json::array(
             {"move_cursor",
                   "mouse_down",
                   "mouse_up",
                   "click",
                   "double_click",
                   "triple_click",
                   "middle_click",
                   "right_click",
                   "drag_and_drop",
                   "mouse_scroll",
                   "type_text",
                   "key_down",
                   "key_up",
                   "press_key",
                   "shortcut",
                   "wait"}
         )               }
    });
    schema["properties"]["actions"]["items"]["properties"]["x"]           = neograph::json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["y"]           = neograph::json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["button"]      = neograph::json({
        {"type", "string"                                          },
        {"enum", neograph::json::array({"left", "right", "middle"})}
    });
    schema["properties"]["actions"]["items"]["properties"]["clicks"]      = neograph::json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["amount"]      = neograph::json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["direction"]   = neograph::json({
        {"type", "string"                             },
        {"enum", neograph::json::array({"up", "down"})}
    });
    schema["properties"]["actions"]["items"]["properties"]["text"]        = neograph::json({
        {"type", "string"}
    });
    schema["properties"]["actions"]["items"]["properties"]["key"]         = neograph::json({
        {"type", "string"}
    });
    schema["properties"]["actions"]["items"]["properties"]["keys"]        = neograph::json({
        {"type",  "array"                             },
        {"items", neograph::json({{"type", "string"}})}
    });
    schema["properties"]["actions"]["items"]["properties"]["delay_ms"]    = neograph::json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["duration_ms"] = neograph::json({
        {"type", "integer"}
    });
    return schema.dump();
}

} // namespace agentxx_computer_use_plugin

using namespace agentxx_computer_use_plugin;

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        0,
        agentxx::plugin::PluginStringView::fromCstr("agentxx_computer_use"),
        agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
        agentxx::plugin::PluginStringView::fromCstr(
            "Computer control on Windows: mouse, keyboard, and scroll input (SendInput based)"
        ),
    };
    return &info;
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

            auto        p      = ctx->toolPrompt("agentxx_ui_control_keyboard_mouse");
            std::string depict = p.depict.empty() ? kUiControlDefaultDepict : p.depict;

            agentxx::plugin::blocking_tool(
                *ctx,
                "agentxx_ui_control_keyboard_mouse",
                depict,
                makeUiControlSchema(),
                [](PluginCtx&, std::string_view args_json) -> std::string {
                    std::string argsStr(
                        args_json.data() ? args_json.data() : "{}",
                        args_json.size()
                    );
                    SimpleJson args(argsStr.empty() ? "{}" : argsStr);
                    if (!args.ok()) {
                        throw std::runtime_error("invalid args json");
                    }
                    return uiControlExecute(args);
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
