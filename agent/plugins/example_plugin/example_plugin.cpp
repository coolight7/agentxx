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
#include "agentxx/plugin/plugin_guard.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "agentxx/plugin/plugin_tool_sync.h"
#include "fmt/format.h"
#include "fmt/ranges.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

static const AgentxxHost* g_host = nullptr;
/// 宿主接口表缓存 (entry 时 AgentIfaces::query 一次查询; 进程级静态数据)
static agentxx::plugin::AgentIfaces g_if{};

/// C ABI 边界异常守卫日志 (由守卫函数调用处显式传入; 定义见 client 侧全局
/// 声明之后 —— 需引用双端宿主缓存; noexcept)
static void pluginCatchLog(const char* msg) noexcept;

/// 字符串视图 → JSON 字符串字面量 (agentxx.agent.json 接口表; 结果含引号;
/// 供 fmt::format 组装 JSON 时嵌入字段值, 避免手工拼接)
static std::string agentJsonEscape(AgentxxPluginStringView sv) {
    if (!g_host || !g_if.json || !g_if.json->json_escape || agentxx_plugin_sv_empty(sv)) {
        return "\"\"";
    }
    char* esc = g_if.json->json_escape(g_host, sv);
    if (!esc) {
        return "\"\"";
    }
    std::string out{esc};
    g_host->vtable->free(esc);
    return out;
}

/* ---------------- get_info ---------------- */

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL (宿主按"未导出"处理)
    return agentxx::plugin_guard::guardCall(
        pluginCatchLog,
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
        static const AgentxxPluginInfo info{
            AGENTXX_PLUGIN_API_VERSION,
            AGENTXX_SV("example_plugin"),
            AGENTXX_SV("1.0.0"),
            AGENTXX_SV("Example native plugin: echo tool, hook, event, capability"),
        };
        return &info;
    });
}

/* ---------------- tool: example_echo (内联完成型: 快同步, io 线程直接执行) ---------------- */

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
    // C ABI 回调异常守卫: 内联工具在宿主 io 线程直接执行
    return agentxx::plugin_guard::guardCall(
        pluginCatchLog,
        nullptr,
        [&]() -> char* {
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
    });
}

/* ---------------- tool: example_sleep (自管异步型演示: 无线程轮询推进) ----------------
 * 展示统一异步操作模型的核心价值 —— 不开线程、不用 asio, 纯状态机实现异步:
 * start 登记截止时刻立即返回; 宿主 io 协程按 poll 建议延迟挂起等待,
 * 与其他会话/工具协程交错执行 (旧模型下这是阻塞池线程 600ms 的黑盒)
 */
typedef struct SleepJob {
    AgentxxOpNotify                    notify;
    std::chrono::steady_clock::time_point deadline;
    int                                totalMs;
    int                                cancelled;
} SleepJob;

static void* sleep_start(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView thread_id,
    AgentxxPluginStringView tool_call_id,
    const AgentxxOpNotify*  notify,
    char**                  error_out
) {
    (void)user_data;
    (void)thread_id;
    (void)tool_call_id;
    (void)error_out;
    // C ABI 回调异常守卫: start 由宿主 io 线程调用
    return agentxx::plugin_guard::guardCall(
        pluginCatchLog,
        nullptr,
        [&]() -> void* {
        if (!g_host) {
            return nullptr;
        }
        // 轻量解析 duration_ms (默认 200)
        int ms = 200;
        if (!agentxx_plugin_sv_empty(args_json)) {
            char* v = g_if.json ? g_if.json->json_get_string(g_host, args_json, AGENTXX_SV("durationMs"))
                                : nullptr;
            if (v) {
                try {
                    ms = std::stoi(v);
                } catch (...) {
                    ms = 200;
                }
                g_host->vtable->free(v);
            }
        }
        auto* job      = new SleepJob{};
        job->notify    = *notify;
        job->totalMs   = ms > 0 ? ms : 0;
        job->deadline  = std::chrono::steady_clock::now() + std::chrono::milliseconds(job->totalMs);
        return job;
    });
}

