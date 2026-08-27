/*
 * example_plugin —— 一期示例插件 (C++ 实现, 基于 plugin_kit.h)
 *
 * 演示能力:
 * 1. 工具注册:
 *    - example_echo (快同步 fast_tool, io 线程直跑)
 *    - example_caller (锚定 Task 协程, 经 call_tool 异步互调)
 *    - example_sleep (锚定 Task 协程, 经 co_await sleep 精确唤醒)
 * 2. 钩子: agent_start 钩子
 * 3. 事件: 订阅 plugin.demo.topic 与跨端事件
 * 4. 能力: 声明 capability "example.demo"
 * 5. 卸载: destroy 释放实例
 * 6. client 入口 (双端插件, agentxx_client_create)
 */
#include "agentxx/plugin/client_plugin_api.h"
#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_guard.h"
#include "agentxx/plugin/plugin_kit.h"
#include "fmt/format.h"

#include <memory>
#include <string>
#include <string_view>

/* =====================================================================
 * 每实例上下文
 * ===================================================================== */

struct AgentCtx : public agentxx::kit::PluginBase {
};

struct ClientCtx {
    const AgentxxClientHost*      host         = nullptr;
    agentxx::plugin::ClientIfaces iface        {};
    const AgentxxClientUiIface*   ui           = nullptr;
    AgentxxStatusItem*            status_item  = nullptr;
    AgentxxPanel*                 panel        = nullptr;
    AgentxxInfoSection*           info_section = nullptr;
    int                           turn_count   = 0;
};

static auto agentGuardLogger(AgentCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx && ctx->host && ctx->iface.log && ctx->iface.log->log) {
            agentxx::plugin_guard::logTo(ctx->host, ctx->iface.log, 4, "example_plugin", msg);
        }
    };
}

static auto clientGuardLogger(ClientCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx && ctx->host && ctx->iface.log && ctx->iface.log->log) {
            agentxx::plugin_guard::logTo(ctx->host, ctx->iface.log, 4, "example_plugin", msg);
        }
    };
}

