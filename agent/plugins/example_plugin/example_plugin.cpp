/// example_plugin —— 一期示例插件 (C++ 实现, 基于 plugin_kit.h)
///
/// 演示能力:
/// 1. 工具注册:
///    - example_echo (同步体, 工作池线程上跑; 返回字符串 = 永远成功)
///    - example_fail_demo (返回 ToolResult 的对照: 失败靠状态, 不靠抛异常或文本前缀)
///    - example_caller (锚定 Task 协程, 经 call_tool 异步互调)
///    - example_sleep (锚定 Task 协程, 经 co_await sleep 精确唤醒)
///    - example_bridge (协程驱动桥样例: 主程序 timer + 主程序回调两个真实唤醒源)
///    - example_polled_timer (受控轮询样例: 插件本地 reactor 上的 asio steady_timer)
/// 2. 通知: agent_start 通知点
/// 3. 事件: 订阅 plugin.demo.topic 与跨端事件
/// 4. 能力: 声明 capability "example.demo"
/// 5. 卸载: destroy 释放实例
/// 6. client 入口 (双端插件, agentxx_client_plugin_create)
#include "agentxx/plugin/api/agentxx_broadcast_ids.h"
#include "agentxx/plugin/api/agentxx_point_ids.h"
#include "agentxx/plugin/api/client_plugin_api.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/agent_plugin_kit.h"
#include "agentxx/plugin/api/client_plugin_kit.h"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/json.h"

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

/// =====================================================================
/// 每实例上下文
/// =====================================================================

struct AgentCtx : public agentxx::plugin::PluginBase {};

/// client 侧示例面板监听的频道 (`plugin.<插件名>.<事件>`): 对端与本端都可发布
constexpr const char* kExampleProgressTopic = "plugin.example_plugin.progress";

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

extern "C" PLUGINXX_EXPORT int32_t agentxx_agent_plugin_get_info(PluginxxInfo* out) {
    return agentxx::plugin::detail::fillAgentPluginInfo(
        out,
        "example_plugin",
        "1.0.0",
        "Example native plugin: echo tool, hook, event, capability"
    );
}

/// ---------------- event handlers ----------------

/// ---------------- capability handlers ----------------

/// 能力 "example.demo" 的实现行: 同步完成, 回一段固定 JSON
/// - 能力没有独立的方法名, 要做什么看 `args_json`
static void* PLUGINXX_CALL on_demo_capability(
    void*,
    const PluginxxHost*,
    const PluginxxStringView*,
    const PluginxxStringView*,
    const PluginxxOperatorNotify* notify,
    PluginxxString*
) {
    if (notify && notify->done) {
        auto payload = agentxx::plugin::PluginStringView::fromCstr(
            R"_({"ok":true,"from":"example_plugin"})_"
        );
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &payload);
    }
    return nullptr; ///< 同步完成: 不返回句柄
}

static void PLUGINXX_CALL on_demo_event(const PluginxxStringView*, void* ud) {
    auto* ctxRaw = static_cast<AgentCtx*>(ud);
    agentxx::plugin::guardCallVoid(agentGuardLogger(ctxRaw), [&] {
        auto* ctx = static_cast<AgentCtx*>(ud);
        if (ctx) {
            ctx->log.info("example event received");
        }
    });
}

static void PLUGINXX_CALL on_client_hello(const PluginxxStringView*, void* ud) {
    auto* ctxRaw = static_cast<AgentCtx*>(ud);
    agentxx::plugin::guardCallVoid(agentGuardLogger(ctxRaw), [&] {
        auto* ctx = static_cast<AgentCtx*>(ud);
        if (ctx) {
            ctx->log.info("example received client hello event");
        }
    });
}

/// ---------------- 实例生命周期 (create = 构造 + 注册 / stop 撤销 / destroy 释放) ----------------
///
/// 入口语义 (见 docs/zh-cn/design/plugins.md 生命周期小节):
/// - `create`: 构造上下文并做注册事务 (旧版 `create` + `start` 合并); 同一实例第二次
///   调用 (禁用后再启用) 时复用已有上下文, 只重新注册。
/// - `stop`: 撤销自管资源 (本插件没有自管线程/定时器), 注册记录由主程序按挂载记录撤销。
/// - `destroy`: 只释放本地内存; 不创建异步工作、不调用主程序注册接口。

