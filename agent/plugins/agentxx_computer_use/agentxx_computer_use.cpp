// agentxx_computer_use —— 计算机控制插件 (Windows)
// - 注册工具:
//   - agentxx_ui_control_keyboard_mouse: 批量 UI 命令 (鼠标/键盘/滚动/拖拽)
// - 屏幕捕获已拆分到独立插件 agentxx_screen_capture (本插件 plugin.yaml
//   depends 声明依赖, 加载时须先于本插件)
// - 插件不链接 libagentxx: 描述经 get_tool_prompt 读取, 日志经 vtable log
#include "codegraph/core/json.hpp"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "computer_use_plugin.h"
#include "fmt/format.h"
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ui_control.cpp 提供的执行函数 (Windows 分支; 非 Windows 分支由 ui_control.cpp 提供)
namespace agentxx {
namespace tools {
std::string uiControlExecute(agentxx_computer_use_plugin::SimpleJson& arguments);
} // namespace tools
} // namespace agentxx

namespace agentxx_computer_use_plugin {

// =====================================================================
// 工具注册辅助
// =====================================================================

/// 读取宿主 toolPrompt 的 depict; 未配置返回空
static std::string readToolDepict(const std::string& toolName) {
    if (!g_host || !g_if.config || !g_if.config->get_tool_prompt) {
        return {};
    }
    char* json = g_if.config->get_tool_prompt(
        g_host,
        agentxx_plugin_sv(toolName.data(), toolName.size())
    );
    if (!json) {
        return {};
    }
    std::string s{json};
    g_host->vtable->free(json);
    SimpleJson j(s);
    if (!j.ok()) {
        return {};
    }
    std::string depict;
    jsonGetString(j.doc().at_pointer("/depict"), depict);
    return depict;
}

/// 注册工具 (schema/描述存储于插件侧静态区)
/// - 静态存储: spec.execute 为静态 lambda (无捕获), fn 存于静态区
///   (unique_ptr 保证地址稳定), 经 user_data 传递; 插件生命周期内有效
/// - 字符串字段以 string_view 传入 (宿主注册时拷贝, 插件侧静态区存 std::string)
struct ToolEntry {
    std::function<std::string(SimpleJson&)> fn;
};

static void registerTool(
    const char*                             name,
    const char*                             defaultDepict,
    const std::string&                      schema,
    std::function<std::string(SimpleJson&)> fn,
    int                                     flags = 0
) {
    static std::vector<std::string> g_storage;
    std::string                     depict = readToolDepict(name);
    if (depict.empty()) {
        depict = defaultDepict;
    }
    g_storage.push_back(std::move(depict));
    g_storage.push_back(schema);

    static std::vector<std::unique_ptr<ToolEntry>> g_entries;
    auto                                           entry = std::make_unique<ToolEntry>();
    entry->fn                                            = std::move(fn);
    auto* entryPtr                                       = entry.get();
    g_entries.push_back(std::move(entry));

    AgentxxToolSpec spec{};
    spec.name        = agentxx_plugin_sv(name, std::strlen(name));
    spec.description = agentxx_plugin_sv(
        g_storage[g_storage.size() - 2].data(),
        g_storage[g_storage.size() - 2].size()
    );
    spec.parameters_json = agentxx_plugin_sv(g_storage.back().data(), g_storage.back().size());
    spec.user_data       = entryPtr;
    spec.flags           = flags;
    spec.execute         = +[](void*                   ud,
                       AgentxxPluginStringView args_json,
                       AgentxxPluginStringView,
                       AgentxxPluginStringView,
                       char** err) -> char* {
        auto* e = static_cast<ToolEntry*>(ud);
        try {
            std::string argsStr{args_json.data ? args_json.data : "{}", args_json.size};
            SimpleJson  args(argsStr.empty() ? "{}" : argsStr);
            if (!args.ok()) {
                throw std::runtime_error("invalid args json");
            }
            return pluginStrdup(e->fn(args).c_str());
        } catch (const std::exception& ex) {
            if (err) {
                *err = pluginStrdup(ex.what());
            }
            return nullptr;
        } catch (...) {
            if (err) {
                *err = pluginStrdup("unknown exception");
            }
            return nullptr;
        }
    };
    if (!g_if.tools || !g_if.tools->register_tool
        || g_if.tools->register_tool(g_host, &spec) != 0) {
        pluginLog(3, fmt::format("agentxx_computer_use: register tool {} failed", name));
    }
}

// =====================================================================
// agentxx_ui_control_keyboard_mouse
// =====================================================================

/// 工具默认描述 (从 lib AgentPrompt 剥离迁移, 2026-08)
/// - 宿主 toolPrompt 无条目时经 set_prompt 写入 (见 ensureToolPromptInHost),
///   用户可经 yaml 覆盖 (覆盖早于插件加载, 写入前检查条目已存在则跳过)
static const char* kUiControlDefaultDepict
    = R"(Control mouse and keyboard on Windows. Accepts a list of UI commands and executes them sequentially.

## Actions

### Mouse
- `mouse_move`: Move cursor. Params: `x`, `y`
- `mouse_click`: Click. Params: `button` ("left"/"right"/"middle", default "left"), `x`, `y` (optional, move then click)
- `mouse_double_click`: Double-click. Params: same as mouse_click
- `mouse_scroll`: Scroll wheel. Params: `delta` (positive=up, negative=down, ±120 per notch), `x`, `y` (optional)
- `mouse_drag`: Drag. Params: `x1`, `y1`, `x2`, `y2`, `button` (default "left"), `duration_ms` (default 200)

### Keyboard
- `key_press`: Press and release a key. Params: `key`
- `key_down`: Hold a key down. Params: `key`
- `key_up`: Release a held key. Params: `key`
- `key_combo`: Press a key combination (e.g. Ctrl+C). Params: `keys` (array of key names)
- `key_type`: Type a text string. Params: `text`

### Utility
- `wait`: Pause execution. Params: `ms` (milliseconds, max 30000)
- `get_cursor_pos`: Get current cursor position. No params.
- `get_screen_size`: Get screen resolution. No params.

### Key Names
- Characters: "a"-"z", "0"-"9"
- Special: "enter", "tab", "escape", "backspace", "delete", "insert", "home", "end", "pageup", "pagedown", "up", "down", "left", "right", "space"
- Modifiers: "shift", "ctrl", "alt", "win"
- Function keys: "f1"-"f12"
- Lock keys: "capslock", "numlock", "scrolllock"
- Other: "printscreen", "pause", "apps"

### Examples
```json
{"action": "mouse_click", "button": "left", "x": 100, "y": 200}
{"action": "key_combo", "keys": ["ctrl", "c"]}
{"action": "key_type", "text": "Hello World"}
{"action": "mouse_drag", "x1": 100, "y1": 100, "x2": 300, "y2": 300}
```)";