/* ---------------- get_info ---------------- */

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    return agentxx::plugin_guard::guardCall(
        [](const char*) noexcept {},
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

/* ---------------- event handlers ---------------- */

static void on_demo_event(AgentxxPluginStringView event_json, void* ud) {
    (void)event_json;
    auto* ctxRaw = static_cast<AgentCtx*>(ud);
    agentxx::plugin_guard::guardCallVoid(agentGuardLogger(ctxRaw), [&] {
        auto* ctx = static_cast<AgentCtx*>(ud);
        if (ctx) {
            ctx->log.info("example event received");
        }
    });
}

static void on_client_hello(AgentxxPluginStringView event_json, void* ud) {
    (void)event_json;
    auto* ctxRaw = static_cast<AgentCtx*>(ud);
    agentxx::plugin_guard::guardCallVoid(agentGuardLogger(ctxRaw), [&] {
        auto* ctx = static_cast<AgentCtx*>(ud);
        if (ctx) {
            ctx->log.info("example received client hello event");
        }
    });
}

/* ---------------- entry / unload ---------------- */

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_create(const AgentxxHost* host, void** plugin_ctx) {
    AgentCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
        [&raw](const char* msg) noexcept { agentGuardLogger(raw)(msg); },
        -1,
        [&]() -> int {
        if (!host || !host->vtable || !plugin_ctx) {
            return -1;
        }
        auto ctx = std::make_unique<AgentCtx>();
        ctx->init(host);
        raw = ctx.get();

        if (!ctx->iface.tools || !ctx->iface.tools->register_tool || !ctx->iface.events) {
            return -1;
        }

        // 1.1 echo: 快同步内联工具 (fast_tool)
        agentxx::kit::fast_tool(
            *ctx,
            "example_echo",
            "Echo the input arguments back as JSON (example plugin tool).",
            R"({"type":"object","properties":{},"additionalProperties":true})",
            [](AgentCtx& c, std::string_view args, std::string_view tid) -> std::string {
                return fmt::format(
                    R"({{"echo": {},"sessionId": {}}})",
                    args.empty() ? "{}" : args,
                    c.jsonEscape(agentxx_plugin_sv(tid.data(), tid.size()))
                );
            }
        );

        // 1.2 caller: 锚定 Task 协程互调工具 (tool + call_tool)
        agentxx::kit::tool(
            *ctx,
            "example_caller",
            "Call example_echo via call_tool to demonstrate plugin interop.",
            R"({"type":"object","properties":{},"additionalProperties":true})",
            [](AgentCtx& c, std::string_view args, agentxx::kit::OpCtl ctl) -> agentxx::kit::Task<std::string> {
                std::string resp = co_await agentxx::kit::call_tool(c, "example_echo", args, ctl.threadId);
                co_return fmt::format(R"({{"via_call_tool": {}}})", resp);
            }
        );

        // 1.3 sleeper: 锚定 Task 协程 sleep 工具 (tool + sleep)
        agentxx::kit::tool(
            *ctx,
            "example_sleep",
            "Sleep duration_ms milliseconds then return (slow plugin tool).",
            R"({"type":"object","properties":{"durationMs":{"type":"integer"}}})",
            [](AgentCtx& c, std::string_view args, agentxx::kit::OpCtl ctl) -> agentxx::kit::Task<std::string> {
                int ms = 200;
                try {
                    auto j = neograph::json::parse(args);
                    if (j.contains("durationMs") && j["durationMs"].is_number()) {
                        ms = j["durationMs"].get<int>();
                    }
                } catch (...) {}
                co_await agentxx::kit::sleep(c, ms > 0 ? ms : 0);
                ctl.throw_if_cancelled();
                co_return fmt::format(R"({{"slept_ms": {}}})", ms);
            }
        );

        // 2. 钩子 (agent_start)
        if (ctx->iface.hooks && ctx->iface.hooks->register_hook) {
            agentxx::kit::hook(*ctx, AGENTXX_HOOK_AGENT_START, [](AgentCtx& c, AgentxxHookPoint, std::string_view) {
                c.log.info("example hook: agent_start fired");
            });
        }

        // 3. 事件订阅
        ctx->iface.events->subscribe(host, AGENTXX_SV("demo.topic"), on_demo_event, ctx.get());
        ctx->iface.events->subscribe(host, AGENTXX_SV("client.example_plugin.hello"), on_client_hello, ctx.get());

        // 4. 能力
        if (ctx->iface.capabilities && ctx->iface.capabilities->register_capability) {
            ctx->iface.capabilities->register_capability(host, AGENTXX_SV("example.demo"));
        }

        // 5. 提示词读写
        if (ctx->iface.prompt && ctx->iface.prompt->get_prompt && ctx->iface.prompt->set_prompt) {
            char* full = ctx->iface.prompt->get_prompt(host);
            if (full) {
                std::string prompt{full};
                host->vtable->free(full);
                if (prompt.find("\"example_echo\"") == std::string::npos) {
                    const char* promptJson
                        = R"({"toolPrompt":{"example_echo":{"depict":"Echo the input arguments back as JSON (example plugin tool).","args":{}}}})";
                    ctx->iface.prompt->set_prompt(host, AGENTXX_SV(promptJson));
                }
            }
        }

        ctx->log.info("example plugin loaded");
        *plugin_ctx = ctx.release();
        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<AgentCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(agentGuardLogger(ctx), [&] {
        if (!ctx) {
            return;
        }
        delete ctx;
    });
}

/* =====================================================================
 * client 侧入口 (agentxx_client_*)
 * ===================================================================== */

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxClientPluginInfo* agentxx_client_get_info(void) {
    return agentxx::plugin_guard::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxClientPluginInfo* {
        static const AgentxxClientPluginInfo info{
            AGENTXX_CLIENT_PLUGIN_API_VERSION,
            AGENTXX_SV("example_plugin"),
            AGENTXX_SV("1.0.0"),
            AGENTXX_SV("Example client plugin: status item, panel, commands, event bridge"),
        };
        return &info;
    });
}

static std::string clientJsonEscape(const ClientCtx& ctx, std::string_view text) {
    if (ctx.iface.json && ctx.iface.json->json_escape) {
        char* esc = ctx.iface.json->json_escape(
            ctx.host, agentxx_plugin_sv(text.data(), text.size())
        );
        if (esc) {
            std::string s{esc};
            ctx.host->vtable->free(esc);
            return s;
        }
    }
    return fmt::format("\"{}\"", text);
}

static char* example_cmd_execute(void* ud, AgentxxPluginStringView args_json, char** error_out) {
    auto* ctxRaw = static_cast<ClientCtx*>(ud);
    return agentxx::plugin_guard::guardCall(
        clientGuardLogger(ctxRaw),
        nullptr,
        [&]() -> char* {
        auto* ctx = static_cast<ClientCtx*>(ud);
        if (!ctx || !ctx->host) {
            return nullptr;
        }
        std::string suffix;
        if (ctx->iface.json && ctx->iface.json->json_get_string) {
            char* text = ctx->iface.json->json_get_string(
                ctx->host, args_json, AGENTXX_SV("text")
            );
            if (text) {
                suffix = text;
                ctx->host->vtable->free(text);
            }
        }
        std::string text = "Hello from example plugin";
        if (!suffix.empty()) {
            text = fmt::format("{} ({})", text, suffix);
        }
        const std::string out = fmt::format(R"({{"action":"send","text":{}}})", clientJsonEscape(*ctx, text));
        return ctx->host->vtable->strdup(out.c_str());
    });
}

static char* example_toast_execute(void* ud, AgentxxPluginStringView args_json, char** error_out) {
    auto* ctxRaw = static_cast<ClientCtx*>(ud);
    return agentxx::plugin_guard::guardCall(
        clientGuardLogger(ctxRaw),
        nullptr,
        [&]() -> char* {
        auto* ctx = static_cast<ClientCtx*>(ud);
        if (!ctx || !ctx->host) {
            return nullptr;
        }
        char* argText = ctx->iface.json ? ctx->iface.json->json_get_string(
                                              ctx->host, args_json, AGENTXX_SV("text"))
                                         : nullptr;
        std::string text = argText && *argText ? argText : "toast from example plugin";
        if (argText) {
            ctx->host->vtable->free(argText);
        }
        const std::string out
            = fmt::format(R"({{"action":"toast","text":{},"level":1}})", clientJsonEscape(*ctx, text));
        return ctx->host->vtable->strdup(out.c_str());
    });
}

static void on_client_ready(AgentxxPluginStringView payload_json, void* ud) {
    (void)payload_json;
    auto* ctxRaw = static_cast<ClientCtx*>(ud);
    agentxx::plugin_guard::guardCallVoid(clientGuardLogger(ctxRaw), [&] {
        auto* ctx = ctxRaw;
        if (!ctx || !ctx->host) {
            return;
        }
        if (ctx->iface.log && ctx->iface.log->log) {
            ctx->iface.log->log(ctx->host, 2, AGENTXX_SV("client example: ready"));
        }
        if (ctx->iface.wire && ctx->iface.wire->send_plugin_data) {
            ctx->iface.wire->send_plugin_data(
                ctx->host,
                AGENTXX_SV("hello"),
                AGENTXX_SV(R"({"from":"client-example"})")
            );
        }
    });
}

static void on_client_turn_end(AgentxxPluginStringView payload_json, void* ud) {
    (void)payload_json;
    auto* ctxRaw = static_cast<ClientCtx*>(ud);
    agentxx::plugin_guard::guardCallVoid(clientGuardLogger(ctxRaw), [&] {
        auto* ctx = ctxRaw;
        if (!ctx || !ctx->host) {
            return;
        }
        ++ctx->turn_count;
        if (ctx->status_item && ctx->ui && ctx->ui->update_status_item) {
            const std::string json = fmt::format(
                R"({{"text":{}}})",
                clientJsonEscape(*ctx, fmt::format("turns: {}", ctx->turn_count))
            );
            ctx->ui->update_status_item(
                ctx->host,
                ctx->status_item,
                agentxx_plugin_sv(json.data(), json.size())
            );
        }
        if (ctx->info_section && ctx->ui && ctx->ui->update_info_section) {
            const std::string json = fmt::format(
                R"({{"items":[{{"kind":"text","text":{}}},{{"kind":"text","role":"hint","text":"Example Info section is live"}}]}})",
                clientJsonEscape(*ctx, fmt::format("Turns: {}", ctx->turn_count))
            );
            ctx->ui->update_info_section(ctx->host, ctx->info_section, AGENTXX_SV(json.c_str()));
        }
    });
}

static void on_client_plugin_data(AgentxxPluginStringView payload_json, void* ud) {
    auto* ctx = static_cast<ClientCtx*>(ud);
    if (!ctx || !ctx->host || !ctx->panel) {
        return;
    }
    if (!ctx->iface.json || !ctx->iface.json->json_get_string) {
        return;
    }
    char* plugin = ctx->iface.json->json_get_string(
        ctx->host, payload_json, AGENTXX_SV("plugin")
    );
    char* event = ctx->iface.json->json_get_string(
        ctx->host, payload_json, AGENTXX_SV("event")
    );
    char* data = ctx->iface.json->json_get_string(
        ctx->host, payload_json, AGENTXX_SV("data")
    );
    agentxx::plugin_guard::guardCallVoid(clientGuardLogger(ctx), [&] {
        std::string line = fmt::format("{}.{}", plugin ? plugin : "?", event ? event : "?");
        if (data && *data) {
            line = fmt::format("{}: {}", line, data);
        }
        const std::string json = fmt::format(
            R"({{"items":[{{"kind":"text","text":{}}},{{"kind":"badge","text":"updated"}}]}})",
            clientJsonEscape(*ctx, line)
        );
        if (ctx->ui && ctx->ui->update_panel) {
            ctx->ui->update_panel(ctx->host, ctx->panel, AGENTXX_SV(json.c_str()));
        }
    });
    if (plugin) {
        ctx->host->vtable->free(plugin);
    }
    if (event) {
        ctx->host->vtable->free(event);
    }
    if (data) {
        ctx->host->vtable->free(data);
    }
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_client_create(const AgentxxClientHost* host, void** plugin_ctx) {
    ClientCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
        [&raw](const char* msg) noexcept { clientGuardLogger(raw)(msg); },
        -1,
        [&]() -> int {
        if (!host || !host->vtable || !plugin_ctx) return -1;
        auto ctx = std::make_unique<ClientCtx>();
        ctx->host = host;
        ctx->iface = agentxx::plugin::ClientIfaces::query(host);
        ctx->ui = ctx->iface.ui;
        raw = ctx.get();

        ctx->status_item = ctx->ui && ctx->ui->register_status_item
                             ? ctx->ui->register_status_item(
                                 host,
                                 AGENTXX_SV("example_plugin.turns"),
                                 AGENTXX_SV(R"({"text":"turns: 0"})"),
                                 0,
                                 10
                             )
                             : nullptr;

        ctx->panel = ctx->ui && ctx->ui->register_panel
                       ? ctx->ui->register_panel(
                           host,
                           AGENTXX_SV("example_plugin.panel"),
                           AGENTXX_SV(R"({"title":"Example"})")
                       )
                       : nullptr;

        ctx->info_section = ctx->ui && ctx->ui->register_info_section
                              ? ctx->ui->register_info_section(
                                  host,
                                  AGENTXX_SV("example_plugin.info"),
                                  AGENTXX_SV(R"({"title":"Example Info"})")
                              )
                              : nullptr;

        if (!ctx->ui || !ctx->ui->register_command) {
            return -1;
        }
        if (ctx->ui->register_command(
                host,
                AGENTXX_SV("example"),
                AGENTXX_SV("Send a message from the example plugin"),
                example_cmd_execute,
                ctx.get()
            )
            != 0) {
            return -1;
        }
        if (ctx->ui->register_command(
                host,
                AGENTXX_SV("example_toast"),
                AGENTXX_SV("Show a toast from the example plugin"),
                example_toast_execute,
                ctx.get()
            )
            != 0) {
            return -1;
        }

        if (!ctx->iface.events || !ctx->iface.events->subscribe) {
            return -1;
        }
        if (!ctx->iface.events
                 ->subscribe(host, AGENTXX_CLIENT_EVT_READY, on_client_ready, ctx.get())) {
            return -1;
        }
        if (!ctx->iface.events
                 ->subscribe(host, AGENTXX_CLIENT_EVT_TURN_END, on_client_turn_end, ctx.get())) {
            return -1;
        }
        if (!ctx->iface.events
                 ->subscribe(host, AGENTXX_CLIENT_EVT_PLUGIN_DATA, on_client_plugin_data, ctx.get())) {
            return -1;
        }

        if (ctx->iface.log && ctx->iface.log->log) {
            ctx->iface.log->log(host, 2, AGENTXX_SV("example client plugin loaded"));
        }
        *plugin_ctx = ctx.release();
        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_client_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ClientCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(clientGuardLogger(ctx), [&] {
        if (!ctx || !ctx->host) {
            delete ctx;
            return;
        }
        if (ctx->ui && ctx->ui->unregister_command) {
            ctx->ui->unregister_command(ctx->host, AGENTXX_SV("example"));
            ctx->ui->unregister_command(ctx->host, AGENTXX_SV("example_toast"));
        }
        if (ctx->status_item && ctx->ui && ctx->ui->unregister_status_item) {
            ctx->ui->unregister_status_item(ctx->host, ctx->status_item);
            ctx->status_item = nullptr;
        }
        if (ctx->panel && ctx->ui && ctx->ui->unregister_panel) {
            ctx->ui->unregister_panel(ctx->host, ctx->panel);
            ctx->panel = nullptr;
        }
        if (ctx->info_section && ctx->ui && ctx->ui->unregister_info_section) {
            ctx->ui->unregister_info_section(ctx->host, ctx->info_section);
            ctx->info_section = nullptr;
        }
        if (ctx->iface.log && ctx->iface.log->log) {
            ctx->iface.log->log(
                ctx->host,
                2,
                AGENTXX_SV("example client plugin unloaded")
            );
        }
        delete ctx;
    });
}