static void exampleAgentStart(AgentCtx&, const PluginxxOperatorNotify*, PluginxxString*);

extern "C" PLUGINXX_EXPORT void agentxx_agent_plugin_create(
    const PluginxxHost*           host,
    void**                        plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    if (!host || !host->vtable) {
        agentxx::plugin::detail::notifyLifecycleFailure(
            host,
            notify,
            err,
            "example_plugin: create requires host"
        );
        return;
    }
    agentxx::plugin::detail::createCtxEntry<AgentCtx>(
        host,
        plugin_ctx,
        notify,
        err,
        "example_plugin",
        [](AgentCtx& ctx) -> std::string {
            if (!ctx.iface.tools || !ctx.iface.tools->register_tool || !ctx.iface.broadcasts) {
                return "example_plugin: host has no tools/broadcasts interface";
            }
            return {};
        },
        exampleAgentStart
    );
}

/// 挂载事务 (mount): 工具/通知/订阅/能力/提示词 (**协程形态**)
/// - `ref` 为本次挂载的 agent 引用, 全部登记都记在该 agent 名下
/// - 每条登记都是“提交 + `co_await`”: 提交就已经生效, 等结果只为了拿到失败原因
static pluginxx::Task<void>
    exampleAgentSetup(AgentCtx& ctx, agentxx::plugin::AgentRef ref) {
    // 1.1 echo: 最简单的同步体工具
    co_await agentxx::plugin::tool(
         ctx,
         ref,
        "example_echo",
        "Echo the input arguments back as JSON (example plugin tool).",
        R"_({"type":"object","properties":{},"additionalProperties":true})_",
        [](AgentCtx& c, std::string_view args, std::string_view tid) -> std::string {
            return fmt::format(
                R"_({{"echo": {},"sessionId": {}}})_",
                args.empty() ? "{}" : args,
                c.jsonEscape(tid)
            );
        }
    );

    // 1.1b fail_demo: 返回带状态的结果 (ToolResult)
    // - `ToolResult::fail(错误内容)` = 执行完成但失败: 错误内容只写正文,
    //   不加 `[Error: ...]` 前缀 (主程序统一排版)
    // - `ToolResult::ok(文本)` 与直接返回字符串等价
    // - 与 example_echo 对照: 一个返回字符串 (永远成功), 一个能表达失败
    co_await agentxx::plugin::tool(
         ctx,
         ref,
        "example_fail_demo",
        "Demonstrate returning a failed tool result without throwing (pass fail=true).",
        R"_({"type":"object","properties":{"fail":{"type":"boolean"}}})_",
        [](std::string_view args) -> agentxx::plugin::ToolResult {
            const bool shouldFail = args.find("\"fail\":true") != std::string_view::npos
                                    || args.find("\"fail\": true") != std::string_view::npos;
            if (shouldFail) {
                return agentxx::plugin::ToolResult::fail(
                    "example_fail_demo was asked to fail (this text is the whole error content)"
                );
            }
            return agentxx::plugin::ToolResult::ok(R"_({"ok":true})_");
        }
    );

    // 1.2 caller: 锚定 Task 协程互调工具 (tool + call_tool)
    co_await agentxx::plugin::tool(
         ctx,
         ref,
        "example_caller",
        "Call example_echo via call_tool to demonstrate plugin interop.",
        R"_({"type":"object","properties":{},"additionalProperties":true})_",
        [ref](AgentCtx& c, std::string_view args, agentxx::plugin::OpCtl ctl
        ) -> agentxx::plugin::Task<std::string> {
            std::string resp = co_await agentxx::plugin::call_tool(
                c,
                ref,
                "example_echo",
                args,
                ctl.scopeKey
            );
            co_return fmt::format(R"_({{"via_call_tool": {}}})_", resp);
        }
    );

    // 1.3 sleeper: 锚定 Task 协程 sleep 工具 (tool + sleep)
    co_await agentxx::plugin::tool(
         ctx,
         ref,
        "example_sleep",
        "Sleep duration_ms milliseconds then return (slow plugin tool).",
        R"_({"type":"object","properties":{"durationMs":{"type":"integer"}}})_",
        [ref](AgentCtx& c, std::string_view args, agentxx::plugin::OpCtl ctl
        ) -> agentxx::plugin::Task<std::string> {
            int ms = 200;
            try {
                auto j = utilxx_base::Json::parse(args);
                if (j.contains("durationMs") && j["durationMs"].is_number()) {
                    ms = j["durationMs"].get<int>();
                }
            } catch (...) {
            }
            co_await agentxx::plugin::sleep(c, ms > 0 ? ms : 0);
            ctl.throw_if_canceled();
            co_return fmt::format(R"_({{"slept_ms": {}}})_", ms);
        }
    );

    // 1.4 bridge: 协程驱动桥诊断 (plugin.md 样例)
    //
    // 这个工具演示驱动桥的两个"真实唤醒源":
    //   1. 主程序已有 scheduler timer 回调 (`co_await sleep`) —— 到期时 adapter 只把
    //      continuation 投递到本地执行器并唤醒, 下一步由下一次 host driver 推进;
    //   2. 主程序回调式异步接口 (`co_await call_tool`) —— 完成回调同样不重入插件协程。
    // 因此这里的循环既不是轮询、也不占用额外线程: 每一步都对应一次主程序驱动。
    co_await agentxx::plugin::tool(
         ctx,
         ref,
        "example_bridge",
        "Report coroutine bridge diagnostics (driver tickets/steps, host IO thread).",
        R"_({"type":"object","properties":{"ticks":{"type":"integer"}}})_",
        [ref](AgentCtx& c, std::string_view args, agentxx::plugin::OpCtl ctl
        ) -> agentxx::plugin::Task<std::string> {
            int ticks = 1;
            try {
                auto j = utilxx_base::Json::parse(args);
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
                // 主程序计时器适配: 到期 -> 投递 continuation + wake -> 下一次 driver 恢复
                co_await agentxx::plugin::sleep(c, 1);
                ctl.throw_if_canceled();
            }
            // 主程序回调式互调: 完成回调同样只投递 + 唤醒
            std::string echo = co_await agentxx::plugin::call_tool(
                c,
                ref,
                "example_echo",
                R"_({"from":"example_bridge"})_",
                ctl.scopeKey
            );
            co_return fmt::format(
                R"_({{"driverAvailable": {},"onHostIoThread": {},"ticks": {},"calls": {}}})_",
                bridge != nullptr ? "true" : "false",
                onIoAtStart ? "true" : "false",
                ticks,
                echo.empty() ? "{}" : echo
            );
        }
    );

    // 1.5 polled_timer: asio 协程体样例 (`tool` 的 awaitable 形态)
    //
    // 业务体是 `asio::awaitable`, 等待的是**插件本地 reactor** 上的 `steady_timer`:
    // 这类等待没有"主程序可见唤醒源", 因此由桥以受控轮询推进 ——
    // - 有未完成的 polled 操作时才申请驱动请求 (空闲零开销);
    // - 有进展立刻续下一次请求, 无进展退避 10ms, 突发 256 步后强制让出;
    // - 主程序不提供 `scheduler.request_driver`/`scheduler.sleep` 时自动降级为
    //   offload 工作线程 + 局部 io_context 跑完 (行为对业务体透明)。
    // 返回的 pumpOnStart 证明该协程确实以"受控轮询根"身份运行。
    co_await agentxx::plugin::tool(
         ctx,
         ref,
        "example_polled_timer",
        "Wait ticks * intervalMs with an asio steady_timer on the plugin local reactor "
        "(controlled polling sample) and report the measured elapsed time.",
        R"_({"type":"object","properties":{"ticks":{"type":"integer"},"intervalMs":{"type":"integer"}}})_",
        [](AgentCtx&        c,
           std::string_view args,
           std::string_view,
           std::string_view,
           const PluginxxCancelToken* cancel_token) -> asio::awaitable<std::string> {
            int ticks      = 3;
            int intervalMs = 20;
            try {
                auto j = utilxx_base::Json::parse(args);
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
                if (pluginxx_cancel_is_requested(cancel_token)) {
                    throw agentxx::plugin::CanceledException("example_polled_timer Canceled");
                }
            }
            const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - startAt
            )
                                       .count();
            co_return fmt::format(
                R"_({{"driverAvailable": {},"onHostIoThread": {},"pumpOnStart": {},"ticks": {},"intervalMs": {},"elapsedMs": {}}})_",
                bridge != nullptr ? "true" : "false",
                onIoThread ? "true" : "false",
                pumpOnStart ? "true" : "false",
                ticks,
                intervalMs,
                elapsedMs
            );
        }
    );

    // 2. 通知型功能埋点: 插件"想知道某个点发生了"就订阅同名广播频道
    //    (已经不再有"处理器登记"这回事; 序号已换成点 id)
    {
        co_await agentxx::plugin::subscribePluginBroadcast(
            ctx,
            ref.ptr(),
            agentxx::plugin::points::ToolEnd,
            on_demo_event, ///< 与自己的频道共用同一个处理器 (只打印载荷)
            &ctx
        );
    }
    // 3. 事件订阅 (读侧可跨族, 但必须是全名)
    //    自己的频道要先声明再订阅 (声明是显式动作; 主程序频道由主程序声明)
    const char* demoChannel = "plugin.example_plugin.demo";
    co_await agentxx::plugin::declarePluginBroadcast(ctx, demoChannel);
    co_await agentxx::plugin::subscribePluginBroadcast(
        ctx,
        ref.ptr(),
        demoChannel,
        on_demo_event,
        &ctx
    );
    // client 插件上行: client 侧自己的族 `agentxx.client.plugin.<插件名>.<事件>`
    co_await agentxx::plugin::subscribePluginBroadcast(
        ctx,
        ref.ptr(),
        "agentxx.client.plugin.example_plugin.hello",
        on_client_hello,
        &ctx
    );

    // 4. 能力 (声明 + 实现行)
    if (ctx.iface.capabilities && ctx.iface.capabilities->register_capability) {
        auto capSv = agentxx::plugin::PluginStringView::fromCstr("example.demo");
        PluginxxCapabilitySpec spec{};
        spec.version     = PLUGINXX_API_VERSION;
        spec.struct_size = sizeof(PluginxxCapabilitySpec);
        spec.id          = capSv;
        spec.impls       = PLUGINXX_CAPABILITY_IMPLS_MANY;
        spec.merge       = PLUGINXX_CAPABILITY_MERGE_FIRST;
        spec.phases      = PLUGINXX_CAPABILITY_PHASE_MASK_IMPL;
        spec.phase       = PLUGINXX_CAPABILITY_PHASE_IMPL;
        spec.run_start   = &on_demo_capability;
        // 作用域: 挂载引用 = 这一行只对该作用域生效 (空 = 进程级)
        // 表成员是异步契约: 登记结论只有结果里有 (失败不影响挂载, 只记日志)
        auto capResult = co_await agentxx::plugin::tableOp(
            ctx,
            [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return ctx.iface.capabilities->register_capability(
                    ctx.host,
                    ref.ptr(),
                    &spec,
                    cb,
                    ud,
                    err
                );
            }
        );
        if (!capResult.ok()) {
            ctx.log.warn(
                fmt::format(
                    "example_plugin: register capability failed: {}",
                    capResult.accepted() ? std::string(capResult.text()) : capResult.startError
                )
            );
        }
    }

    // 5. 提示词读写
    if (ctx.iface.prompt && ctx.iface.prompt->get_prompt && ctx.iface.prompt->set_prompt) {
        auto promptResult = co_await agentxx::plugin::promptJson(ctx, ref.ptr());
        if (promptResult.ok() && !promptResult.text().empty()) {
            if (promptResult.text().find("\"example_echo\"") == std::string_view::npos) {
                const char* promptJson
                    = R"_({"toolPrompt":{"example_echo":{"depict":"Echo the input arguments back as JSON (example plugin tool).","args":{}}}})_";
                co_await agentxx::plugin::setPromptJson(ctx, ref.ptr(), promptJson);
            }
        }
    }

    co_return;
}

