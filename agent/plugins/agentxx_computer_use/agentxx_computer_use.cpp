// agentxx_computer_use —— 计算机控制插件 (Windows)
#include "codegraph/core/json.hpp"
#include "computer_use_plugin.h"
#include "fmt/format.h"
#include <cstring>
#include <string>

namespace agentxx_computer_use_plugin {
std::string uiControlExecute(agentxx_computer_use_plugin::SimpleJson& arguments);

struct PluginCtx : public agentxx::kit::PluginBase {};

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
    codegraph::Json schema                   = codegraph::Json::object();
    schema["type"]                           = "object";
    schema["required"]                       = codegraph::Json::array({codegraph::Json("actions")});
    schema["properties"]                     = codegraph::Json::object();
    schema["properties"]["actions"]          = codegraph::Json::object();
    schema["properties"]["actions"]["type"]  = "array";
    schema["properties"]["actions"]["items"] = codegraph::Json::object();
    schema["properties"]["actions"]["items"]["type"] = "object";
    schema["properties"]["actions"]["items"]["required"]
        = codegraph::Json::array({codegraph::Json("action")});
    schema["properties"]["actions"]["items"]["properties"]              = codegraph::Json::object();
    schema["properties"]["actions"]["items"]["properties"]["action"]    = codegraph::Json({
        {"type", "string"},
        {"enum",
         codegraph::Json::array(
             {codegraph::Json("move_cursor"),
                 codegraph::Json("mouse_down"),
                 codegraph::Json("mouse_up"),
                 codegraph::Json("click"),
                 codegraph::Json("double_click"),
                 codegraph::Json("triple_click"),
                 codegraph::Json("middle_click"),
                 codegraph::Json("right_click"),
                 codegraph::Json("drag_and_drop"),
                 codegraph::Json("mouse_scroll"),
                 codegraph::Json("type_text"),
                 codegraph::Json("key_down"),
                 codegraph::Json("key_up"),
                 codegraph::Json("press_key"),
                 codegraph::Json("shortcut"),
                 codegraph::Json("wait")}
         )               }
    });
    schema["properties"]["actions"]["items"]["properties"]["x"]         = codegraph::Json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["y"]         = codegraph::Json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["button"]    = codegraph::Json({
        {"type", "string"},
        {"enum",
         codegraph::Json::array(
             {codegraph::Json("left"), codegraph::Json("right"), codegraph::Json("middle")}
         )               }
    });
    schema["properties"]["actions"]["items"]["properties"]["clicks"]    = codegraph::Json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["amount"]    = codegraph::Json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["direction"] = codegraph::Json({
        {"type", "string"                                                                },
        {"enum", codegraph::Json::array({codegraph::Json("up"), codegraph::Json("down")})}
    });
    schema["properties"]["actions"]["items"]["properties"]["text"]      = codegraph::Json({
        {"type", "string"}
    });
    schema["properties"]["actions"]["items"]["properties"]["key"]       = codegraph::Json({
        {"type", "string"}
    });
    schema["properties"]["actions"]["items"]["properties"]["keys"]      = codegraph::Json({
        {"type",  "array"                              },
        {"items", codegraph::Json({{"type", "string"}})}
    });
    schema["properties"]["actions"]["items"]["properties"]["delay_ms"]  = codegraph::Json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["duration_ms"] = codegraph::Json({
        {"type", "integer"}
    });
    return schema.dump();
}

} // namespace agentxx_computer_use_plugin

using namespace agentxx_computer_use_plugin;

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_PLUGIN_SV("agentxx_computer_use"),
        AGENTXX_PLUGIN_SV("1.0.0"),
        AGENTXX_PLUGIN_SV(
            "Computer control on Windows: mouse, keyboard, and scroll input (SendInput based)"
        ),
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
            raw = ctx.get();

            if (!ctx->iface.tools || !ctx->iface.tools->register_tool) {
                return -1;
            }

            auto        p      = ctx->toolPrompt("agentxx_ui_control_keyboard_mouse");
            std::string depict = p.depict.empty() ? kUiControlDefaultDepict : p.depict;

            agentxx::kit::blocking_tool(
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
    agentxx::plugin_guard::guardCallVoid(ctxGuardLogger(ctx), [&] {
        if (ctx) {
            delete ctx;
        }
    });
}
