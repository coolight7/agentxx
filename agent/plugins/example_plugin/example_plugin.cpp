/*
 * example_plugin —— 一期示例插件 (C++ 实现, 仅依赖纯 C ABI 头)
 *
 * 演示能力:
 * 1. 工具注册: example_echo (原样回显) / example_caller (经 call_tool 互调)
 * 2. 钩子: agent_start 钩子 (日志输出)
 * 3. 事件: 订阅 plugin.demo.topic
 * 4. 能力: 声明 capability "example.demo"
 * 5. 卸载: unload 回调主动反注册
 * 6. client 入口 (双端插件, agentxx_client_entry):
 *    - 状态栏项 example.status (轮次结束事件更新文本)
 *    - 侧边栏面板 example.panel (展示收到的跨端插件事件)
 *    - 命令 /example (send 动作) 与 /example_toast (toast 动作)
 *    - 订阅 client 事件 (READY / TURN_END / PLUGIN_DATA)
 *    - 跨端数据: READY 时 send_plugin_data("hello") 上行到 agent 侧
 *      (agent 侧订阅 "client.example_plugin.hello" 消费)
 *
 * 编译 (无需链接 libagentxx):
 *   g++ -std=c++26 -fPIC -shared example_native.cpp -o libexample_plugin.so
 */
#include "agentxx/plugin/client_plugin_api.h"
#include "agentxx/plugin/plugin_api.h"
#include "fmt/format.h"
#include "fmt/ranges.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

static const AgentxxHost* g_host = nullptr;

/// 字符串视图 → JSON 字符串字面量 (agent 侧宿主 vtable json_escape; 结果含
/// 引号; 供 fmt::format 组装 JSON 时嵌入字段值, 避免手工拼接)
static std::string agentJsonEscape(AgentxxPluginStringView sv) {
    if (!g_host || agentxx_plugin_sv_empty(sv)) {
        return "\"\"";
    }
    char* esc = g_host->vtable->json_escape(g_host, sv);
    if (!esc) {
        return "\"\"";
    }
    std::string out{esc};
    g_host->vtable->free(esc);
    return out;
}

/* ---------------- get_info ---------------- */

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("example_plugin"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("Example native plugin: echo tool, hook, event, capability"),
    };
    return &info;
}

/* ---------------- tool: example_echo ---------------- */

static char* echo_execute(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView session_id,
    AgentxxPluginStringView tool_call_id,
    char**                  error_out
) {
    (void)user_data;
    (void)tool_call_id;
    (void)error_out;
    if (!g_host) {
        return nullptr; // 宿主不可用: 无法分配错误串 (host 为 null)
    }
    // 结果 JSON: {"echo": <原样参数>}
    const std::string out = fmt::format(
        R"({{"echo": {},"sessionId": {}}})",
        std::string_view{args_json.data ? args_json.data : "{}", args_json.size},
        agentJsonEscape(session_id)
    );
    return g_host->vtable->strdup(out.c_str());
}

/* ---------------- tool: example_sleep (慢工具, 测试超时/卸载竞态用) ---------------- */