static void
    exampleAgentStart(AgentCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString* error) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_plugin start: notify required"
            );
        }
        return;
    }
    // 进程级启动: 本插件没有自管线程/定时器, 注册全在挂载阶段做
    ctx.log.info("example plugin started");
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
}

static void* exampleAgentMount(
    AgentCtx&                          ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error
) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_plugin mount: notify required"
            );
        }
        return nullptr;
    }
    if (!ctx.iface.tasks || !ctx.iface.tasks->register_task) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_plugin mount: host has no pluginxx.tasks (spawn unavailable)"
            );
        }
        return nullptr;
    }
    auto later = agentxx::plugin::finishLater(notify);
    // 注册事务交给协程: 工具/订阅/能力/提示词逐条"提交 + co_await", 全部生效后再放行挂载
    const agentxx::plugin::AgentRef agentRef(*spec);
    ctx.spawn([later, agentRef](AgentCtx& c, agentxx::plugin::OpCtl&) -> pluginxx::Task<void> {
        try {
            co_await exampleAgentSetup(c, agentRef);
            later.ok();
        } catch (const std::exception& e) {
            later.failed(e.what());
        }
        co_return;
    });
    return later.provider();
}

static void exampleAgentUnmount(
    AgentCtx&, void*, const PluginxxOperatorNotify* notify, PluginxxString*
) {
    // 工具/通知/订阅/提示词由主程序按挂载记录撤销
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
}

