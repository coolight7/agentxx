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
 * 6. client 入口 (双端插件, agentxx_plugin_client_create)
 */
#include "agentxx/plugin/api/client_plugin_api.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "fmt/format.h"

#include <memory>
#include <string>
#include <string_view>

/* =====================================================================
 * 每实例上下文
 * ===================================================================== */

struct AgentCtx : public agentxx::plugin::PluginBase {};

struct ClientCtx {
    const AgentxxPluginHost*      host = nullptr;
    agentxx::plugin::ClientIfaces iface{};
    const AgentxxClientUiIface*   ui           = nullptr;
    AgentxxStatusItem*            status_item  = nullptr;
    AgentxxPanel*                 panel        = nullptr;
    AgentxxInfoSection*           info_section = nullptr;
    int                           turn_count   = 0;
};

static auto agentGuardLogger(AgentCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx && ctx->host && ctx->iface.log && ctx->iface.log->log) {
            agentxx::plugin::logTo(ctx->host, ctx->iface.log, 4, "example_plugin", msg);
        }
    };
}

static auto clientGuardLogger(ClientCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx && ctx->host && ctx->iface.log && ctx->iface.log->log) {
            agentxx::plugin::logTo(ctx->host, ctx->iface.log, 4, "example_plugin", msg);
        }
    };
}

/* ---------------- get_info ---------------- */

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
            static const AgentxxPluginInfo info{
                AGENTXX_PLUGIN_API_VERSION, 0,
                agentxx_plugin_sv_cstr("example_plugin"),
                agentxx_plugin_sv_cstr("1.0.0"),
                agentxx_plugin_sv_cstr("Example native plugin: echo tool, hook, event, capability"),
            };
            return &info;
        }
    );
}

/* ---------------- event handlers ---------------- */

static void AGENTXX_PLUGIN_CALL on_demo_event(const AgentxxPluginStringView*, void* ud) {
    auto* ctxRaw = static_cast<AgentCtx*>(ud);
    agentxx::plugin::guardCallVoid(agentGuardLogger(ctxRaw), [&] {
        auto* ctx = static_cast<AgentCtx*>(ud);
        if (ctx) {
            ctx->log.info("example event received");
        }
    });
}

static void AGENTXX_PLUGIN_CALL on_client_hello(const AgentxxPluginStringView*, void* ud) {
    auto* ctxRaw = static_cast<AgentCtx*>(ud);
    agentxx::plugin::guardCallVoid(agentGuardLogger(ctxRaw), [&] {
        auto* ctx = static_cast<AgentCtx*>(ud);
        if (ctx) {
            ctx->log.info("example received client hello event");
        }
    });
}

