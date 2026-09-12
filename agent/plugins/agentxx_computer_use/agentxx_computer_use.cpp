/// agentxx_computer_use —— 计算机控制插件 (Windows)
#include "agentxx/util/json.h"
#include "computer_use_plugin.h"
#include "fmt/format.h"
#include <cstring>
#include <string>

namespace agentxx_computer_use_plugin {
std::string uiControlExecute(const agentxx::util::Json& arguments);

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
    agentxx::util::Json schema                           = agentxx::util::Json::object();
    schema["type"]                                       = "object";
    schema["required"]                                   = agentxx::util::Json::array({"actions"});
    schema["properties"]                                 = agentxx::util::Json::object();
    schema["properties"]["actions"]                      = agentxx::util::Json::object();
    schema["properties"]["actions"]["type"]              = "array";
    schema["properties"]["actions"]["items"]             = agentxx::util::Json::object();
    schema["properties"]["actions"]["items"]["type"]     = "object";
    schema["properties"]["actions"]["items"]["required"] = agentxx::util::Json::array({"action"});
    schema["properties"]["actions"]["items"]["properties"] = agentxx::util::Json::object();
    schema["properties"]["actions"]["items"]["properties"]["action"]      = agentxx::util::Json({
        {"type", "string"},
        {"enum",
         agentxx::util::Json::array(
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
    schema["properties"]["actions"]["items"]["properties"]["x"]           = agentxx::util::Json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["y"]           = agentxx::util::Json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["button"]      = agentxx::util::Json({
        {"type", "string"                                               },
        {"enum", agentxx::util::Json::array({"left", "right", "middle"})}
    });
    schema["properties"]["actions"]["items"]["properties"]["clicks"]      = agentxx::util::Json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["amount"]      = agentxx::util::Json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["direction"]   = agentxx::util::Json({
        {"type", "string"                                  },
        {"enum", agentxx::util::Json::array({"up", "down"})}
    });
    schema["properties"]["actions"]["items"]["properties"]["text"]        = agentxx::util::Json({
        {"type", "string"}
    });
    schema["properties"]["actions"]["items"]["properties"]["key"]         = agentxx::util::Json({
        {"type", "string"}
    });
    schema["properties"]["actions"]["items"]["properties"]["keys"]        = agentxx::util::Json({
        {"type",  "array"                                  },
        {"items", agentxx::util::Json({{"type", "string"}})}
    });
    schema["properties"]["actions"]["items"]["properties"]["delay_ms"]    = agentxx::util::Json({
        {"type", "integer"}
    });
    schema["properties"]["actions"]["items"]["properties"]["duration_ms"] = agentxx::util::Json({
        {"type", "integer"}
    });
    return schema.dump();
}

} // namespace agentxx_computer_use_plugin

using namespace agentxx_computer_use_plugin;

struct ComputerUsePluginCtx : public agentxx::plugin::PluginBase {};

/// ==================== 生命周期 (create 只构造, start 注册, stop 撤销) ====================

static void* computerUseAgentStart(
    ComputerUsePluginCtx&              ctx,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               err
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

    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

/// stop: 本插件不持有自管线程/定时器, 只上报完成 (宿主负责撤销注册)
static void*
    computerUseAgentStop(ComputerUsePluginCtx&, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*) {
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
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