static int sleep_poll(void* user_data, void* op) {
    (void)user_data;
    // C ABI 回调异常守卫: poll 由宿主 io 驱动循环调用
    return agentxx::plugin_guard::guardCall(
        pluginCatchLog,
        AGENTXX_OP_POLL_DONE,
        [&]() -> int {
        auto* job = static_cast<SleepJob*>(op);
        if (!job || job->notify.done == nullptr) {
            return AGENTXX_OP_POLL_DONE;
        }
        if (job->cancelled) {
            char* payload = g_host ? g_host->vtable->strdup("{}") : nullptr;
            job->notify.done(job->notify.host_ud, AGENTXX_OP_CANCELLED, payload);
            delete job;
            return AGENTXX_OP_POLL_DONE;
        }
        auto now   = std::chrono::steady_clock::now();
        auto restMs = std::chrono::duration_cast<std::chrono::milliseconds>(job->deadline - now).count();
        if (restMs > 0) {
            return static_cast<int>(restMs); ///< 建议宿主睡到截止再问 (不空转不占线程)
        }
        const std::string out       = fmt::format(R"({{"slept_ms": {}}})", job->totalMs);
        char*             payload   = g_host ? g_host->vtable->strdup(out.c_str()) : nullptr;
        job->notify.done(job->notify.host_ud, AGENTXX_OP_OK, payload);
        delete job;
        return AGENTXX_OP_POLL_DONE;
    });
}

static void sleep_cancel(void* user_data, void* op) {
    (void)user_data;
    // C ABI 回调异常守卫
    agentxx::plugin_guard::guardCallVoid(pluginCatchLog, [&] {
        auto* job          = static_cast<SleepJob*>(op);
        if (job) {
            job->cancelled = 1;
        }
    });
}

/* ---------------- tool: example_caller (互调; 阻塞委托型) ---------------- */