/* ---------------- entry / unload ---------------- */

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    AgentCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* msg) noexcept {
            agentGuardLogger(raw)(msg);
        },
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
            agentxx::plugin::fast_tool(
                *ctx,
                "example_echo",
                "Echo the input arguments back as JSON (example plugin tool).",
                R"({"type":"object","properties":{},"additionalProperties":true})",
                [](AgentCtx& c, std::string_view args, std::string_view tid) -> std::string {
                    return fmt::format(
                        R"({{"echo": {},"sessionId": {}}})",
                        args.empty() ? "{}" : args,
                        c.jsonEscape(tid)
                    );
                }
            );

            // 1.2 caller: 锚定 Task 协程互调工具 (tool + call_tool)
            agentxx::plugin::tool(
                *ctx,
                "example_caller",
                "Call example_echo via call_tool to demonstrate plugin interop.",
                R"({"type":"object","properties":{},"additionalProperties":true})",
                [](AgentCtx& c, std::string_view args, agentxx::plugin::OpCtl ctl
                ) -> agentxx::plugin::Task<std::string> {
                    std::string resp = co_await agentxx::plugin::call_tool(
                        c,
                        "example_echo",
                        args,
                        ctl.threadId
                    );
                    co_return fmt::format(R"({{"via_call_tool": {}}})", resp);
                }
            );

            // 1.3 sleeper: 锚定 Task 协程 sleep 工具 (tool + sleep)
            agentxx::plugin::tool(
                *ctx,
                "example_sleep",
                "Sleep duration_ms milliseconds then return (slow plugin tool).",
                R"({"type":"object","properties":{"durationMs":{"type":"integer"}}})",
                [](AgentCtx& c, std::string_view args, agentxx::plugin::OpCtl ctl
                ) -> agentxx::plugin::Task<std::string> {
                    int ms = 200;
                    try {
                        auto j = neograph::json::parse(args);
                        if (j.contains("durationMs") && j["durationMs"].is_number()) {
                            ms = j["durationMs"].get<int>();
                        }
                    } catch (...) {
                    }
                    co_await agentxx::plugin::sleep(c, ms > 0 ? ms : 0);
                    ctl.throw_if_cancelled();
                    co_return fmt::format(R"({{"slept_ms": {}}})", ms);
                }
            );

            // 2. 钩子 (agent_start)
            if (ctx->iface.hooks && ctx->iface.hooks->register_hook) {
                agentxx::plugin::hook(
                    *ctx,
                    AGENTXX_PLUGIN_HOOK_AGENT_START,
                    [](AgentCtx& c, AgentxxPluginHookPoint, std::string_view) {
                        c.log.info("example hook: agent_start fired");
                    }
                );
            }

            // 3. 事件订阅
            auto topic1 = agentxx_plugin_sv_cstr("demo.topic");
            ctx->iface.events->subscribe(host, &topic1, on_demo_event, ctx.get());
            auto topic2 = agentxx_plugin_sv_cstr("client.example_plugin.hello");
            ctx->iface.events->subscribe(host, &topic2, on_client_hello, ctx.get());

            // 4. 能力
            if (ctx->iface.capabilities && ctx->iface.capabilities->register_capability) {
                auto capSv = agentxx_plugin_sv_cstr("example.demo");
                ctx->iface.capabilities->register_capability(host, &capSv);
            }

            // 5. 提示词读写
            if (ctx->iface.prompt && ctx->iface.prompt->get_prompt
                && ctx->iface.prompt->set_prompt) {
                AgentxxPluginString full{nullptr, 0};
                ctx->iface.prompt->get_prompt(host, &full);
                if (full.data) {
                    std::string prompt(full.data, static_cast<size_t>(full.size));
                    agentxx_plugin_string_free(host, &full);
                    if (prompt.find("\"example_echo\"") == std::string::npos) {
                        const char* promptJson
                            = R"({"toolPrompt":{"example_echo":{"depict":"Echo the input arguments back as JSON (example plugin tool).","args":{}}}})";
                        auto promptSv = agentxx_plugin_sv_cstr(promptJson);
                        ctx->iface.prompt->set_prompt(host, &promptSv);
                    }
                }
            }

            ctx->log.info("example plugin loaded");
            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<AgentCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(agentGuardLogger(ctx), [&] {
        if (!ctx) {
            return;
        }
        delete ctx;
    });
}

/* =====================================================================
 * client 侧入口 (agentxx_client_*)
 * ===================================================================== */

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxClientPluginInfo* agentxx_plugin_client_get_info(void
) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxClientPluginInfo* {
            static const AgentxxClientPluginInfo info{
                AGENTXX_CLIENT_PLUGIN_API_VERSION, 0,
                agentxx_plugin_sv_cstr("example_plugin"),
                agentxx_plugin_sv_cstr("1.0.0"),
                agentxx_plugin_sv_cstr(
                    "Example client plugin: status item, panel, commands, event bridge"
                ),
            };
            return &info;
        }
    );
}

static std::string clientJsonEscape(const ClientCtx& ctx, std::string_view text) {
    if (ctx.iface.json && ctx.iface.json->json_escape) {
        AgentxxPluginString esc{nullptr, 0};
        auto textSv = agentxx_plugin_sv(text.data(), text.size());
        ctx.iface.json->json_escape(ctx.host, &textSv, &esc);
        if (esc.data) {
            std::string s(esc.data, static_cast<size_t>(esc.size));
            agentxx_plugin_string_free(ctx.host, &esc);
            return s;
        }
    }
    return fmt::format("\"{}\"", text);
}

static int32_t AGENTXX_PLUGIN_CALL example_cmd_execute(
    void*                          ud,
    const AgentxxPluginStringView* args_json,
    AgentxxPluginString*           actionOut,
    AgentxxPluginString*           errorOut
) {
    (void)errorOut;
    auto* ctxRaw = static_cast<ClientCtx*>(ud);
    return agentxx::plugin::guardCall(
        clientGuardLogger(ctxRaw),
        -1,
        [&]() -> int {
            auto* ctx = static_cast<ClientCtx*>(ud);
            if (!ctx || !ctx->host) {
                return -1;
            }
            std::string suffix;
            if (ctx->iface.json && ctx->iface.json->json_get_string && args_json) {
                auto argsSv = agentxx_plugin_sv(args_json->data ? args_json->data : "{}", args_json->size);
                auto keySv  = agentxx_plugin_sv_cstr("text");
                AgentxxPluginString text{nullptr, 0};
                ctx->iface.json->json_get_string(ctx->host, &argsSv, &keySv, &text);
                if (text.data) {
                    suffix.assign(text.data, static_cast<size_t>(text.size));
                    agentxx_plugin_string_free(ctx->host, &text);
                }
            }
            std::string text = "Hello from example plugin";
            if (!suffix.empty()) {
                text = fmt::format("{} ({})", text, suffix);
            }
            const std::string out
                = fmt::format(R"({{"action":"send","text":{}}})", clientJsonEscape(*ctx, text));
            auto outSv = agentxx_plugin_sv(out.data(), out.size());
            if (actionOut) {
                *actionOut = agentxx_plugin_string_from_sv(ctx->host, &outSv);
            }
            return 0;
        }
    );
}