static void exampleAgentStop(AgentCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    // 本插件没有自管线程/定时器: stop 只给出完成信号。注册记录 (工具/hook/能力/
    // 订阅/prompt 贡献) 由主程序在 stop 后统一撤销, 这里不重复反注册, 避免与主程序
    // 的清理交叉。
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
}

AGENTXX_AGENT_PLUGIN_LIFECYCLE_EXPORT(
    AgentCtx,
    exampleAgentStop,
    exampleAgentMount,
    exampleAgentUnmount
)

extern "C" PLUGINXX_EXPORT void agentxx_agent_plugin_destroy(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    agentxx::plugin::detail::destroyCtxEntry<AgentCtx>(
        plugin_ctx,
        notify,
        err,
        [](AgentCtx* ctx) {
            if (!ctx) {
                return;
            }
            delete ctx;
        }
    );
}

/// =====================================================================
/// client 侧入口 (agentxx_client_*)
/// =====================================================================

extern "C" PLUGINXX_EXPORT int32_t agentxx_client_plugin_get_info(PluginxxInfo* out) {
    return agentxx::plugin::detail::fillClientPluginInfo(
        out,
        "example_plugin",
        "1.0.0",
        "Example client plugin: status item, panel, commands, event bridge"
    );
}

/// 字符串 → JSON 字符串字面量 (经主程序 agentxx.client.json 接口表; 结果含引号)
/// - 转义逻辑统一在 SDK (见 agentxx::plugin::jsonEscape), 此处仅固定本插件的调用形态
static std::string clientJsonEscape(const ClientCtx& ctx, std::string_view text) {
    return agentxx::plugin::jsonEscape(ctx.host, ctx.iface.json, text);
}

