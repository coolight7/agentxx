/*
 * agentxx_plugin_example —— 一期示例插件 (C++ 实现, 仅依赖纯 C ABI 头)
 *
 * 演示能力:
 * 1. 工具注册: example_echo (原样回显) / example_caller (经 call_tool 互调)
 * 2. 钩子: agent_start 钩子 (日志输出)
 * 3. 事件: 订阅 plugin.demo.topic
 * 4. 能力: 声明 capability "example.demo"
 * 5. 卸载: unload 回调主动反注册
 *
 * 编译 (无需链接 libagentxx):
 *   g++ -std=c++17 -fPIC -shared example_native.cpp -o libagentxx_plugin_example.so
 */
#include "agentxx/plugin/plugin_api.h"

#include <cstdio>
#include <cstring>
#include <string>

static const AgentxxHost* g_host = nullptr;

/* ---------------- get_info ---------------- */

extern "C" const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        "agentxx_plugin_example",
        "1.0.0",
        "Example native plugin: echo tool, hook, event, capability",
    };
    return &info;
}

/* ---------------- tool: example_echo ---------------- */

static char* echo_execute(void*, const char* args_json, const char* thread_id,
                          const char* tool_call_id, char** error_out) {
    (void)tool_call_id;
    if (!g_host) {
        *error_out = g_host->vtable->strdup("host not ready");
        return nullptr;
    }
    // 结果 JSON: {"echo": <原样参数>}
    std::string out = "{\"echo\": ";
    out += (args_json ? args_json : "{}");
    out += ", \"thread_id\": \"";
    out += (thread_id ? thread_id : "");
    out += "\"}";
    return g_host->vtable->strdup(out.c_str());
}

/* ---------------- tool: example_caller (互调) ---------------- */

static char* caller_execute(void*, const char* args_json, const char* thread_id,
                            const char* tool_call_id, char** error_out) {
    (void)tool_call_id;
    if (!g_host) {
        *error_out = g_host->vtable->strdup("host not ready");
        return nullptr;
    }
    // 调用本插件的另一个工具 example_echo, 演示插件互调
    char* err  = nullptr;
    char* resp = g_host->vtable->call_tool(
        g_host, "example_echo", args_json ? args_json : "{}", thread_id, &err
    );
    if (!resp) {
        if (err) {
            *error_out = err; // 直接移交
        } else {
            *error_out = g_host->vtable->strdup("call_tool failed");
        }
        return nullptr;
    }
    std::string out = "{\"via_call_tool\": ";
    out += resp;
    out += "}";
    g_host->vtable->free(resp);
    return g_host->vtable->strdup(out.c_str());
}

/* ---------------- hook: agent_start ---------------- */

static int on_agent_start(void* user_data, AgentxxHookPoint point, const char* node_input_json,
                          char** out_json, char** error_out) {
    (void)user_data;
    (void)out_json;
    (void)error_out;
    if (g_host) {
        g_host->vtable->log(g_host, 2 /* info */, "example hook: agent_start fired");
    }
    std::printf("[example] hook point=%d input=%s\n", static_cast<int>(point),
                node_input_json ? node_input_json : "");
    return 0;
}

/* ---------------- event ---------------- */

static void on_demo_event(const char* event_json, void* ud) {
    (void)ud;
    if (g_host) {
        g_host->vtable->log(g_host, 2, "example event received");
    }
    std::printf("[example] event: %s\n", event_json ? event_json : "");
}

/* ---------------- entry / unload ---------------- */

extern "C" int agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    g_host = host;
    (void)plugin_ctx;

    // 1. 工具
    AgentxxToolSpec echo{};
    echo.name            = "example_echo";
    echo.description     = "Echo the input arguments back as JSON (example plugin tool).";
    echo.parameters_json = R"({"type":"object","properties":{},"additionalProperties":true})";
    echo.execute         = echo_execute;
    if (host->vtable->register_tool(host, &echo) != 0) {
        return -1;
    }

    AgentxxToolSpec caller{};
    caller.name            = "example_caller";
    caller.description     = "Call example_echo via call_tool to demonstrate plugin interop.";
    caller.parameters_json = R"({"type":"object","properties":{},"additionalProperties":true})";
    caller.execute         = caller_execute;
    if (host->vtable->register_tool(host, &caller) != 0) {
        return -1;
    }

    // 2. 钩子 (agent_start)
    if (host->vtable->register_hook(host, AGENTXX_HOOK_AGENT_START, on_agent_start, nullptr)
        != 0) {
        return -1;
    }

    // 3. 事件订阅 (topic 自动加 "plugin." 前缀 → plugin.demo.topic)
    AgentxxSubscription* sub = host->vtable->subscribe(host, "demo.topic", on_demo_event, nullptr);
    if (!sub) {
        return -1;
    }

    // 4. 能力
    if (host->vtable->register_capability(host, "example.demo") != 0) {
        return -1;
    }

    host->vtable->log(host, 2, "example plugin loaded");
    return 0;
}

extern "C" void agentxx_plugin_unload(void* plugin_ctx) {
    (void)plugin_ctx;
    if (!g_host) {
        return;
    }
    // 主动反注册 (宿主也会自动清理, 这里演示插件侧约定)
    g_host->vtable->unregister_tool(g_host, "example_echo");
    g_host->vtable->unregister_tool(g_host, "example_caller");
    g_host->vtable->unregister_hook(g_host, AGENTXX_HOOK_AGENT_START, on_agent_start, nullptr);
    g_host->vtable->unregister_capability(g_host, "example.demo");
    g_host->vtable->log(g_host, 2, "example plugin unloaded");
    g_host = nullptr;
}