static std::string uiControlSchema() {
    codegraph::Json commandsItem = codegraph::Json::object();
    commandsItem["type"]         = "object";
    commandsItem["properties"]   = codegraph::Json::object();
    auto& p                      = commandsItem["properties"];
    p["action"]                  = codegraph::Json({
        {"type",        "string"                                            },
        {"description",
         "Action to perform. One of: mouse_move, mouse_click, "
                                           "mouse_double_click, mouse_scroll, mouse_drag, key_press, "
                                           "key_down, key_up, key_combo, key_type, wait, "
                                           "get_cursor_pos, get_screen_size"}
    });
    p["x"]                       = codegraph::Json({
        {"type", "integer"}
    });
    p["y"]                       = codegraph::Json({
        {"type", "integer"}
    });
    p["x1"]                      = codegraph::Json({
        {"type", "integer"}
    });
    p["y1"]                      = codegraph::Json({
        {"type", "integer"}
    });
    p["x2"]                      = codegraph::Json({
        {"type", "integer"}
    });
    p["y2"]                      = codegraph::Json({
        {"type", "integer"}
    });
    p["button"]                  = codegraph::Json({
        {"type", "string"},
        {"enum",
         codegraph::Json::array(
             {codegraph::Json("left"), codegraph::Json("right"), codegraph::Json("middle")}
         )               }
    });
    p["delta"]                   = codegraph::Json({
        {"type", "integer"}
    });
    p["key"]                     = codegraph::Json({
        {"type",        "string"                                },
        {"description", "Key name for key_press/key_down/key_up"}
    });
    p["keys"]                    = codegraph::Json({
        {"type",        "array"                                         },
        {"items",       codegraph::Json({{"type", "string"}})           },
        {"description", "Key names for key_combo, e.g. [\"ctrl\",\"c\"]"}
    });
    p["text"]                    = codegraph::Json({
        {"type",        "string"                  },
        {"description", "Text string for key_type"}
    });
    p["ms"]                      = codegraph::Json({
        {"type",        "integer"            },
        {"description", "Wait duration in ms"}
    });
    p["durationMs"]              = codegraph::Json({
        {"type", "integer"}
    });

    codegraph::Json schema              = codegraph::Json::object();
    schema["type"]                      = "object";
    schema["properties"]                = codegraph::Json::object();
    schema["properties"]["commands"]    = codegraph::Json({
        {"type",        "array"                                      },
        {"description", "List of UI commands to execute sequentially"},
        {"items",       commandsItem                                 }
    });
    schema["properties"]["interval_ms"] = codegraph::Json({
        {"type",        "integer"                                   },
        {"description", "Delay between commands in ms. Default: 50."}
    });
    schema["required"]                  = codegraph::Json::array({codegraph::Json("commands")});
    return schema.dump();
}

