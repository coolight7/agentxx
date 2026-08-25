// agentxx_computer_use —— 计算机控制插件 (Windows)
// - 注册工具:
//   - agentxx_ui_control_keyboard_mouse: 批量 UI 命令 (鼠标/键盘/滚动/拖拽)
// - 屏幕捕获已拆分到独立插件 agentxx_screen_capture (本插件 plugin.yaml
//   depends 声明依赖, 加载时须先于本插件)
// - 插件不链接 libagentxx: 描述经 get_tool_prompt 读取, 日志经 vtable log
#include "codegraph/core/json.hpp"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "agentxx/plugin/plugin_tool_sync.h"
#include "computer_use_plugin.h"
#include "fmt/format.h"
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ui_control.cpp 提供的执行函数 (Windows 分支; 非 Windows 分支由 ui_control.cpp 提供)
namespace agentxx_computer_use_plugin {
std::string uiControlExecute(agentxx_computer_use_plugin::SimpleJson& arguments);
} // namespace agentxx_computer_use_plugin

namespace agentxx_computer_use_plugin {

// =====================================================================
// 每实例上下文 (多实例契约: 原进程级 static 存储/取消标志全部移入)
// =====================================================================

/// 注册工具执行闭包条目 (经 user_data 传递; unique_ptr 保证地址稳定;
/// 实例生命周期内有效)
struct ToolEntry {
    const AgentxxHost*                      host = nullptr;
    std::function<std::string(SimpleJson&)> fn;
};

struct PluginCtx {
    const AgentxxHost*           host  = nullptr;
    agentxx::plugin::AgentIfaces iface {};
    /// spec 字符串稳定存储 + 工具闭包条目 + 垫片适配器 (随实例销毁释放;
    /// 原 static 容器在多实例下互相串状态且只增不减 —— 已修)
    std::vector<std::string> storage;
    std::vector<std::unique_ptr<ToolEntry>> tool_entries;
    std::vector<std::unique_ptr<AgentxxSyncToolShim>> sync_tool_shims;
};

// =====================================================================
// 工具注册辅助
// =====================================================================

static std::string readToolDepict(const PluginCtx& ctx, const std::string& toolName) {
    if (!ctx.host || !ctx.iface.config || !ctx.iface.config->get_tool_prompt) {
        return {};
    }
    char* json = ctx.iface.config->get_tool_prompt(
        ctx.host,
        agentxx_plugin_sv(toolName.data(), toolName.size())
    );
    if (!json) {
        return {};
    }
    std::string s{json};
    ctx.host->vtable->free(json);
    SimpleJson j(s);
    if (!j.ok()) {
        return {};
    }
    std::string depict;
    jsonGetString(j.doc().at_pointer("/depict"), depict);
    return depict;
}

static void registerTool(
    PluginCtx&                              ctx,
    const char*                             name,
    const char*                             defaultDepict,
    const std::string&                      schema,
    std::function<std::string(SimpleJson&)> fn,
    int                                     flags = 0
) {
    std::string depict = readToolDepict(ctx, name);
    if (depict.empty()) {
        depict = defaultDepict;
    }
    ctx.storage.push_back(std::move(depict));
    ctx.storage.push_back(schema);

    auto  entry    = std::make_unique<ToolEntry>();
    entry->host    = ctx.host;
    entry->fn      = std::move(fn);
    auto* entryPtr = entry.get();
    ctx.tool_entries.push_back(std::move(entry));

    // 垫片适配器: 实例内嵌存储 (随实例销毁释放; 多实例契约)
    ctx.sync_tool_shims.push_back(std::make_unique<AgentxxSyncToolShim>());
    auto* shim = ctx.sync_tool_shims.back().get();

    AgentxxSyncToolSpec spec{};
    spec.name        = agentxx_plugin_sv(name, std::strlen(name));
    spec.description = agentxx_plugin_sv(
        ctx.storage[ctx.storage.size() - 2].data(),
        ctx.storage[ctx.storage.size() - 2].size()
    );
    spec.parameters_json = agentxx_plugin_sv(ctx.storage.back().data(), ctx.storage.back().size());
    spec.user_data       = entryPtr;
    spec.flags           = flags;
    // 阻塞委托型: 键鼠注入序列执行为慢同步操作 (offload 池线程)
    spec.execute         = +[](void*                   ud,
                       AgentxxPluginStringView args_json,
                       AgentxxPluginStringView,
                       AgentxxPluginStringView,
                       volatile int*,
                       char** err) -> char* {
        auto* e = static_cast<ToolEntry*>(ud);
        try {
            std::string argsStr{args_json.data ? args_json.data : "{}", args_json.size};
            SimpleJson  args(argsStr.empty() ? "{}" : argsStr);
            if (!args.ok()) {
                throw std::runtime_error("invalid args json");
            }
            return pluginStrdup(e->host, e->fn(args).c_str());
        } catch (const std::exception& ex) {
            if (err) {
                *err = pluginStrdup(e->host, ex.what());
            }
            return nullptr;
        } catch (...) {
            if (err) {
                *err = pluginStrdup(e->host, "unknown exception");
            }
            return nullptr;
        }
    };
    if (agentxx_register_sync_tool(ctx.host, &spec, shim) != 0) {
        pluginLog(ctx.host, ctx.iface.log, 3,
                  fmt::format("agentxx_computer_use: register tool {} failed", name));
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

static void registerUiControlTool(PluginCtx& ctx) {
    registerTool(
        ctx,
        "agentxx_ui_control_keyboard_mouse",
        kUiControlDefaultDepict,
        uiControlSchema(),
        [](SimpleJson& args) -> std::string {
            return agentxx_computer_use_plugin::uiControlExecute(args);
        }
    );
}

/// 把插件默认提示词写入宿主 toolPrompt (仅当宿主无该条目时; io 线程)
/// - 用户 yaml 覆盖早于插件加载 → get_prompt 已含覆盖 → 跳过 (尊重用户配置)
/// - 宿主未提供 get_prompt/set_prompt (旧宿主) → 跳过, registerTool 回退插件默认
static void ensureToolPromptInHost(PluginCtx& ctx) {
    if (!ctx.host || !ctx.iface.prompt || !ctx.iface.prompt->get_prompt
        || !ctx.iface.prompt->set_prompt) {
        return;
    }
    char* json = ctx.iface.prompt->get_prompt(ctx.host);
    if (!json) {
        return;
    }
    std::string s{json};
    ctx.host->vtable->free(json);
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
    if (ctx.iface.prompt->set_prompt(ctx.host, agentxx_plugin_sv(payload.data(), payload.size()))
        != 0) {
        pluginLog(ctx.host, ctx.iface.log, 3, "agentxx_computer_use: set_prompt failed");
    }
}

} // namespace agentxx_computer_use_plugin

using namespace agentxx_computer_use_plugin;

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
            AGENTXX_SV("agentxx_computer_use"),
            AGENTXX_SV("1.0.0"),
            AGENTXX_SV(
                "Computer control on Windows: keyboard/mouse (ui_control); screen capture provided by agentxx_screen_capture"
            ),
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
        // 默认提示词写入宿主 (剥离自 lib AgentPrompt; 用户 yaml 覆盖优先)
        ensureToolPromptInHost(*ctx);
        registerUiControlTool(*ctx);
        pluginLog(ctx->host, ctx->iface.log, 2, "agentxx_computer_use loaded (1 tool)");
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
        pluginLog(ctx ? ctx->host : nullptr, ctx ? ctx->iface.log : nullptr,
                  2, "agentxx_computer_use unloaded");
        delete ctx; // storage/tool_entries/shims 均为 ctx 成员, 随之释放
        });
}