static int32_t AGENTXX_PLUGIN_CALL example_toast_execute(
    void*                          ud,
    const AgentxxPluginStringView* args_json,
    AgentxxPluginString*           actionOut,
    AgentxxPluginString*           errorOut
) {
    (void)errorOut;
    auto* ctxRaw = static_cast<ClientCtx*>(ud);
    return agentxx::plugin::guardCall(
        clientGuardLogger(ctxRaw),
        -1,
        [&]() -> int {
            auto* ctx = static_cast<ClientCtx*>(ud);
            if (!ctx || !ctx->host) {
                return -1;
            }
            AgentxxPluginString argText{nullptr, 0};
            if (ctx->iface.json && ctx->iface.json->json_get_string && args_json) {
                auto argsSv = agentxx_plugin_sv(args_json->data ? args_json->data : "{}", args_json->size);
                auto keySv  = agentxx_plugin_sv_cstr("text");
                ctx->iface.json->json_get_string(ctx->host, &argsSv, &keySv, &argText);
            }
            std::string text = argText.data && argText.size > 0
                                   ? std::string(argText.data, static_cast<size_t>(argText.size))
                                   : "toast from example plugin";
            if (argText.data) {
                agentxx_plugin_string_free(ctx->host, &argText);
            }
            const std::string out = fmt::format(
                R"({{"action":"toast","text":{},"level":1}})",
                clientJsonEscape(*ctx, text)
            );
            auto outSv = agentxx_plugin_sv(out.data(), out.size());
            if (actionOut) {
                *actionOut = agentxx_plugin_string_from_sv(ctx->host, &outSv);
            }
            return 0;
        }
    );
}

static void AGENTXX_PLUGIN_CALL on_client_ready(const AgentxxPluginStringView* payload_json, void* ud) {
    (void)payload_json;
    auto* ctxRaw = static_cast<ClientCtx*>(ud);
    agentxx::plugin::guardCallVoid(clientGuardLogger(ctxRaw), [&] {
        auto* ctx = ctxRaw;
        if (!ctx || !ctx->host) {
            return;
        }
        if (ctx->iface.log && ctx->iface.log->log) {
            auto msgSv = agentxx_plugin_sv_cstr("client example: ready");
            ctx->iface.log->log(ctx->host, 2, &msgSv);
        }
        if (ctx->iface.wire && ctx->iface.wire->send_plugin_data) {
            auto evtSv = agentxx_plugin_sv_cstr("hello");
            auto paySv = agentxx_plugin_sv_cstr(R"({"from":"client-example"})");
            ctx->iface.wire->send_plugin_data(ctx->host, &evtSv, &paySv);
        }
    });
}

static void AGENTXX_PLUGIN_CALL on_client_turn_end(const AgentxxPluginStringView* payload_json, void* ud) {
    (void)payload_json;
    auto* ctxRaw = static_cast<ClientCtx*>(ud);
    agentxx::plugin::guardCallVoid(clientGuardLogger(ctxRaw), [&] {
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
            auto jsonSv = agentxx_plugin_sv(json.data(), json.size());
            ctx->ui->update_status_item(ctx->host, ctx->status_item, &jsonSv);
        }
        if (ctx->info_section && ctx->ui && ctx->ui->update_info_section) {
            const std::string json = fmt::format(
                R"({{"items":[{{"kind":"text","text":{}}},{{"kind":"text","role":"hint","text":"Example Info section is live"}}]}})",
                clientJsonEscape(*ctx, fmt::format("Turns: {}", ctx->turn_count))
            );
            auto jsonSv = agentxx_plugin_sv_cstr(json.c_str());
            ctx->ui->update_info_section(ctx->host, ctx->info_section, &jsonSv);
        }
    });
}

