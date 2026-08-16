// agentxx_computer_use —— 计算机控制插件 (Windows)
// - 注册工具:
//   - agentxx_ui_control_keyboard_mouse: 批量 UI 命令 (鼠标/键盘/滚动/拖拽)
// - 屏幕捕获已拆分到独立插件 agentxx_screen_capture (本插件 plugin.yaml
//   depends 声明依赖, 加载时须先于本插件)
// - 插件不链接 libagentxx: 描述经 get_tool_prompt 读取, 日志经 vtable log
#include "computer_use_plugin.h"
#include "codegraph/core/json.hpp"
#include "fmt/format.h"
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
    if (!g_host || !g_host->vtable || !g_host->vtable->get_tool_prompt) {
        return {};
    }
    char* json = g_host->vtable->get_tool_prompt(g_host, toolName.c_str());
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
struct ToolEntry {
    std::function<std::string(SimpleJson&)> fn;
};

static void registerTool(
    const char*                                   name,
    const char*                                   defaultDepict,
    const std::string&                            schema,
    std::function<std::string(SimpleJson&)>       fn,
    int                                           flags = 0
) {
    static std::vector<std::string> g_storage;
    std::string depict = readToolDepict(name);
    if (depict.empty()) {
        depict = defaultDepict;
    }
    g_storage.push_back(std::move(depict));
    g_storage.push_back(schema);

    static std::vector<std::unique_ptr<ToolEntry>> g_entries;
    auto entry = std::make_unique<ToolEntry>();
    entry->fn  = std::move(fn);
    auto* entryPtr = entry.get();
    g_entries.push_back(std::move(entry));

    AgentxxToolSpec spec{};
    spec.name            = const_cast<char*>(name);
    spec.description     = const_cast<char*>(g_storage[g_storage.size() - 2].c_str());
    spec.parameters_json = const_cast<char*>(g_storage.back().c_str());
    spec.user_data       = entryPtr;
    spec.flags           = flags;
    spec.execute         = +[](void* ud, const char* args_json, const char*, const char*, char** err
                           ) -> char* {
        auto* e = static_cast<ToolEntry*>(ud);
        try {
            SimpleJson args(args_json ? args_json : "{}");
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
    if (g_host->vtable->register_tool(g_host, &spec) != 0) {
        pluginLog(3, fmt::format("agentxx_computer_use: register tool {} failed", name));
    }
}

// =====================================================================
// agentxx_ui_control_keyboard_mouse
// =====================================================================

static std::string uiControlSchema() {
    codegraph::Json commandsItem = codegraph::Json::object();
    commandsItem["type"]         = "object";
    commandsItem["properties"]   = codegraph::Json::object();
    auto& p                      = commandsItem["properties"];
    p["action"] = codegraph::Json({{"type", "string"},
                                   {"description",
                                    "Action to perform. One of: mouse_move, mouse_click, "
                                    "mouse_double_click, mouse_scroll, mouse_drag, key_press, "
                                    "key_down, key_up, key_combo, key_type, wait, "
                                    "get_cursor_pos, get_screen_size"}});
    p["x"]      = codegraph::Json({{"type", "number"}});
    p["y"]      = codegraph::Json({{"type", "number"}});
    p["x1"]     = codegraph::Json({{"type", "number"}});
    p["y1"]     = codegraph::Json({{"type", "number"}});
    p["x2"]     = codegraph::Json({{"type", "number"}});
    p["y2"]     = codegraph::Json({{"type", "number"}});
    p["button"] = codegraph::Json(
        {{"type", "string"},
         {"enum", codegraph::Json::array({codegraph::Json("left"),
                                          codegraph::Json("right"),
                                          codegraph::Json("middle")})}}
    );
    p["delta"] = codegraph::Json({{"type", "number"}});
    p["key"]   = codegraph::Json(
        {{"type", "string"},
         {"description", "Key name for key_press/key_down/key_up"}}
    );
    p["keys"] = codegraph::Json(
        {{"type", "array"},
         {"items", codegraph::Json({{"type", "string"}})},
         {"description", "Key names for key_combo, e.g. [\"ctrl\",\"c\"]"}}
    );
    p["text"] = codegraph::Json(
        {{"type", "string"}, {"description", "Text string for key_type"}}
    );
    p["ms"]          = codegraph::Json({{"type", "number"}, {"description", "Wait duration in ms"}});
    p["duration_ms"] = codegraph::Json({{"type", "number"}});

    codegraph::Json schema = codegraph::Json::object();
    schema["type"]         = "object";
    schema["properties"]   = codegraph::Json::object();
    schema["properties"]["commands"] = codegraph::Json(
        {{"type", "array"},
         {"description", "List of UI commands to execute sequentially"},
         {"items", commandsItem}}
    );
    schema["properties"]["interval_ms"] = codegraph::Json(
        {{"type", "number"},
         {"description", "Delay between commands in ms. Default: 50."}}
    );
    schema["required"] = codegraph::Json::array({codegraph::Json("commands")});
    return schema.dump();
}

static void registerUiControlTool() {
    registerTool(
        "agentxx_ui_control_keyboard_mouse",
        "Control mouse and keyboard on Windows. Accepts a list of UI commands and executes them sequentially.",
        uiControlSchema(),
        [](SimpleJson& args) -> std::string {
            return agentxx::tools::uiControlExecute(args);
        }
    );
}

} // namespace agentxx_computer_use_plugin

using namespace agentxx_computer_use_plugin;

// =====================================================================
// 插件入口 (C ABI)
// =====================================================================

extern "C" const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        "agentxx_computer_use",
        "1.0.0",
        "Computer control on Windows: keyboard/mouse (ui_control); screen capture provided by agentxx_screen_capture",
    };
    return &info;
}

extern "C" int agentxx_plugin_entry(const AgentxxHost* host, void** /*plugin_ctx*/) {
    g_host = host;
    registerUiControlTool();
    pluginLog(2, "agentxx_computer_use loaded (1 tool)");
    return 0;
}

extern "C" void agentxx_plugin_unload(void* /*plugin_ctx*/) {
    pluginLog(2, "agentxx_computer_use unloaded");
}