static int32_t PLUGINXX_CALL example_cmd_execute(
    void*                         ud,
    const PluginxxStringView*     args_json,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
) {
    (void)error_out;
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
            auto           keySv = agentxx::plugin::PluginStringView::fromCstr("text");
            PluginxxString text{nullptr, 0};
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
            = fmt::format(R"_({{"action":"send","text":{}}})_", clientJsonEscape(*ctx, text));
        // 同步完成: 动作 JSON 直接经本次操作的回调交回
        agentxx::plugin::notifyText(notify, out);
        return 0;
    });
}

static int32_t PLUGINXX_CALL example_toast_execute(
    void*                         ud,
    const PluginxxStringView*     args_json,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
) {
    (void)error_out;
    auto* ctxRaw = static_cast<ClientCtx*>(ud);
    return agentxx::plugin::guardCall(clientGuardLogger(ctxRaw), -1, [&]() -> int {
        auto* ctx = static_cast<ClientCtx*>(ud);
        if (!ctx || !ctx->host) {
            return -1;
        }
        PluginxxString argText{nullptr, 0};
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
            R"_({{"action":"toast","text":{},"level":1}})_",
            clientJsonEscape(*ctx, text)
        );
        // 同步完成: 动作 JSON 直接经本次操作的回调交回
        agentxx::plugin::notifyText(notify, out);
        return 0;
    });
}

