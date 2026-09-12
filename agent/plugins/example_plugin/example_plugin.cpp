/// example_plugin —— 一期示例插件 (C++ 实现, 基于 plugin_kit.h)
///
/// 演示能力:
/// 1. 工具注册:
///    - example_echo (快同步 fast_tool, io 线程直跑)
///    - example_caller (锚定 Task 协程, 经 call_tool 异步互调)
///    - example_sleep (锚定 Task 协程, 经 co_await sleep 精确唤醒)
///    - example_bridge (协程驱动桥样例: 宿主 timer + 宿主回调两个真实唤醒源)
///    - example_polled_timer (受控轮询样例: 插件本地 reactor 上的 asio steady_timer)
/// 2. 钩子: agent_start 钩子
/// 3. 事件: 订阅 plugin.demo.topic 与跨端事件
/// 4. 能力: 声明 capability "example.demo"
/// 5. 卸载: destroy 释放实例
/// 6. client 入口 (双端插件, agentxx_plugin_client_create)
#include "agentxx/plugin/api/client_plugin_api.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

/// =====================================================================
/// 每实例上下文
/// =====================================================================

struct AgentCtx : public agentxx::plugin::PluginBase {};

struct ClientCtx : public agentxx::plugin::ClientPluginBase {
    const AgentxxClientUiIface* ui           = nullptr;
    AgentxxStatusItem*          status_item  = nullptr;
    AgentxxPanel*               panel        = nullptr;
    AgentxxInfoSection*         info_section = nullptr;
    int                         turn_count   = 0;
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

/// ---------------- get_info ----------------

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
            static const AgentxxPluginInfo info{
                AGENTXX_PLUGIN_API_VERSION,
                0,
                agentxx::plugin::PluginStringView::fromCstr("example_plugin"),
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "Example native plugin: echo tool, hook, event, capability"
                ),
            };
            return &info;
        }
    );
}

/// ---------------- event handlers ----------------

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

/// ---------------- 实例生命周期 (create 构造 / start 注册 / stop 撤销) ----------------
///
/// 入口语义 (见 docs/zh-cn/design/plugins.md 生命周期小节):
/// - `create`: 只分配上下文、查询接口、初始化纯本地字段; 不提交任何运行时注册,
///   不启动不受托管的线程。
/// - `start`: 在宿主 IO 线程执行注册事务 (工具/hook/事件/能力/prompt); 失败时
///   返回 NULL + error, 宿主按"拒绝"处理并回滚已生效的注册。
/// - `stop`: 在实例停用/关闭时给出完成信号 (本插件没有自管线程/定时器)。
/// - `destroy`: 只释放本地内存; 不创建异步工作、不调用宿主注册接口。

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

            // create 只做构造与接口查询: 注册事务由 start 执行
            if (!ctx->iface.tools || !ctx->iface.tools->register_tool || !ctx->iface.events) {
                return -1;
            }
            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