static void registerUiControlTool() {
    registerTool(
        "agentxx_ui_control_keyboard_mouse",
        kUiControlDefaultDepict,
        uiControlSchema(),
        [](SimpleJson& args) -> std::string {
            return agentxx::tools::uiControlExecute(args);
        }
    );
}

/// 把插件默认提示词写入宿主 toolPrompt (仅当宿主无该条目时; io 线程)
/// - 用户 yaml 覆盖早于插件加载 → get_prompt 已含覆盖 → 跳过 (尊重用户配置)
/// - 宿主未提供 get_prompt/set_prompt (旧宿主) → 跳过, registerTool 回退插件默认
static void ensureToolPromptInHost() {
    if (!g_host || !g_if.prompt || !g_if.prompt->get_prompt || !g_if.prompt->set_prompt) {
        return;
    }
    char* json = g_if.prompt->get_prompt(g_host);
    if (!json) {
        return;
    }
    std::string s{json};
    g_host->vtable->free(json);
    SimpleJson j(s);
    if (!j.ok()) {
        return;
    }
    // 宿主已有条目 (用户 yaml 覆盖 / 之前已写入): 尊重, 不覆盖
    if (!j.doc().at_pointer("/toolPrompt/agentxx_ui_control_keyboard_mouse").error()) {
        return;
    }
    codegraph::Json tp    = codegraph::Json::object();
    tp["depict"]          = std::string{kUiControlDefaultDepict};
    codegraph::Json args  = codegraph::Json::object();
    args["commands"]      = "Ordered list of UI commands to execute sequentially.";
    args["interval_ms"]   = "Delay between commands in milliseconds. Default: 50. Set `0` for "
                            "no delay.";
    tp["args"]            = args;
    codegraph::Json patch = codegraph::Json::object();
    patch["toolPrompt"]   = codegraph::Json::object();
    patch["toolPrompt"]["agentxx_ui_control_keyboard_mouse"] = tp;
    std::string payload                                      = patch.dump();
    if (g_if.prompt->set_prompt(g_host, agentxx_plugin_sv(payload.data(), payload.size()))
        != 0) {
        pluginLog(3, "agentxx_computer_use: set_prompt failed");
    }
}

} // namespace agentxx_computer_use_plugin

using namespace agentxx_computer_use_plugin;

// =====================================================================
// 插件入口 (C ABI)
// =====================================================================

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("agentxx_computer_use"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV(
            "Computer control on Windows: keyboard/mouse (ui_control); screen capture provided by agentxx_screen_capture"
        ),
    };
    return &info;
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_entry(const AgentxxHost* host, void** /*plugin_ctx*/) {
    g_host = host;
    // 默认提示词写入宿主 (剥离自 lib AgentPrompt; 用户 yaml 覆盖优先)
    ensureToolPromptInHost();
    registerUiControlTool();
    pluginLog(2, "agentxx_computer_use loaded (1 tool)");
    return 0;
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_unload(void* /*plugin_ctx*/) {
    pluginLog(2, "agentxx_computer_use unloaded");
}