static void PLUGINXX_CALL on_client_ready(const PluginxxStringView* payload_json, void* ud) {
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
        agentxx::plugin::clientSendPluginData(
            ctx->host,
            ctx->iface.wire,
            "agentxx.client.plugin.example_plugin.hello",
            R"_({"from":"client-example"})_"
        );
    });
}

static void PLUGINXX_CALL on_client_turn_end(const PluginxxStringView* payload_json, void* ud) {
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
                R"_({{"text":{}}})_",
                clientJsonEscape(*ctx, fmt::format("turns: {}", ctx->turn_count))
            );
            ctx->setStatusJson(ctx->status_item, json);
        }
        if (ctx->info_section && ctx->ui && ctx->ui->update_info_section) {
            // 组件项用描述层与 kit 组装 (不必手写 JSON 字符串与转义)
            std::vector<pluginxx::ui::Item> items;
            utilxx_base::Json               turnsText;
            turnsText["text"] = fmt::format("Turns: {}", ctx->turn_count);
            items.push_back(agentxx::ui::kit::text(turnsText));
            items.push_back(agentxx::ui::kit::hint({
                {"text", "Example Info section is live"}
            }));
            ctx->setInfoSectionItems(ctx->info_section, items);
        }
    });
}

/// 示例频道 (`plugin.example_plugin.progress`): 对端/本端发出的进度值
static void PLUGINXX_CALL on_client_plugin_data(const PluginxxStringView* payload_json, void* ud) {
    auto* ctx = static_cast<ClientCtx*>(ud);
    if (!ctx || !ctx->host || !ctx->panel) {
        return;
    }
    agentxx::plugin::guardCallVoid(clientGuardLogger(ctx), [&] {
        std::string line{kExampleProgressTopic};
        if (payload_json && payload_json->data && payload_json->size > 0) {
            line = fmt::format(
                "{}: {}",
                kExampleProgressTopic,
                std::string_view{payload_json->data, static_cast<size_t>(payload_json->size)}
            );
        }
        // 组件项用描述层与 kit 组装 (不必手写 JSON 字符串与转义)
        std::vector<pluginxx::ui::Item> items;
        utilxx_base::Json               lineText;
        lineText["text"] = line;
        items.push_back(agentxx::ui::kit::text(lineText));
        items.push_back(agentxx::ui::kit::badge({
            {"text", "updated"}
        }));
        if (ctx->ui && ctx->panel) {
            ctx->setPanelItems(ctx->panel, items);
        }
    });
}