static char* caller_execute(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView session_id,
    AgentxxPluginStringView tool_call_id,
    volatile int*           cancel_flag,
    char**                  error_out
) {
    (void)user_data;
    (void)tool_call_id;
    (void)cancel_flag;
    // C ABI 回调异常守卫: 同步工具在宿主阻塞池线程执行
    return agentxx::plugin_guard::guardCall(
        pluginCatchLog,
        nullptr,
        [&]() -> char* {
        if (!g_host) {
            return nullptr;
        }
        // 调用本插件的另一个工具 example_echo, 演示插件互调
        // (阻塞便捷版运行在 offload 池线程 —— io 线程被 fail-fast 拒绝)
        if (!g_if.tools || !g_if.tools->call_tool) {
            return nullptr;
        }
        char* err  = nullptr;
        char* resp = g_if.tools->call_tool(
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
    });
}

/* ---------------- hook: agent_start (快同步钩子垫片) ---------------- */

static int on_agent_start(
    void*                   user_data,
    AgentxxHookPoint        point,
    AgentxxPluginStringView node_input_json,
    char**                  error_out
) {
    (void)user_data;
    (void)point;
    (void)node_input_json;
    (void)error_out;
    // C ABI 回调异常守卫
    return agentxx::plugin_guard::guardCall(
        pluginCatchLog,
        1,
        [&]() -> int {
        if (g_host && g_if.log && g_if.log->log) {
            g_if.log->log(g_host, 2 /* info */, AGENTXX_SV("example hook: agent_start fired"));
        }
        return 0;
    });
}

/* ---------------- event ---------------- */

static void on_demo_event(AgentxxPluginStringView event_json, void* ud) {
    (void)event_json;
    (void)ud;
    // C ABI 回调异常守卫 (宿主 EventBus 派发直调)
    agentxx::plugin_guard::guardCallVoid(pluginCatchLog, [&] {
        if (g_host && g_if.log && g_if.log->log) {
            g_if.log->log(g_host, 2, AGENTXX_SV("example event received"));
        }
    });
}

/// 跨端事件: client 插件 send_plugin_data("hello") 上行 →
/// 服务端发布 plugin.client.example_plugin.hello → 本订阅消费 (演示双端互通)
static void on_client_hello(AgentxxPluginStringView event_json, void* ud) {
    (void)event_json;
    (void)ud;
    // C ABI 回调异常守卫
    agentxx::plugin_guard::guardCallVoid(pluginCatchLog, [&] {
        if (g_host && g_if.log && g_if.log->log) {
            g_if.log->log(g_host, 2, AGENTXX_SV("example received client hello event"));
        }
    });
}

/* ---------------- entry / unload ---------------- */

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: entry 含注册/提示词读写等可抛操作, 异常返回 -1
    return agentxx::plugin_guard::guardCall(
        pluginCatchLog,
        -1,
        [&]() -> int {
        g_host = host;
        (void)plugin_ctx;

        // COM 风格接口表查询: entry 内一次性查询全部已知 IID 并缓存
        // (进程级静态数据, 长期有效; 未实现的表为 NULL, 使用前判空)
        static const agentxx::plugin::AgentIfaces s_if = agentxx::plugin::AgentIfaces::query(host);
        g_if = s_if;

        // 1. 工具 (agentxx.agent.tools 接口表)
        if (!s_if.tools || !s_if.tools->register_tool || !s_if.events) {
            return -1;
        }

        // echo: 内联完成型 (快同步, io 线程直接执行, 零线程切换)
        AgentxxInlineToolSpec echo{};
        echo.name        = AGENTXX_SV("example_echo");
        echo.description = AGENTXX_SV("Echo the input arguments back as JSON (example plugin tool).");
        echo.parameters_json
            = AGENTXX_SV(R"({"type":"object","properties":{},"additionalProperties":true})");
        echo.execute = echo_execute;
        if (agentxx_register_inline_tool(host, &echo) != 0) {
            return -1;
        }

        // caller: 阻塞委托型 (经 offload 在池线程执行; 内部用阻塞版 call_tool)
        AgentxxSyncToolSpec caller{};
        caller.name = AGENTXX_SV("example_caller");
        caller.description
            = AGENTXX_SV("Call example_echo via call_tool to demonstrate plugin interop.");
        caller.parameters_json
            = AGENTXX_SV(R"({"type":"object","properties":{},"additionalProperties":true})");
        caller.execute = caller_execute;
        if (agentxx_register_sync_tool(host, &caller) != 0) {
            return -1;
        }

        // sleeper: 自管异步型 (纯状态机轮询推进 —— 不开线程不用异步库;
        // 宿主 io 协程按 poll 建议延迟挂起, 与其他协程交错执行)
        AgentxxToolSpec sleeper{};
        sleeper.name = AGENTXX_SV("example_sleep");
        sleeper.description
            = AGENTXX_SV("Sleep duration_ms milliseconds then return (slow plugin tool).");
        sleeper.parameters_json
            = AGENTXX_SV(R"({"type":"object","properties":{"durationMs":{"type":"integer"}}})");
        sleeper.execute_start      = sleep_start;
        sleeper.execute_poll       = sleep_poll;
        sleeper.execute_cancel     = sleep_cancel;
        sleeper.user_data          = nullptr;
        sleeper.default_timeout_ms = 0; // 无默认超时 (测试用例自行指定)
        if (s_if.tools->register_tool(host, &sleeper) != 0) {
            return -1;
        }

        // 2. 钩子 (agentxx.agent.hooks 接口表; 快同步钩子垫片注册)
        if (!s_if.hooks || !s_if.hooks->register_hook
            || agentxx_register_sync_hook(host, AGENTXX_HOOK_AGENT_START, on_agent_start, nullptr)
                   != 0) {
            return -1;
        }

        // 3. 事件订阅 (agentxx.agent.events 接口表; topic 自动加 "plugin." 前缀 → plugin.demo.topic)
        AgentxxSubscription* sub
            = s_if.events->subscribe(host, AGENTXX_SV("demo.topic"), on_demo_event, nullptr);
        if (!sub) {
            return -1;
        }

        // 3.1 跨端事件订阅: client 插件上行 (服务端发布 plugin.client.example_plugin.hello;
        // 插件侧传 "client.example_plugin.hello" 即可)
        if (!s_if.events->subscribe(
                host,
                AGENTXX_SV("client.example_plugin.hello"),
                on_client_hello,
                nullptr
            )) {
            return -1;
        }

        // 4. 能力 (agentxx.agent.capabilities 接口表)
        if (!s_if.capabilities || !s_if.capabilities->register_capability
            || s_if.capabilities->register_capability(host, AGENTXX_SV("example.demo")) != 0) {
            return -1;
        }

        // 5. 提示词读写演示 (agentxx.agent.prompt 接口表; 宿主未提供该表时跳过)
        //    - 把 example_echo 的默认提示词写入宿主 toolPrompt (仅当宿主无该条目,
        //      用户 yaml 覆盖早于插件加载, 已存在则尊重用户配置不覆盖)
        //    - 宿主卸载插件时自动回滚本次写入 (恢复加载前状态)
        if (s_if.prompt && s_if.prompt->get_prompt && s_if.prompt->set_prompt) {
            char* full = s_if.prompt->get_prompt(host);
            if (full) {
                std::string prompt{full};
                host->vtable->free(full);
                bool hasEntry = prompt.find("\"example_echo\"") != std::string::npos;
                if (!hasEntry) {
                    const char* promptJson
                        = R"({"toolPrompt":{"example_echo":{"depict":"Echo the input arguments back as JSON (example plugin tool).","args":{}}}})";
                    int rc = s_if.prompt->set_prompt(host, AGENTXX_SV(promptJson));
                    if (rc != 0) {
                        if (s_if.log && s_if.log->log) {
                            s_if.log->log(host, 3, AGENTXX_SV("example plugin set_prompt failed"));
                        }
                        return -1;
                    }
                }
            }
        }

        s_if.log->log(host, 2, AGENTXX_SV("example plugin loaded"));
        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_unload(void* plugin_ctx) {
    // C ABI 边界异常守卫: 卸载回调异常不得外泄
    agentxx::plugin_guard::guardCallVoid(pluginCatchLog, [&] {
        (void)plugin_ctx;
        if (!g_host) {
            return;
        }
        // 主动反注册 (宿主也会自动清理, 这里演示插件侧约定; 接口表判空遵循契约)
        if (g_if.tools && g_if.tools->unregister_tool) {
            g_if.tools->unregister_tool(g_host, AGENTXX_SV("example_echo"));
            g_if.tools->unregister_tool(g_host, AGENTXX_SV("example_caller"));
            g_if.tools->unregister_tool(g_host, AGENTXX_SV("example_sleep"));
        }
        if (g_if.hooks && g_if.hooks->unregister_hook) {
            g_if.hooks->unregister_hook(g_host, AGENTXX_HOOK_AGENT_START);
        }
        if (g_if.capabilities && g_if.capabilities->unregister_capability) {
            g_if.capabilities->unregister_capability(g_host, AGENTXX_SV("example.demo"));
        }
        if (g_if.log && g_if.log->log) {
            g_if.log->log(g_host, 2, AGENTXX_SV("example plugin unloaded"));
        }
        g_host = nullptr;
    });
}

/* =====================================================================
 * client 侧入口 (agentxx_client_entry) —— 双端插件演示
 *
 * 同一动态库同时导出 agent 入口 (agentxx_plugin_entry) 与 client 入口
 * (agentxx_client_entry); 两个 PluginManager 各自 dlopen/装配, 实例状态
 * 彼此独立, 互通一律走 wire (send_plugin_data ↔ WirePluginDataUp)
 * ===================================================================== */

static const AgentxxClientHost* g_client_host  = nullptr;
/// client 侧接口表缓存 (entry 时 ClientIfaces::query 一次查询)
static agentxx::plugin::ClientIfaces g_client_if{};
/// "agentxx.client.ui" 展示接口表 (状态栏/面板/Info 段落/命令/toast; 表内不支持子能力
/// 成员为 NULL, 调用前判空)
static const AgentxxClientUiIface* g_client_ui = nullptr;
static AgentxxStatusItem*       g_status_item  = nullptr;
static AgentxxPanel*            g_panel        = nullptr;
static AgentxxInfoSection*      g_info_section = nullptr;
static int                      g_turn_count   = 0;

/// C ABI 边界异常守卫日志 (noexcept; 栈缓冲 + 宿主 log 接口表, 双端插件
/// agent/client 两侧宿主任一可用即输出)
static void pluginCatchLog(const char* msg) noexcept {
    if (g_host && g_if.log && g_if.log->log) {
        agentxx::plugin_guard::logTo(g_host, g_if.log, 4, "example_plugin", msg);
        return;
    }
    if (g_client_host && g_client_if.log && g_client_if.log->log) {
        agentxx::plugin_guard::logTo(
            g_client_host,
            g_client_if.log,
            4,
            "example_plugin",
            msg
        );
    }
}

/// 字符串 → JSON 字符串字面量 (经宿主 agentxx.client.json 接口表; 结果含引号;
/// 供 fmt::format 组装 JSON 时嵌入字段值, 避免手工拼接)
static std::string clientJsonEscape(const std::string& s) {
    if (!g_client_host || !g_client_if.json || !g_client_if.json->json_escape || s.empty()) {
        return "\"\"";
    }
    char* esc = g_client_if.json->json_escape(
        g_client_host,
        agentxx_plugin_sv(s.data(), s.size())
    );
    if (!esc) {
        return "\"\"";
    }
    std::string out{esc};
    g_client_host->vtable->free(esc);
    return out;
}

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxClientPluginInfo* agentxx_client_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL (宿主按"未导出"处理)
    return agentxx::plugin_guard::guardCall(
        pluginCatchLog,
        nullptr,
        [&]() -> const AgentxxClientPluginInfo* {
        static const AgentxxClientPluginInfo info{
            AGENTXX_CLIENT_PLUGIN_API_VERSION,
            AGENTXX_SV("example_plugin"),
            AGENTXX_SV("1.0.0"),
            AGENTXX_SV(
                "Example client plugin: status item, panel, Info section, commands, events, cross-side data"
            ),
            // v4 移除 min_ui_caps 位图字段: 最低接口要求改由 plugin.yaml
            // interfaces.require 声明 (宿主加载前门禁), 见 docs/agent/plugins.md
        };
        return &info;
    });
}

/* ---------------- 命令: /example (send 动作) ---------------- */

static char* example_cmd_execute(void* ud, AgentxxPluginStringView args_json, char** error_out) {
    (void)ud;
    (void)error_out;
    // C ABI 回调异常守卫: 命令执行在 client io 线程 (宿主输入管线直调)
    return agentxx::plugin_guard::guardCall(
        pluginCatchLog,
        nullptr,
        [&]() -> char* {
        if (!g_client_host) {
            return nullptr;
        }
        // 参数: {"text": "..."} (输入 "/example 参数" 的剩余部分)
        char* argText = g_client_if.json ? g_client_if.json->json_get_string(
                                g_client_host, args_json, AGENTXX_SV("text"))
                                         : nullptr;
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
    });
}

/* ---------------- 命令: /example_toast (toast 动作) ---------------- */

static char* example_toast_execute(void* ud, AgentxxPluginStringView args_json, char** error_out) {
    (void)ud;
    (void)error_out;
    // C ABI 回调异常守卫
    return agentxx::plugin_guard::guardCall(
        pluginCatchLog,
        nullptr,
        [&]() -> char* {
        if (!g_client_host) {
            return nullptr;
        }
        char* argText  = g_client_if.json ? g_client_if.json->json_get_string(
                                g_client_host, args_json, AGENTXX_SV("text"))
                                          : nullptr;
        std::string text = argText && *argText ? argText : "toast from example plugin";
        if (argText) {
            g_client_host->vtable->free(argText);
        }
        const std::string out
            = fmt::format(R"({{"action":"toast","text":{},"level":1}})", clientJsonEscape(text));
        return g_client_host->vtable->strdup(out.c_str());
    });
}

/* ---------------- 事件订阅 ---------------- */

/// READY: 服务端就绪 → 更新状态栏项 + 跨端上行 hello
static void on_client_ready(AgentxxPluginStringView payload_json, void* ud) {
    (void)ud;
    // C ABI 回调异常守卫 (client io 线程派发直调)
    agentxx::plugin_guard::guardCallVoid(pluginCatchLog, [&] {
        if (!g_client_host) {
            return;
        }
        if (g_client_if.log && g_client_if.log->log) {
            g_client_if.log->log(g_client_host, 2, AGENTXX_SV("client example: ready"));
        }
        // 跨端数据 (agentxx.client.wire 接口表): client → agent (服务端发布到
        // plugin.client.example_plugin.hello, agent 侧 on_client_hello 订阅消费)
        if (g_client_if.wire && g_client_if.wire->send_plugin_data) {
            g_client_if.wire->send_plugin_data(
                g_client_host,
                AGENTXX_SV("hello"),
                AGENTXX_SV(R"({"from":"client-example"})")
            );
        }
    });
}

/// TURN_END: 轮次结束 → 状态栏项文本更新 + Info 栏段落内容更新
static void on_client_turn_end(AgentxxPluginStringView payload_json, void* ud) {
    (void)ud;
    (void)payload_json;
    // C ABI 回调异常守卫
    agentxx::plugin_guard::guardCallVoid(pluginCatchLog, [&] {
        if (!g_client_host) {
            return;
        }
        ++g_turn_count;
        if (g_status_item && g_client_ui && g_client_ui->update_status_item) {
            // json_escape 返回带引号的 JSON 字符串字面量, fmt 直接嵌入
            const std::string json = fmt::format(
                R"({{"text":{}}})",
                clientJsonEscape(fmt::format("turns: {}", g_turn_count))
            );
            g_client_ui->update_status_item(
                g_client_host,
                g_status_item,
                agentxx_plugin_sv(json.data(), json.size())
            );
        }
        if (g_info_section && g_client_ui && g_client_ui->update_info_section) {
            // Info 栏段落: {"items":[{"kind":"text","text":"Turns: N"},
            // {"kind":"text","role":"hint","text":"Example Info section is live"}]}
            // 注意: clientJsonEscape 返回【带引号的 JSON 字面量】(如 "\"Turns: 1\""),
            // 必须整段放入 escape 调用 (占整个 text 值), 不能嵌入模板的 {} 里
            // (否则产生 {"text":"Turns: "1""} 非法 JSON, 宿主解析失败静默丢弃)
            const std::string json = fmt::format(
                R"({{"items":[{{"kind":"text","text":{}}},{{"kind":"text","role":"hint","text":"Example Info section is live"}}]}})",
                clientJsonEscape(fmt::format("Turns: {}", g_turn_count))
            );
            g_client_ui->update_info_section(g_client_host, g_info_section, AGENTXX_SV(json.c_str()));
        }
    });
}

/// PLUGIN_DATA: 收到 agent 侧插件事件 (WirePluginData) → 面板展示
static void on_client_plugin_data(AgentxxPluginStringView payload_json, void* ud) {
    (void)ud;
    if (!g_client_host || !g_panel) {
        return;
    }
    if (!g_client_if.json || !g_client_if.json->json_get_string) {
        return;
    }
    // payload: {"plugin","event","data"}
    char* plugin   = g_client_if.json->json_get_string(
        g_client_host, payload_json, AGENTXX_SV("plugin")
    );
    char* event    = g_client_if.json->json_get_string(
        g_client_host, payload_json, AGENTXX_SV("event")
    );
    char* data     = g_client_if.json->json_get_string(
        g_client_host, payload_json, AGENTXX_SV("data")
    );
    // 异常守卫: 处理区含字符串分配; free 在守卫块后无条件执行防泄漏
    agentxx::plugin_guard::guardCallVoid(pluginCatchLog, [&] {
        std::string line = fmt::format("{}.{}", plugin ? plugin : "?", event ? event : "?");
        if (data && *data) {
            line = fmt::format("{}: {}", line, data);
        }
        // json_escape 返回带引号的 JSON 字符串字面量, fmt 直接嵌入
        const std::string json = fmt::format(
            R"({{"items":[{{"kind":"text","text":{}}},{{"kind":"badge","text":"updated"}}]}})",
            clientJsonEscape(line)
        );
        if (g_client_ui && g_client_ui->update_panel) {
            g_client_ui->update_panel(g_client_host, g_panel, AGENTXX_SV(json.c_str()));
        }
    });
    if (plugin) {
        g_client_host->vtable->free(plugin);
    }
    if (event) {
        g_client_host->vtable->free(event);
    }
    if (data) {
        g_client_host->vtable->free(data);
    }
}

/* ---------------- entry / unload ---------------- */

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_client_entry(const AgentxxClientHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: 异常返回 -1 (加载失败)
    return agentxx::plugin_guard::guardCall(
        pluginCatchLog,
        -1,
        [&]() -> int {
        g_client_host = host;
        (void)plugin_ctx;

        // COM 风格接口表查询: entry 内一次性查询全部已知 IID 并缓存
        g_client_if  = agentxx::plugin::ClientIfaces::query(host);
        g_client_ui  = g_client_if.ui;

        // 1. 状态栏项 (左侧 align=0, order=10)
        g_status_item = g_client_ui && g_client_ui->register_status_item
                          ? g_client_ui->register_status_item(
                              host,
                              AGENTXX_SV("example_plugin.turns"),
                              AGENTXX_SV(R"({"text":"turns: 0"})"),
                              0,
                              10
                          )
                          : nullptr;
        // 宿主不支持状态栏 (如 CLI) 时成员为 NULL, 插件降级 (不视为失败)

        // 2. 侧边栏面板
        g_panel = g_client_ui && g_client_ui->register_panel
                    ? g_client_ui->register_panel(
                        host,
                        AGENTXX_SV("example_plugin.panel"),
                        AGENTXX_SV(R"({"title":"Example"})")
                    )
                    : nullptr;

        // 3. 侧边栏 Info 栏段落 (段落标题 "Example Info"; 内容由 TURN_END 更新)
        g_info_section = g_client_ui && g_client_ui->register_info_section
                           ? g_client_ui->register_info_section(
                               host,
                               AGENTXX_SV("example_plugin.info"),
                               AGENTXX_SV(R"({"title":"Example Info"})")
                           )
                           : nullptr;

        // 4. 命令 (命令输入管线接口 agentxx.client.command 不支持的宿主上成员为 NULL:
        //    命令是本插件核心演示功能, 此时加载失败并报告, 与原行为一致)
        if (!g_client_ui || !g_client_ui->register_command) {
            return -1;
        }
        if (g_client_ui->register_command(
                host,
                AGENTXX_SV("example"),
                AGENTXX_SV("Send a message from the example plugin"),
                example_cmd_execute,
                nullptr
            )
            != 0) {
            return -1;
        }
        if (g_client_ui->register_command(
                host,
                AGENTXX_SV("example_toast"),
                AGENTXX_SV("Show a toast from the example plugin"),
                example_toast_execute,
                nullptr
            )
            != 0) {
            return -1;
        }

        // 5. 事件订阅 (agentxx.client.events 接口表)
        if (!g_client_if.events || !g_client_if.events->subscribe) {
            return -1;
        }
        if (!g_client_if.events
                 ->subscribe(host, AGENTXX_CLIENT_EVT_READY, on_client_ready, nullptr)) {
            return -1;
        }
        if (!g_client_if.events
                 ->subscribe(host, AGENTXX_CLIENT_EVT_TURN_END, on_client_turn_end, nullptr)) {
            return -1;
        }
        if (!g_client_if.events
                 ->subscribe(host, AGENTXX_CLIENT_EVT_PLUGIN_DATA, on_client_plugin_data, nullptr)) {
            return -1;
        }

        if (g_client_if.log && g_client_if.log->log) {
            g_client_if.log->log(host, 2, AGENTXX_SV("example client plugin loaded"));
        }
        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_client_unload(void* plugin_ctx) {
    // C ABI 边界异常守卫: 卸载回调异常不得外泄
    agentxx::plugin_guard::guardCallVoid(pluginCatchLog, [&] {
        (void)plugin_ctx;
        if (!g_client_host) {
            return;
        }
        // 主动反注册 (宿主也会自动清理, 这里演示插件侧约定; 成员判空遵循
        // 扩展表契约 —— 不支持的子能力成员为 NULL)
        if (g_client_ui->unregister_command) {
            g_client_ui->unregister_command(g_client_host, AGENTXX_SV("example"));
            g_client_ui->unregister_command(g_client_host, AGENTXX_SV("example_toast"));
        }
        if (g_status_item && g_client_ui->unregister_status_item) {
            g_client_ui->unregister_status_item(g_client_host, g_status_item);
            g_status_item = nullptr;
        }
        if (g_panel && g_client_ui->unregister_panel) {
            g_client_ui->unregister_panel(g_client_host, g_panel);
            g_panel = nullptr;
        }
        if (g_info_section && g_client_ui->unregister_info_section) {
            g_client_ui->unregister_info_section(g_client_host, g_info_section);
            g_info_section = nullptr;
        }
        if (g_client_if.log && g_client_if.log->log) {
            g_client_if.log->log(
                g_client_host,
                2,
                AGENTXX_SV("example client plugin unloaded")
            );
        }
        g_client_host = nullptr;
        g_client_ui   = nullptr;
    });
}