/// 注册事务 (start 的实际内容): 任一步骤失败返回 -1, 由宿主回滚已生效的注册。
static int exampleAgentSetup(AgentCtx& ctx) {
    const AgentxxPluginHost* host = ctx.host;
    // 1.1 echo: 快同步内联工具 (fast_tool)
    agentxx::plugin::fast_tool(
        ctx,
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
        ctx,
        "example_caller",
        "Call example_echo via call_tool to demonstrate plugin interop.",
        R"({"type":"object","properties":{},"additionalProperties":true})",
        [](AgentCtx& c, std::string_view args, agentxx::plugin::OpCtl ctl
        ) -> agentxx::plugin::Task<std::string> {
            std::string resp
                = co_await agentxx::plugin::call_tool(c, "example_echo", args, ctl.threadId);
            co_return fmt::format(R"({{"via_call_tool": {}}})", resp);
        }
    );

    // 1.3 sleeper: 锚定 Task 协程 sleep 工具 (tool + sleep)
    agentxx::plugin::tool(
        ctx,
        "example_sleep",
        "Sleep duration_ms milliseconds then return (slow plugin tool).",
        R"({"type":"object","properties":{"durationMs":{"type":"integer"}}})",
        [](AgentCtx& c, std::string_view args, agentxx::plugin::OpCtl ctl
        ) -> agentxx::plugin::Task<std::string> {
            int ms = 200;
            try {
                auto j = agentxx::util::Json::parse(args);
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

    // 1.4 bridge: 协程驱动桥诊断 (plugin.md 样例)
    //
    // 这个工具演示驱动桥的两个"真实唤醒源":
    //   1. 宿主已有 scheduler timer 回调 (`co_await sleep`) —— 到期时 adapter 只把
    //      continuation 投递到本地执行器并唤醒, 下一步由下一次 host driver 推进;
    //   2. 宿主回调式异步接口 (`co_await call_tool`) —— 完成回调同样不重入插件协程。
    // 因此这里的循环既不是轮询、也不占用额外线程: 每一步都对应一次宿主驱动。
    agentxx::plugin::tool(
        ctx,
        "example_bridge",
        "Report coroutine bridge diagnostics (driver tickets/steps, host IO thread).",
        R"({"type":"object","properties":{"ticks":{"type":"integer"}}})",
        [](AgentCtx& c, std::string_view args, agentxx::plugin::OpCtl ctl
        ) -> agentxx::plugin::Task<std::string> {
            int ticks = 1;
            try {
                auto j = agentxx::util::Json::parse(args);
                if (j.contains("ticks") && j["ticks"].is_number()) {
                    ticks = j["ticks"].get<int>();
                }
            } catch (...) {
            }
            if (ticks < 1) {
                ticks = 1;
            }
            if (ticks > 32) {
                ticks = 32;
            }
            auto* bridge      = &c.bridge();
            bool  onIoAtStart = bridge->onHostIoThread();
            for (int i = 0; i < ticks; ++i) {
                // 宿主计时器适配: 到期 -> 投递 continuation + wake -> 下一次 driver 恢复
                co_await agentxx::plugin::sleep(c, 1);
                ctl.throw_if_cancelled();
            }
            // 宿主回调式互调: 完成回调同样只投递 + 唤醒
            std::string echo = co_await agentxx::plugin::call_tool(
                c,
                "example_echo",
                R"({"from":"example_bridge"})",
                ctl.threadId
            );
            co_return fmt::format(
                R"({{"driverAvailable": {},"onHostIoThread": {},"ticks": {},"calls": {}}})",
                bridge != nullptr ? "true" : "false",
                onIoAtStart ? "true" : "false",
                ticks,
                echo.empty() ? "{}" : echo
            );
        }
    );

    // 1.5 polled_timer: 受控轮询样例 (声明式 `polled_tool`)
    //
    // 业务体是 asio 协程, 等待的是**插件本地 reactor** 上的 `steady_timer`:
    // 这类等待没有"宿主可见唤醒源", 因此由桥以受控轮询推进 ——
    // - 有在途 polled 操作时才申请驱动请求 (空闲零开销);
    // - 有进展立刻续下一次请求, 无进展退避 10ms, 突发 256 步后强制让出;
    // - 宿主不提供 `coroutine_runtime`/`scheduler.sleep` 时自动降级为
    //   offload 工作线程 + 局部 io_context 跑完 (行为对业务体透明)。
    // 返回的 pumpOnStart 证明该协程确实以"受控轮询根"身份运行。
    agentxx::plugin::polled_tool(
        ctx,
        "example_polled_timer",
        "Wait ticks * intervalMs with an asio steady_timer on the plugin local reactor "
        "(controlled polling sample) and report the measured elapsed time.",
        R"({"type":"object","properties":{"ticks":{"type":"integer"},"intervalMs":{"type":"integer"}}})",
        [](AgentCtx&        c,
           std::string_view args,
           std::string_view,
           std::string_view,
           const AgentxxPluginCancelToken* cancel_token) -> asio::awaitable<std::string> {
            int ticks      = 3;
            int intervalMs = 20;
            try {
                auto j = agentxx::util::Json::parse(args);
                if (j.contains("ticks") && j["ticks"].is_number()) {
                    ticks = j["ticks"].get<int>();
                }
                if (j.contains("intervalMs") && j["intervalMs"].is_number()) {
                    intervalMs = j["intervalMs"].get<int>();
                }
            } catch (...) {
            }
            if (ticks < 1) {
                ticks = 1;
            }
            if (ticks > 64) {
                ticks = 64;
            }
            if (intervalMs < 1) {
                intervalMs = 1;
            }

            auto*      bridge      = &c.bridge();
            const bool pumpOnStart = bridge->isPumping();
            const bool onIoThread  = bridge->onHostIoThread();

            auto ex      = co_await asio::this_coro::executor;
            auto startAt = std::chrono::steady_clock::now();
            for (int i = 0; i < ticks; ++i) {
                asio::steady_timer timer(ex);
                timer.expires_after(std::chrono::milliseconds(intervalMs));
                co_await timer.async_wait(asio::use_awaitable);
                if (agentxx_plugin_cancel_is_requested(cancel_token)) {
                    throw agentxx::plugin::CancelledException("example_polled_timer cancelled");
                }
            }
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - startAt
            )
                                       .count();
            co_return fmt::format(
                R"({{"driverAvailable": {},"onHostIoThread": {},"pumpOnStart": {},"ticks": {},"intervalMs": {},"elapsedMs": {}}})",
                bridge != nullptr ? "true" : "false",
                onIoThread ? "true" : "false",
                pumpOnStart ? "true" : "false",
                ticks,
                intervalMs,
                elapsedMs
            );
        }
    );

    // 2. 钩子 (agent_start)
    if (ctx.iface.hooks && ctx.iface.hooks->register_hook) {
        agentxx::plugin::hook(
            ctx,
            AGENTXX_PLUGIN_HOOK_AGENT_START,
            [](AgentCtx& c, AgentxxPluginHookPoint, std::string_view) {
                c.log.info("example hook: agent_start fired");
            }
        );
    }

    // 3. 事件订阅
    auto topic1 = agentxx::plugin::PluginStringView::fromCstr("demo.topic");
    ctx.iface.events->subscribe(host, &topic1, on_demo_event, &ctx);
    auto topic2 = agentxx::plugin::PluginStringView::fromCstr("client.example_plugin.hello");
    ctx.iface.events->subscribe(host, &topic2, on_client_hello, &ctx);

    // 4. 能力
    if (ctx.iface.capabilities && ctx.iface.capabilities->register_capability) {
        auto capSv = agentxx::plugin::PluginStringView::fromCstr("example.demo");
        ctx.iface.capabilities->register_capability(host, &capSv);
    }

    // 5. 提示词读写
    if (ctx.iface.prompt && ctx.iface.prompt->get_prompt && ctx.iface.prompt->set_prompt) {
        AgentxxPluginString full{nullptr, 0};
        ctx.iface.prompt->get_prompt(host, &full);
        if (full.data) {
            std::string prompt(full.data, static_cast<size_t>(full.size));
            agentxx::plugin::PluginString::free(host, &full);
            if (prompt.find("\"example_echo\"") == std::string::npos) {
                const char* promptJson
                    = R"({"toolPrompt":{"example_echo":{"depict":"Echo the input arguments back as JSON (example plugin tool).","args":{}}}})";
                auto promptSv = agentxx::plugin::PluginStringView::fromCstr(promptJson);
                ctx.iface.prompt->set_prompt(host, &promptSv);
            }
        }
    }

    return 0;
}

static void* exampleAgentStart(
    AgentCtx&                          ctx,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error
) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_plugin start: notify required"
            );
        }
        return nullptr;
    }
    if (exampleAgentSetup(ctx) != 0) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_plugin start: registration transaction failed"
            );
        }
        return nullptr;
    }
    ctx.log.info("example plugin started");
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