/// client 侧注册事务 (协程形态): 逐条提交并等结果, 任何一条拒绝就整体失败
///
/// 跑在插件执行线程上 (`exampleClientStart` 里 `ctx.spawn`), 每条注册都是
/// `co_await clientTableOpAsync(...)`: 提交后不再阻塞线程, 完成经同步桥就地唤醒。
static pluginxx::Task<void> exampleClientSetupAsync(ClientCtx& ctx) {
    const PluginxxHost* host = ctx.host;
    const auto          ui   = ctx.iface.ui;
    ctx.ui                   = ui;
    if (!ui) {
        throw std::runtime_error("client ui iface is unavailable");
    }

    // 句柄类结果: 正文是主程序侧地址的十进制文本, 解析不到说明注册被拒
    constexpr std::string_view kStatusJson = R"_({"text":"turns: 0"})_";
    constexpr std::string_view kPanelJson  = R"_({"title":"Example"})_";
    constexpr std::string_view kInfoJson   = R"_({"title":"Example Info"})_";

    {
        const auto idSv   = agentxx::plugin::PluginStringView::fromCstr("example_plugin.turns");
        const auto jsonSv = agentxx::plugin::PluginStringView::from(kStatusJson.data(), kStatusJson.size());
        auto res = co_await agentxx::plugin::clientTableOpAsync(
            ctx,
            [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return ui->register_status_item(host, &idSv, &jsonSv, 0, 10, cb, ud, err);
            }
        );
        ctx.status_item = agentxx::plugin::clientHandleOf<AgentxxStatusItem>(res);
        if (!ctx.status_item) {
            throw std::runtime_error(fmt::format("register status item failed: {}", res.text()));
        }
    }
    {
        const auto idSv   = agentxx::plugin::PluginStringView::fromCstr("example_plugin.panel");
        const auto jsonSv = agentxx::plugin::PluginStringView::from(kPanelJson.data(), kPanelJson.size());
        auto res = co_await agentxx::plugin::clientTableOpAsync(
            ctx,
            [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return ui->register_panel(host, &idSv, &jsonSv, cb, ud, err);
            }
        );
        ctx.panel = agentxx::plugin::clientHandleOf<AgentxxPanel>(res);
        if (!ctx.panel) {
            throw std::runtime_error(fmt::format("register panel failed: {}", res.text()));
        }
    }
    {
        const auto idSv   = agentxx::plugin::PluginStringView::fromCstr("example_plugin.info");
        const auto jsonSv = agentxx::plugin::PluginStringView::from(kInfoJson.data(), kInfoJson.size());
        auto res = co_await agentxx::plugin::clientTableOpAsync(
            ctx,
            [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return ui->register_info_section(host, &idSv, &jsonSv, cb, ud, err);
            }
        );
        ctx.info_section = agentxx::plugin::clientHandleOf<AgentxxInfoSection>(res);
        if (!ctx.info_section) {
            throw std::runtime_error(fmt::format("register info section failed: {}", res.text()));
        }
    }

    // 命令回调是异步操作契约: 动作 JSON 经回调自己的 notify 交回 (本插件同步给).
    // 主程序无命令能力 (精简客户端) 时注册失败, 不影响其余展示能力.
    {
        const auto nameSv = agentxx::plugin::PluginStringView::fromCstr("example");
        const auto descSv
            = agentxx::plugin::PluginStringView::fromCstr("Send a message from the example plugin");
        auto res = co_await agentxx::plugin::clientTableOpAsync(
            ctx,
            [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return ui->register_command(
                    host,
                    &nameSv,
                    &descSv,
                    example_cmd_execute,
                    &ctx,
                    cb,
                    ud,
                    err
                );
            }
        );
        if (!res.ok()) {
            throw std::runtime_error(fmt::format("register command `example` failed: {}", res.text()));
        }
    }
    {
        const auto nameSv = agentxx::plugin::PluginStringView::fromCstr("example_toast");
        const auto descSv
            = agentxx::plugin::PluginStringView::fromCstr("Show a toast from the example plugin");
        auto res = co_await agentxx::plugin::clientTableOpAsync(
            ctx,
            [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return ui->register_command(
                    host,
                    &nameSv,
                    &descSv,
                    example_toast_execute,
                    &ctx,
                    cb,
                    ud,
                    err
                );
            }
        );
        if (!res.ok()) {
            throw std::runtime_error(fmt::format("register command `example_toast` failed: {}", res.text()));
        }
    }

    const auto broadcasts = ctx.iface.broadcasts;
    if (!broadcasts || !broadcasts->subscribe_broadcast) {
        throw std::runtime_error("broadcasts iface is unavailable");
    }
    struct SubSpec {
        std::string_view                                  id;
        void(PLUGINXX_CALL* handler)(const PluginxxStringView*, void*);
    };
    const SubSpec subs[] = {
        {agentxx::plugin::broadcasts::ClientReady, on_client_ready},
        {agentxx::plugin::broadcasts::ClientTurnEnd, on_client_turn_end},
        {kExampleProgressTopic,                    on_client_plugin_data },
    };
    for (const auto& sub : subs) {
        const auto idSv = agentxx::plugin::PluginStringView::from(sub.id.data(), sub.id.size());
        auto res = co_await agentxx::plugin::clientTableOpAsync(
            ctx,
            [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return broadcasts->subscribe_broadcast(
                    host,
                    nullptr,
                    &idSv,
                    sub.handler,
                    &ctx,
                    nullptr,
                    cb,
                    ud,
                    err
                );
            }
        );
        if (!res.ok()) {
            throw std::runtime_error(
                fmt::format("subscribe `{}` failed: {}", sub.id, res.text())
            );
        }
    }

    if (ctx.iface.log && ctx.iface.log->log) {
        auto msgSv = agentxx::plugin::PluginStringView::fromCstr("example client plugin loaded");
        ctx.iface.log->log(host, 2, &msgSv);
    }
    co_return;
}