static void AGENTXX_PLUGIN_CALL on_client_plugin_data(const AgentxxPluginStringView* payload_json, void* ud) {
    auto* ctx = static_cast<ClientCtx*>(ud);
    if (!ctx || !ctx->host || !ctx->panel) {
        return;
    }
    if (!ctx->iface.json || !ctx->iface.json->json_get_string) {
        return;
    }
    AgentxxPluginString plugin{nullptr, 0};
    AgentxxPluginString event{nullptr, 0};
    AgentxxPluginString data{nullptr, 0};
    auto kPlugin = agentxx_plugin_sv_cstr("plugin");
    auto kEvent  = agentxx_plugin_sv_cstr("event");
    auto kData   = agentxx_plugin_sv_cstr("data");
    if (payload_json) {
        ctx->iface.json->json_get_string(ctx->host, payload_json, &kPlugin, &plugin);
        ctx->iface.json->json_get_string(ctx->host, payload_json, &kEvent, &event);
        ctx->iface.json->json_get_string(ctx->host, payload_json, &kData, &data);
    }
    agentxx::plugin::guardCallVoid(clientGuardLogger(ctx), [&] {
        std::string line = fmt::format("{}.{}", plugin.data ? plugin.data : "?", event.data ? event.data : "?");
        if (data.data && data.size > 0) {
            line = fmt::format("{}: {}", line, data.data);
        }
        const std::string json = fmt::format(
            R"({{"items":[{{"kind":"text","text":{}}},{{"kind":"badge","text":"updated"}}]}})",
            clientJsonEscape(*ctx, line)
        );
        if (ctx->ui && ctx->ui->update_panel) {
            auto jsonSv = agentxx_plugin_sv_cstr(json.c_str());
            ctx->ui->update_panel(ctx->host, ctx->panel, &jsonSv);
        }
    });
    if (plugin.data) {
        agentxx_plugin_string_free(ctx->host, &plugin);
    }
    if (event.data) {
        agentxx_plugin_string_free(ctx->host, &event);
    }
    if (data.data) {
        agentxx_plugin_string_free(ctx->host, &data);
    }
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_client_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    ClientCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* msg) noexcept {
            clientGuardLogger(raw)(msg);
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx   = std::make_unique<ClientCtx>();
            ctx->host  = host;
            ctx->iface = agentxx::plugin::ClientIfaces::query(host);
            ctx->ui    = ctx->iface.ui;
            raw        = ctx.get();

            auto sidSv = agentxx_plugin_sv_cstr("example_plugin.turns");
            auto initSv = agentxx_plugin_sv_cstr(R"({"text":"turns: 0"})");
            ctx->status_item = ctx->ui && ctx->ui->register_status_item
                                   ? ctx->ui->register_status_item(host, &sidSv, &initSv, 0, 10)
                                   : nullptr;

            auto pidSv = agentxx_plugin_sv_cstr("example_plugin.panel");
            auto ppropSv = agentxx_plugin_sv_cstr(R"({"title":"Example"})");
            ctx->panel = ctx->ui && ctx->ui->register_panel
                             ? ctx->ui->register_panel(host, &pidSv, &ppropSv)
                             : nullptr;

            auto iidSv = agentxx_plugin_sv_cstr("example_plugin.info");
            auto ipropSv = agentxx_plugin_sv_cstr(R"({"title":"Example Info"})");
            ctx->info_section = ctx->ui && ctx->ui->register_info_section
                                    ? ctx->ui->register_info_section(host, &iidSv, &ipropSv)
                                    : nullptr;

            if (!ctx->ui || !ctx->ui->register_command) {
                return -1;
            }
            auto cmd1NameSv = agentxx_plugin_sv_cstr("example");
            auto cmd1DescSv = agentxx_plugin_sv_cstr("Send a message from the example plugin");
            if (ctx->ui->register_command(host, &cmd1NameSv, &cmd1DescSv, example_cmd_execute, ctx.get())
                != 0) {
                return -1;
            }
            auto cmd2NameSv = agentxx_plugin_sv_cstr("example_toast");
            auto cmd2DescSv = agentxx_plugin_sv_cstr("Show a toast from the example plugin");
            if (ctx->ui->register_command(
                    host,
                    &cmd2NameSv,
                    &cmd2DescSv,
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
            if (!ctx->iface.events->subscribe(
                    host,
                    AGENTXX_CLIENT_EVT_TURN_END,
                    on_client_turn_end,
                    ctx.get()
                )) {
                return -1;
            }
            if (!ctx->iface.events->subscribe(
                    host,
                    AGENTXX_CLIENT_EVT_PLUGIN_DATA,
                    on_client_plugin_data,
                    ctx.get()
                )) {
                return -1;
            }

            if (ctx->iface.log && ctx->iface.log->log) {
                auto msgSv = agentxx_plugin_sv_cstr("example client plugin loaded");
                ctx->iface.log->log(host, 2, &msgSv);
            }
            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_client_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ClientCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(clientGuardLogger(ctx), [&] {
        if (!ctx || !ctx->host) {
            delete ctx;
            return;
        }
        if (ctx->ui && ctx->ui->unregister_command) {
            auto n1 = agentxx_plugin_sv_cstr("example");
            ctx->ui->unregister_command(ctx->host, &n1);
            auto n2 = agentxx_plugin_sv_cstr("example_toast");
            ctx->ui->unregister_command(ctx->host, &n2);
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
            auto msgSv = agentxx_plugin_sv_cstr("example client plugin unloaded");
            ctx->iface.log->log(ctx->host, 2, &msgSv);
        }
        delete ctx;
    });
}