static void*
    exampleAgentStop(AgentCtx&, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*) {
    // 本插件没有自管线程/定时器: stop 只给出完成信号。注册记录 (工具/hook/能力/
    // 订阅/prompt 贡献) 由宿主在 stop 后统一撤销, 这里不重复反注册, 避免与宿主
    // 的清理交叉。
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(AgentCtx, exampleAgentStart, exampleAgentStop)

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<AgentCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(agentGuardLogger(ctx), [&] {
        if (!ctx) {
            return;
        }
        delete ctx;
    });
}

/// =====================================================================
/// client 侧入口 (agentxx_client_*)
/// =====================================================================

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxClientPluginInfo* agentxx_plugin_client_get_info(void
) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxClientPluginInfo* {
            static const AgentxxClientPluginInfo info{
                AGENTXX_CLIENT_PLUGIN_API_VERSION,
                0,
                agentxx::plugin::PluginStringView::fromCstr("example_plugin"),
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
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
        auto textSv = agentxx::plugin::PluginStringView::from(text.data(), text.size());
        ctx.iface.json->json_escape(ctx.host, &textSv, &esc);
        if (esc.data) {
            std::string s(esc.data, static_cast<size_t>(esc.size));
            agentxx::plugin::PluginString::free(ctx.host, &esc);
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
    return agentxx::plugin::guardCall(clientGuardLogger(ctxRaw), -1, [&]() -> int {
        auto* ctx = static_cast<ClientCtx*>(ud);
        if (!ctx || !ctx->host) {
            return -1;
        }
        std::string suffix;
        if (ctx->iface.json && ctx->iface.json->json_get_string && args_json) {
            auto argsSv = agentxx::plugin::PluginStringView::from(
                args_json->data ? args_json->data : "{}",
                args_json->size
            );
            auto                keySv = agentxx::plugin::PluginStringView::fromCstr("text");
            AgentxxPluginString text{nullptr, 0};
            ctx->iface.json->json_get_string(ctx->host, &argsSv, &keySv, &text);
            if (text.data) {
                suffix.assign(text.data, static_cast<size_t>(text.size));
                agentxx::plugin::PluginString::free(ctx->host, &text);
            }
        }
        std::string text = "Hello from example plugin";
        if (!suffix.empty()) {
            text = fmt::format("{} ({})", text, suffix);
        }
        const std::string out
            = fmt::format(R"({{"action":"send","text":{}}})", clientJsonEscape(*ctx, text));
        auto outSv = agentxx::plugin::PluginStringView::from(out.data(), out.size());
        if (actionOut) {
            *actionOut = agentxx::plugin::PluginString::from(ctx->host, &outSv);
        }
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL example_toast_execute(
    void*                          ud,
    const AgentxxPluginStringView* args_json,
    AgentxxPluginString*           actionOut,
    AgentxxPluginString*           errorOut
) {
    (void)errorOut;
    auto* ctxRaw = static_cast<ClientCtx*>(ud);
    return agentxx::plugin::guardCall(clientGuardLogger(ctxRaw), -1, [&]() -> int {
        auto* ctx = static_cast<ClientCtx*>(ud);
        if (!ctx || !ctx->host) {
            return -1;
        }
        AgentxxPluginString argText{nullptr, 0};
        if (ctx->iface.json && ctx->iface.json->json_get_string && args_json) {
            auto argsSv = agentxx::plugin::PluginStringView::from(
                args_json->data ? args_json->data : "{}",
                args_json->size
            );
            auto keySv = agentxx::plugin::PluginStringView::fromCstr("text");
            ctx->iface.json->json_get_string(ctx->host, &argsSv, &keySv, &argText);
        }
        std::string text = argText.data && argText.size > 0
                               ? std::string(argText.data, static_cast<size_t>(argText.size))
                               : "toast from example plugin";
        if (argText.data) {
            agentxx::plugin::PluginString::free(ctx->host, &argText);
        }
        const std::string out = fmt::format(
            R"({{"action":"toast","text":{},"level":1}})",
            clientJsonEscape(*ctx, text)
        );
        auto outSv = agentxx::plugin::PluginStringView::from(out.data(), out.size());
        if (actionOut) {
            *actionOut = agentxx::plugin::PluginString::from(ctx->host, &outSv);
        }
        return 0;
    });
}

static void AGENTXX_PLUGIN_CALL
    on_client_ready(const AgentxxPluginStringView* payload_json, void* ud) {
    (void)payload_json;
    auto* ctxRaw = static_cast<ClientCtx*>(ud);
    agentxx::plugin::guardCallVoid(clientGuardLogger(ctxRaw), [&] {
        auto* ctx = ctxRaw;
        if (!ctx || !ctx->host) {
            return;
        }
        if (ctx->iface.log && ctx->iface.log->log) {
            auto msgSv = agentxx::plugin::PluginStringView::fromCstr("client example: ready");
            ctx->iface.log->log(ctx->host, 2, &msgSv);
        }
        if (ctx->iface.wire && ctx->iface.wire->send_plugin_data) {
            auto evtSv = agentxx::plugin::PluginStringView::fromCstr("hello");
            auto paySv
                = agentxx::plugin::PluginStringView::fromCstr(R"({"from":"client-example"})");
            ctx->iface.wire->send_plugin_data(ctx->host, &evtSv, &paySv);
        }
    });
}

static void AGENTXX_PLUGIN_CALL
    on_client_turn_end(const AgentxxPluginStringView* payload_json, void* ud) {
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
            auto jsonSv = agentxx::plugin::PluginStringView::from(json.data(), json.size());
            ctx->ui->update_status_item(ctx->host, ctx->status_item, &jsonSv);
        }
        if (ctx->info_section && ctx->ui && ctx->ui->update_info_section) {
            const std::string json = fmt::format(
                R"({{"items":[{{"kind":"text","text":{}}},{{"kind":"text","role":"hint","text":"Example Info section is live"}}]}})",
                clientJsonEscape(*ctx, fmt::format("Turns: {}", ctx->turn_count))
            );
            auto jsonSv = agentxx::plugin::PluginStringView::fromCstr(json.c_str());
            ctx->ui->update_info_section(ctx->host, ctx->info_section, &jsonSv);
        }
    });
}

static void AGENTXX_PLUGIN_CALL
    on_client_plugin_data(const AgentxxPluginStringView* payload_json, void* ud) {
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
    auto                kPlugin = agentxx::plugin::PluginStringView::fromCstr("plugin");
    auto                kEvent  = agentxx::plugin::PluginStringView::fromCstr("event");
    auto                kData   = agentxx::plugin::PluginStringView::fromCstr("data");
    if (payload_json) {
        ctx->iface.json->json_get_string(ctx->host, payload_json, &kPlugin, &plugin);
        ctx->iface.json->json_get_string(ctx->host, payload_json, &kEvent, &event);
        ctx->iface.json->json_get_string(ctx->host, payload_json, &kData, &data);
    }
    agentxx::plugin::guardCallVoid(clientGuardLogger(ctx), [&] {
        std::string line
            = fmt::format("{}.{}", plugin.data ? plugin.data : "?", event.data ? event.data : "?");
        if (data.data && data.size > 0) {
            line = fmt::format("{}: {}", line, data.data);
        }
        const std::string json = fmt::format(
            R"({{"items":[{{"kind":"text","text":{}}},{{"kind":"badge","text":"updated"}}]}})",
            clientJsonEscape(*ctx, line)
        );
        if (ctx->ui && ctx->ui->update_panel) {
            auto jsonSv = agentxx::plugin::PluginStringView::fromCstr(json.c_str());
            ctx->ui->update_panel(ctx->host, ctx->panel, &jsonSv);
        }
    });
    if (plugin.data) {
        agentxx::plugin::PluginString::free(ctx->host, &plugin);
    }
    if (event.data) {
        agentxx::plugin::PluginString::free(ctx->host, &event);
    }
    if (data.data) {
        agentxx::plugin::PluginString::free(ctx->host, &data);
    }
}

/// client 侧生命周期事务 (与 agent 侧对称):
/// - create 只构造上下文与接口查询;
/// - start 注册状态栏项/面板/Info 段落/命令/事件订阅;
/// - stop 撤销上述注册 (句柄置空, 可重复调用);
/// - destroy 只释放本地内存。
static int exampleClientSetup(ClientCtx& ctx) {
    const AgentxxPluginHost* host = ctx.host;
    ctx.ui                        = ctx.iface.ui;
    auto sidSv      = agentxx::plugin::PluginStringView::fromCstr("example_plugin.turns");
    auto initSv     = agentxx::plugin::PluginStringView::fromCstr(R"({"text":"turns: 0"})");
    ctx.status_item = ctx.ui && ctx.ui->register_status_item
                          ? ctx.ui->register_status_item(host, &sidSv, &initSv, 0, 10)
                          : nullptr;

    auto pidSv   = agentxx::plugin::PluginStringView::fromCstr("example_plugin.panel");
    auto ppropSv = agentxx::plugin::PluginStringView::fromCstr(R"({"title":"Example"})");
    ctx.panel    = ctx.ui && ctx.ui->register_panel ? ctx.ui->register_panel(host, &pidSv, &ppropSv)
                                                    : nullptr;

    auto iidSv       = agentxx::plugin::PluginStringView::fromCstr("example_plugin.info");
    auto ipropSv     = agentxx::plugin::PluginStringView::fromCstr(R"({"title":"Example Info"})");
    ctx.info_section = ctx.ui && ctx.ui->register_info_section
                           ? ctx.ui->register_info_section(host, &iidSv, &ipropSv)
                           : nullptr;

    if (!ctx.ui || !ctx.ui->register_command) {
        return -1;
    }
    auto cmd1NameSv = agentxx::plugin::PluginStringView::fromCstr("example");
    auto cmd1DescSv
        = agentxx::plugin::PluginStringView::fromCstr("Send a message from the example plugin");
    if (ctx.ui->register_command(host, &cmd1NameSv, &cmd1DescSv, example_cmd_execute, &ctx) != 0) {
        return -1;
    }
    auto cmd2NameSv = agentxx::plugin::PluginStringView::fromCstr("example_toast");
    auto cmd2DescSv
        = agentxx::plugin::PluginStringView::fromCstr("Show a toast from the example plugin");
    if (ctx.ui->register_command(host, &cmd2NameSv, &cmd2DescSv, example_toast_execute, &ctx)
        != 0) {
        return -1;
    }

    if (!ctx.iface.events || !ctx.iface.events->subscribe) {
        return -1;
    }
    if (!ctx.iface.events->subscribe(host, AGENTXX_CLIENT_EVT_READY, on_client_ready, &ctx)) {
        return -1;
    }
    if (!ctx.iface.events->subscribe(host, AGENTXX_CLIENT_EVT_TURN_END, on_client_turn_end, &ctx)) {
        return -1;
    }
    if (!ctx.iface.events
             ->subscribe(host, AGENTXX_CLIENT_EVT_PLUGIN_DATA, on_client_plugin_data, &ctx)) {
        return -1;
    }

    if (ctx.iface.log && ctx.iface.log->log) {
        auto msgSv = agentxx::plugin::PluginStringView::fromCstr("example client plugin loaded");
        ctx.iface.log->log(host, 2, &msgSv);
    }
    return 0;
}

static void* exampleClientStart(
    ClientCtx&                         ctx,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error
) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_plugin client start: notify required"
            );
        }
        return nullptr;
    }
    if (exampleClientSetup(ctx) != 0) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_plugin client start: registration transaction failed"
            );
        }
        return nullptr;
    }
    if (ctx.iface.log && ctx.iface.log->log) {
        auto msgSv = agentxx::plugin::PluginStringView::fromCstr("example client plugin started");
        ctx.iface.log->log(ctx.host, 2, &msgSv);
    }
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