/// 阻塞 duration_ms 毫秒后返回 (模拟慢插件工具):
/// - 宿主超时/取消只终止"等待", 本回调一旦开始执行将持续到返回
///   (宿主按 inflight 计数保证执行期间插件代码段不被卸载)
static char* sleep_execute(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView session_id,
    AgentxxPluginStringView tool_call_id,
    char**                  error_out
) {
    (void)user_data;
    (void)session_id;
    (void)tool_call_id;
    (void)error_out;
    if (!g_host) {
        return nullptr;
    }
    // 轻量解析 duration_ms (默认 200)
    int ms = 200;
    if (!agentxx_plugin_sv_empty(args_json)) {
        char* v = g_host->vtable->json_get_string(g_host, args_json, AGENTXX_SV("durationMs"));
        if (v) {
            try {
                ms = std::stoi(v);
            } catch (...) {
                ms = 200;
            }
            g_host->vtable->free(v);
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(std::max(ms, 0)));
    const std::string out = fmt::format(R"({{"slept_ms": {}}})", ms);
    return g_host->vtable->strdup(out.c_str());
}

/* ---------------- tool: example_caller (互调) ---------------- */

static char* caller_execute(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView session_id,
    AgentxxPluginStringView tool_call_id,
    char**                  error_out
) {
    (void)user_data;
    (void)tool_call_id;
    if (!g_host) {
        return nullptr;
    }
    // 调用本插件的另一个工具 example_echo, 演示插件互调
    char* err  = nullptr;
    char* resp = g_host->vtable->call_tool(
        g_host,
        AGENTXX_SV("example_echo"),
        agentxx_plugin_sv_empty(args_json) ? AGENTXX_SV("{}") : args_json,
        session_id,
        &err
    );
    if (!resp) {
        if (err) {
            *error_out = err; // 直接移交
        } else {
            *error_out = g_host->vtable->strdup("call_tool failed");
        }
        return nullptr;
    }
    const std::string out = fmt::format(R"({{"via_call_tool": {}}})", resp);
    g_host->vtable->free(resp);
    return g_host->vtable->strdup(out.c_str());
}

/* ---------------- hook: agent_start ---------------- */

static int on_agent_start(
    void*                   user_data,
    AgentxxHookPoint        point,
    AgentxxPluginStringView node_input_json,
    char**                  out_json,
    char**                  error_out
) {
    (void)user_data;
    (void)point;
    (void)node_input_json;
    (void)out_json;
    (void)error_out;
    if (g_host) {
        g_host->vtable->log(g_host, 2 /* info */, AGENTXX_SV("example hook: agent_start fired"));
    }
    return 0;
}

/* ---------------- event ---------------- */

static void on_demo_event(AgentxxPluginStringView event_json, void* ud) {
    (void)event_json;
    (void)ud;
    if (g_host) {
        g_host->vtable->log(g_host, 2, AGENTXX_SV("example event received"));
    }
}

/// 跨端事件: client 插件 send_plugin_data("hello") 上行 →
/// 服务端发布 plugin.client.example_plugin.hello → 本订阅消费 (演示双端互通)
static void on_client_hello(AgentxxPluginStringView event_json, void* ud) {
    (void)event_json;
    (void)ud;
    if (g_host) {
        g_host->vtable->log(g_host, 2, AGENTXX_SV("example received client hello event"));
    }
}

/* ---------------- entry / unload ---------------- */

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    g_host = host;
    (void)plugin_ctx;

    // 1. 工具
    AgentxxToolSpec echo{};
    echo.name        = AGENTXX_SV("example_echo");
    echo.description = AGENTXX_SV("Echo the input arguments back as JSON (example plugin tool).");
    echo.parameters_json
        = AGENTXX_SV(R"({"type":"object","properties":{},"additionalProperties":true})");
    echo.execute = echo_execute;
    if (host->vtable->register_tool(host, &echo) != 0) {
        return -1;
    }

    AgentxxToolSpec caller{};
    caller.name = AGENTXX_SV("example_caller");
    caller.description
        = AGENTXX_SV("Call example_echo via call_tool to demonstrate plugin interop.");
    caller.parameters_json
        = AGENTXX_SV(R"({"type":"object","properties":{},"additionalProperties":true})");
    caller.execute = caller_execute;
    if (host->vtable->register_tool(host, &caller) != 0) {
        return -1;
    }

    // 慢工具: 测试插件超时/卸载竞态 (宿主超时后回调仍可能执行, inflight 保活)
    AgentxxToolSpec sleeper{};
    sleeper.name = AGENTXX_SV("example_sleep");
    sleeper.description
        = AGENTXX_SV("Sleep duration_ms milliseconds then return (slow plugin tool).");
    sleeper.parameters_json
        = AGENTXX_SV(R"({"type":"object","properties":{"durationMs":{"type":"integer"}}})");
    sleeper.execute            = sleep_execute;
    sleeper.default_timeout_ms = 0; // 无默认超时 (测试用例自行指定)
    if (host->vtable->register_tool(host, &sleeper) != 0) {
        return -1;
    }

    // 2. 钩子 (agent_start)
    if (host->vtable->register_hook(host, AGENTXX_HOOK_AGENT_START, on_agent_start, nullptr) != 0) {
        return -1;
    }

    // 3. 事件订阅 (topic 自动加 "plugin." 前缀 → plugin.demo.topic)
    AgentxxSubscription* sub
        = host->vtable->subscribe(host, AGENTXX_SV("demo.topic"), on_demo_event, nullptr);
    if (!sub) {
        return -1;
    }

    // 3.1 跨端事件订阅: client 插件上行 (服务端发布 plugin.client.example_plugin.hello;
    // 插件侧传 "client.example_plugin.hello" 即可)
    if (!host->vtable->subscribe(
            host,
            AGENTXX_SV("client.example_plugin.hello"),
            on_client_hello,
            nullptr
        )) {
        return -1;
    }

    // 4. 能力
    if (host->vtable->register_capability(host, AGENTXX_SV("example.demo")) != 0) {
        return -1;
    }

    // 5. 提示词读写演示 (get_prompt/set_prompt; 宿主为旧版本无此 API 时跳过)
    //    - 把 example_echo 的默认提示词写入宿主 toolPrompt (仅当宿主无该条目,
    //      用户 yaml 覆盖早于插件加载, 已存在则尊重用户配置不覆盖)
    //    - 宿主卸载插件时自动回滚本次写入 (恢复加载前状态)
    if (host->vtable->get_prompt && host->vtable->set_prompt) {
        char* full = host->vtable->get_prompt(host);
        if (full) {
            std::string prompt{full};
            host->vtable->free(full);
            bool hasEntry = prompt.find("\"example_echo\"") != std::string::npos;
            if (!hasEntry) {
                const char* promptJson
                    = R"({"toolPrompt":{"example_echo":{"depict":"Echo the input arguments back as JSON (example plugin tool).","args":{}}}})";
                int rc = host->vtable->set_prompt(host, AGENTXX_SV(promptJson));
                if (rc != 0) {
                    host->vtable->log(host, 3, AGENTXX_SV("example plugin set_prompt failed"));
                    return -1;
                }
            }
        }
    }

    host->vtable->log(host, 2, AGENTXX_SV("example plugin loaded"));
    return 0;
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_unload(void* plugin_ctx) {
    (void)plugin_ctx;
    if (!g_host) {
        return;
    }
    // 主动反注册 (宿主也会自动清理, 这里演示插件侧约定)
    g_host->vtable->unregister_tool(g_host, AGENTXX_SV("example_echo"));
    g_host->vtable->unregister_tool(g_host, AGENTXX_SV("example_caller"));
    g_host->vtable->unregister_tool(g_host, AGENTXX_SV("example_sleep"));
    g_host->vtable->unregister_hook(g_host, AGENTXX_HOOK_AGENT_START, on_agent_start, nullptr);
    g_host->vtable->unregister_capability(g_host, AGENTXX_SV("example.demo"));
    g_host->vtable->log(g_host, 2, AGENTXX_SV("example plugin unloaded"));
    g_host = nullptr;
}

/* =====================================================================
 * client 侧入口 (agentxx_client_entry) —— 双端插件演示
 *
 * 同一动态库同时导出 agent 入口 (agentxx_plugin_entry) 与 client 入口
 * (agentxx_client_entry); 两个 PluginManager 各自 dlopen/装配, 实例状态
 * 彼此独立, 互通一律走 wire (send_plugin_data ↔ WirePluginDataUp)
 * ===================================================================== */

static const AgentxxClientHost* g_client_host  = nullptr;
static AgentxxStatusItem*       g_status_item  = nullptr;
static AgentxxPanel*            g_panel        = nullptr;
static AgentxxInfoSection*      g_info_section = nullptr;
static int                      g_turn_count   = 0;

/// 字符串 → JSON 字符串字面量 (经宿主 vtable json_escape; 结果含引号;
/// 供 fmt::format 组装 JSON 时嵌入字段值, 避免手工拼接)
static std::string clientJsonEscape(const std::string& s) {
    if (!g_client_host || s.empty()) {
        return "\"\"";
    }
    char* esc
        = g_client_host->vtable->json_escape(g_client_host, agentxx_plugin_sv(s.data(), s.size()));
    if (!esc) {
        return "\"\"";
    }
    std::string out{esc};
    g_client_host->vtable->free(esc);
    return out;
}

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxClientPluginInfo* agentxx_client_get_info(void) {
    static const AgentxxClientPluginInfo info{
        AGENTXX_CLIENT_PLUGIN_API_VERSION,
        AGENTXX_SV("example_plugin"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV(
            "Example client plugin: status item, panel, Info section, commands, events, cross-side data"
        ),
        0, // min_ui_caps: 无最低要求 (无 UI 能力的 CLI 也可加载)
    };
    return &info;
}

/* ---------------- 命令: /example (send 动作) ---------------- */

static char* example_cmd_execute(void* ud, AgentxxPluginStringView args_json, char** error_out) {
    (void)ud;
    (void)error_out;
    if (!g_client_host) {
        return nullptr;
    }
    // 参数: {"text": "..."} (输入 "/example 参数" 的剩余部分)
    char* argText
        = g_client_host->vtable->json_get_string(g_client_host, args_json, AGENTXX_SV("text"));
    std::string suffix = argText ? argText : "";
    if (argText) {
        g_client_host->vtable->free(argText);
    }
    // 动作: send —— 代发一条用户消息 (与用户输入同排队语义)
    std::string text = "Hello from example plugin";
    if (!suffix.empty()) {
        text = fmt::format("{} ({})", text, suffix);
    }
    // json_escape 返回带引号的 JSON 字符串字面量 (如 "\"abc\""), fmt 直接嵌入
    const std::string out = fmt::format(R"({{"action":"send","text":{}}})", clientJsonEscape(text));
    return g_client_host->vtable->strdup(out.c_str());
}

/* ---------------- 命令: /example_toast (toast 动作) ---------------- */

static char* example_toast_execute(void* ud, AgentxxPluginStringView args_json, char** error_out) {
    (void)ud;
    (void)error_out;
    if (!g_client_host) {
        return nullptr;
    }
    char* argText
        = g_client_host->vtable->json_get_string(g_client_host, args_json, AGENTXX_SV("text"));
    std::string text = argText && *argText ? argText : "toast from example plugin";
    if (argText) {
        g_client_host->vtable->free(argText);
    }
    const std::string out
        = fmt::format(R"({{"action":"toast","text":{},"level":1}})", clientJsonEscape(text));
    return g_client_host->vtable->strdup(out.c_str());
}

/* ---------------- 事件订阅 ---------------- */

/// READY: 服务端就绪 → 更新状态栏项 + 跨端上行 hello
static void on_client_ready(AgentxxPluginStringView payload_json, void* ud) {
    (void)ud;
    if (!g_client_host) {
        return;
    }
    g_client_host->vtable->log(g_client_host, 2, AGENTXX_SV("client example: ready"));
    // 跨端数据: client → agent (服务端发布到 plugin.client.example_plugin.hello,
    // agent 侧 on_client_hello 订阅消费)
    g_client_host->vtable->send_plugin_data(
        g_client_host,
        AGENTXX_SV("hello"),
        AGENTXX_SV(R"({"from":"client-example"})")
    );
}

/// TURN_END: 轮次结束 → 状态栏项文本更新 + Info 栏段落内容更新
static void on_client_turn_end(AgentxxPluginStringView payload_json, void* ud) {
    (void)ud;
    (void)payload_json;
    if (!g_client_host) {
        return;
    }
    ++g_turn_count;
    if (g_status_item) {
        // json_escape 返回带引号的 JSON 字符串字面量, fmt 直接嵌入
        const std::string json = fmt::format(
            R"({{"text":{}}})",
            clientJsonEscape(fmt::format("turns: {}", g_turn_count))
        );
        g_client_host->vtable->update_status_item(
            g_client_host,
            g_status_item,
            agentxx_plugin_sv(json.data(), json.size())
        );
    }
    if (g_info_section) {
        // Info 栏段落: {"items":[{"kind":"text","text":"Turns: N"},
        // {"kind":"text","role":"hint","text":"Example Info section is live"}]}
        // 注意: clientJsonEscape 返回【带引号的 JSON 字面量】(如 "\"Turns: 1\""),
        // 必须整段放入 escape 调用 (占整个 text 值), 不能嵌入模板的 {} 里
        // (否则产生 {"text":"Turns: "1""} 非法 JSON, 宿主解析失败静默丢弃)
        const std::string json = fmt::format(
            R"({{"items":[{{"kind":"text","text":{}}},{{"kind":"text","role":"hint","text":"Example Info section is live"}}]}})",
            clientJsonEscape(fmt::format("Turns: {}", g_turn_count))
        );
        g_client_host->vtable
            ->update_info_section(g_client_host, g_info_section, AGENTXX_SV(json.c_str()));
    }
}

/// PLUGIN_DATA: 收到 agent 侧插件事件 (WirePluginData) → 面板展示
static void on_client_plugin_data(AgentxxPluginStringView payload_json, void* ud) {
    (void)ud;
    if (!g_client_host || !g_panel) {
        return;
    }
    // payload: {"plugin","event","data"}
    char* plugin
        = g_client_host->vtable->json_get_string(g_client_host, payload_json, AGENTXX_SV("plugin"));
    char* event
        = g_client_host->vtable->json_get_string(g_client_host, payload_json, AGENTXX_SV("event"));
    char* data
        = g_client_host->vtable->json_get_string(g_client_host, payload_json, AGENTXX_SV("data"));
    std::string line = fmt::format("{}.{}", plugin ? plugin : "?", event ? event : "?");
    if (data && *data) {
        line = fmt::format("{}: {}", line, data);
    }
    if (plugin) {
        g_client_host->vtable->free(plugin);
    }
    if (event) {
        g_client_host->vtable->free(event);
    }
    if (data) {
        g_client_host->vtable->free(data);
    }
    // json_escape 返回带引号的 JSON 字符串字面量, fmt 直接嵌入
    const std::string json = fmt::format(
        R"({{"items":[{{"kind":"text","text":{}}},{{"kind":"badge","text":"updated"}}]}})",
        clientJsonEscape(line)
    );
    g_client_host->vtable->update_panel(g_client_host, g_panel, AGENTXX_SV(json.c_str()));
}

/* ---------------- entry / unload ---------------- */

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_client_entry(const AgentxxClientHost* host, void** plugin_ctx) {
    g_client_host = host;
    (void)plugin_ctx;

    // 1. 状态栏项 (左侧 align=0, order=10)
    g_status_item = host->vtable->register_status_item(
        host,
        AGENTXX_SV("example_plugin.turns"),
        AGENTXX_SV(R"({"text":"turns: 0"})"),
        0,
        10
    );
    // 宿主不支持状态栏 (如 CLI) 时注册返回 NULL, 插件降级 (不视为失败)

    // 2. 侧边栏面板
    g_panel = host->vtable->register_panel(
        host,
        AGENTXX_SV("example_plugin.panel"),
        AGENTXX_SV(R"({"title":"Example"})")
    );

    // 3. 侧边栏 Info 栏段落 (段落标题 "Example Info"; 内容由 TURN_END 更新)
    g_info_section = host->vtable->register_info_section(
        host,
        AGENTXX_SV("example_plugin.info"),
        AGENTXX_SV(R"({"title":"Example Info"})")
    );

    // 3. 命令
    if (host->vtable->register_command(
            host,
            AGENTXX_SV("example"),
            AGENTXX_SV("Send a message from the example plugin"),
            example_cmd_execute,
            nullptr
        )
        != 0) {
        return -1;
    }
    if (host->vtable->register_command(
            host,
            AGENTXX_SV("example_toast"),
            AGENTXX_SV("Show a toast from the example plugin"),
            example_toast_execute,
            nullptr
        )
        != 0) {
        return -1;
    }

    // 4. 事件订阅
    if (!host->vtable->subscribe(host, AGENTXX_CLIENT_EVT_READY, on_client_ready, nullptr)) {
        return -1;
    }
    if (!host->vtable->subscribe(host, AGENTXX_CLIENT_EVT_TURN_END, on_client_turn_end, nullptr)) {
        return -1;
    }
    if (!host->vtable
             ->subscribe(host, AGENTXX_CLIENT_EVT_PLUGIN_DATA, on_client_plugin_data, nullptr)) {
        return -1;
    }

    host->vtable->log(host, 2, AGENTXX_SV("example client plugin loaded"));
    return 0;
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_client_unload(void* plugin_ctx) {
    (void)plugin_ctx;
    if (!g_client_host) {
        return;
    }
    // 主动反注册 (宿主也会自动清理, 这里演示插件侧约定)
    g_client_host->vtable->unregister_command(g_client_host, AGENTXX_SV("example"));
    g_client_host->vtable->unregister_command(g_client_host, AGENTXX_SV("example_toast"));
    if (g_status_item) {
        g_client_host->vtable->unregister_status_item(g_client_host, g_status_item);
        g_status_item = nullptr;
    }
    if (g_panel) {
        g_client_host->vtable->unregister_panel(g_client_host, g_panel);
        g_panel = nullptr;
    }
    if (g_info_section) {
        g_client_host->vtable->unregister_info_section(g_client_host, g_info_section);
        g_info_section = nullptr;
    }
    g_client_host->vtable->log(g_client_host, 2, AGENTXX_SV("example client plugin unloaded"));
    g_client_host = nullptr;
}