static void exampleClientStart(
    ClientCtx&                    ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error
) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_plugin client start: notify required"
            );
        }
        return;
    }
    // 注册事务写成协程: 每条注册都是提交 + co_await, 全部生效后才交回成功
    auto later = agentxx::plugin::finishLater(notify);
    ctx.spawn([later](ClientCtx& c, pluginxx::OpCtl&) -> pluginxx::Task<void> {
        try {
            co_await exampleClientSetupAsync(c);
        } catch (const std::exception& e) {
            later.failed(e.what());
            co_return;
        }
        if (c.iface.log && c.iface.log->log) {
            auto msgSv
                = agentxx::plugin::PluginStringView::fromCstr("example client plugin started");
            c.iface.log->log(c.host, 2, &msgSv);
        }
        later.ok();
        co_return;
    });
}

static void exampleClientStop(ClientCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    // UI 注册与事件订阅由主程序在本事务后统一撤销 (主程序记录里已保存), 这里只给
    // 完成信号; 插件自身没有线程/定时器需要回收。
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
}

extern "C" PLUGINXX_EXPORT void agentxx_client_plugin_create(
    const PluginxxHost*           host,
    void**                        plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    if (!host || !host->vtable) {
        agentxx::plugin::detail::notifyLifecycleFailure(
            host,
            notify,
            err,
            "example_plugin: client create requires host"
        );
        return;
    }
    agentxx::plugin::detail::createCtxEntry<ClientCtx>(
        host,
        plugin_ctx,
        notify,
        err,
        "example_plugin",
        [](ClientCtx& ctx) -> std::string {
            ctx.ui = ctx.iface.ui;
            if (!ctx.iface.broadcasts || !ctx.iface.broadcasts->subscribe_broadcast) {
                return "example_plugin: host has no broadcasts interface";
            }
            return {};
        },
        exampleClientStart
    );
}

AGENTXX_CLIENT_PLUGIN_LIFECYCLE_EXPORT(ClientCtx, exampleClientStop)

extern "C" PLUGINXX_EXPORT void agentxx_client_plugin_destroy(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    agentxx::plugin::detail::destroyCtxEntry<ClientCtx>(
        plugin_ctx,
        notify,
        err,
        [](ClientCtx* ctx) {
            if (!ctx || !ctx->host) {
                delete ctx;
                return;
            }
            // UI 注册与事件订阅在 stop 事务后由主程序统一撤销 (见 exampleClientStop):
            // destroy 只释放本地内存, 不再调用主程序注册接口。
            if (ctx->iface.log && ctx->iface.log->log) {
                auto msgSv = agentxx::plugin::PluginStringView::fromCstr(
                    "example client plugin unloaded"
                );
                ctx->iface.log->log(ctx->host, 2, &msgSv);
            }
            delete ctx;
        }
    );
}