static void*
    exampleClientStop(ClientCtx&, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*) {
    // UI 注册与事件订阅由宿主在本事务后统一撤销 (宿主记录里已保存), 这里只给
    // 完成信号; 插件自身没有线程/定时器需要回收。
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
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
            auto ctx = std::make_unique<ClientCtx>();
            ctx->init(host);
            ctx->ui = ctx->iface.ui;
            raw     = ctx.get();

            // create 只做构造与接口查询: UI 注册事务由 start 执行
            if (!ctx->iface.events || !ctx->iface.events->subscribe) {
                return -1;
            }
            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

AGENTXX_PLUGIN_CLIENT_LIFECYCLE_EXPORT(ClientCtx, exampleClientStart, exampleClientStop)

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_client_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ClientCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(clientGuardLogger(ctx), [&] {
        if (!ctx || !ctx->host) {
            delete ctx;
            return;
        }
        // UI 注册与事件订阅在 stop 事务后由宿主统一撤销 (见 exampleClientStop):
        // destroy 只释放本地内存, 不再调用宿主注册接口。
        if (ctx->iface.log && ctx->iface.log->log) {
            auto msgSv
                = agentxx::plugin::PluginStringView::fromCstr("example client plugin unloaded");
            ctx->iface.log->log(ctx->host, 2, &msgSv);
        }
        delete ctx;
    });
}
