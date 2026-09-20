/// agentxx_computer_use —— 计算机控制插件 (Windows)
#include "computer_use_plugin.h"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include <cstring>
#include <string>

namespace agentxx_computer_use_plugin {
std::string uiControlExecute(const utilxx_base::Json& arguments);

struct PluginCtx : public agentxx::plugin::PluginBase {};

static const char* kUiControlDefaultDepict
    = "Send mouse, keyboard, and scroll commands in a single batch (Windows). "
      "Commands are executed sequentially in order with optional delays. "
      "Coordinate system: absolute desktop coordinates across all monitors (x/y in pixels); "
      "screen (0,0) is primary monitor top-left. "
      "Text input supports full Unicode, newlines (\\n), emojis, and multiline text. "
      "Shortcut execution automatically presses modifier keys, presses target key, and releases in reverse order.";

static std::string makeUiControlSchema() {
    utilxx_base::Json schema                               = utilxx_base::Json::object();
    schema["type"]                                         = "object";
    schema["required"]                                     = utilxx_base::Json::array({"actions"});
    schema["properties"]                                   = utilxx_base::Json::object();
    schema["properties"]["actions"]                        = utilxx_base::Json::object();
    schema["properties"]["actions"]["type"]                = "array";
    schema["properties"]["actions"]["items"]               = utilxx_base::Json::object();
    schema["properties"]["actions"]["items"]["type"]       = "object";
    schema["properties"]["actions"]["items"]["required"]   = utilxx_base::Json::array({"action"});
    schema["properties"]["actions"]["items"]["properties"] = utilxx_base::Json::object();
    schema["properties"]["actions"]["items"]["properties"]["action"]      = utilxx_base::Json({
        {"type", "string"},
        {"enum",
         utilxx_base::Json::array(
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
    schema["properties"]["actions"]["items"]["properties"]["x"]           = utilxx_base::Json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["y"]           = utilxx_base::Json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["button"]      = utilxx_base::Json({
        {"type", "string"                                             },
        {"enum", utilxx_base::Json::array({"left", "right", "middle"})}
    });
    schema["properties"]["actions"]["items"]["properties"]["clicks"]      = utilxx_base::Json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["amount"]      = utilxx_base::Json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["direction"]   = utilxx_base::Json({
        {"type", "string"                                },
        {"enum", utilxx_base::Json::array({"up", "down"})}
    });
    schema["properties"]["actions"]["items"]["properties"]["text"]        = utilxx_base::Json({
        {"type", "string"}
    });
    schema["properties"]["actions"]["items"]["properties"]["key"]         = utilxx_base::Json({
        {"type", "string"}
    });
    schema["properties"]["actions"]["items"]["properties"]["keys"]        = utilxx_base::Json({
        {"type",  "array"                                },
        {"items", utilxx_base::Json({{"type", "string"}})}
    });
    schema["properties"]["actions"]["items"]["properties"]["delay_ms"]    = utilxx_base::Json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["duration_ms"] = utilxx_base::Json({
        {"type", "integer"}
    });
    return schema.dump();
}

} // namespace agentxx_computer_use_plugin

using namespace agentxx_computer_use_plugin;

struct ComputerUsePluginCtx : public agentxx::plugin::PluginBase {};

/// ==================== 生命周期 (create 只构造, start 注册, stop 撤销) ====================

static void* computerUseAgentStart(
    ComputerUsePluginCtx&         ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    if (!ctx.iface.tools || !ctx.iface.tools->register_tool) {
        agentxx::plugin::PluginString::set(ctx.host, err, "tools iface unavailable");
        return nullptr;
    }

    auto        p      = ctx.toolPrompt("agentxx_ui_control_keyboard_mouse");
    std::string depict = p.depict.empty() ? kUiControlDefaultDepict : p.depict;

    agentxx::plugin::blocking_tool(
        ctx,
        "agentxx_ui_control_keyboard_mouse",
        depict,
        makeUiControlSchema(),
        [](ComputerUsePluginCtx&, std::string_view args_json) -> std::string {
            agentxx::plugin::ArgReader reader(args_json);
            if (reader.hasParseError()) {
                throw std::runtime_error("invalid args json");
            }
            return uiControlExecute(reader.raw());
        }
    );

    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

/// stop: 本插件不持有自管线程/定时器, 只上报完成 (宿主负责撤销注册)
static void*
    computerUseAgentStop(ComputerUsePluginCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_EXPORT(
    ComputerUsePluginCtx,
    "agentxx_computer_use",
    "1.0.0",
    "Computer control on Windows: mouse, keyboard, and scroll input (SendInput based)",
    computerUseAgentStart,
    computerUseAgentStop
);
