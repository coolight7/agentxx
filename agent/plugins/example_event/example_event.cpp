/// example_event —— 事件中枢插件示例 (agentxx.agent.event 接口表)
///
/// 演示一个插件在事件中枢里能做的三件事:
/// 1. **声明自己的事件源** (`plugin.example_event.build` 流式 /
///    `plugin.example_event.ready` 单值):
///    声明内容进中枢的记录表, 模型用 `agentxx_event opt=list` 就能看到 (含说明文字);
/// 2. **发布值 / 结束流**: 模型用 `agentxx_event opt=watch` 等这些事件, 值到达后作为
///    一条以 `[Event] <名字>` 开头的输入进入会话;
/// 3. **替某个会话登记等待**: 挂载配置里给了 `sessionId` 时, 插件替那个会话等
///    `plugin.example_event.build` 的每条值 (值到达后自动变成那个会话的输入)。
///
/// 归属: 事件与等待都记在本插件实例名下, 禁用/卸载时主程序整批回收, `stop` 里不必手清。
///
/// 挂载配置 (可选): `{"sessionId": "<会话 id>"}` —— 不给就不登记等待。
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "utilxx_base/json.h"

#include <memory>
#include <string>
#include <string_view>

/// =====================================================================
/// 每实例上下文 (多实例契约: 状态只放这里, 不用可变全局)
/// =====================================================================

struct EventCtx : public agentxx::plugin::PluginBase {
    /// 本实例发布过多少个值 (仅供日志与示例说明)
    int emitted = 0;
};

/// 一份挂载的状态 (拆挂载时主程序原样交回)
struct EventMountState {
    agentxx::plugin::AgentRef ref;
    std::string               sessionId;
    /// 替该会话登记的等待 id (空 = 没登记)
    std::string waitId;
};

static auto eventGuardLogger(EventCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx && ctx->host && ctx->iface.log && ctx->iface.log->log) {
            agentxx::plugin::logTo(ctx->host, ctx->iface.log, 4, "example_event", msg);
        }
    };
}

extern "C" PLUGINXX_EXPORT const PluginxxInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const PluginxxInfo* {
            static const PluginxxInfo info{
                PLUGINXX_API_VERSION,
                0,
                agentxx::plugin::PluginStringView::fromCstr("example_event"),
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "Example event-hub plugin: define events / publish values / register waits"
                ),
            };
            return &info;
        }
    );
}

extern "C" PLUGINXX_EXPORT int
    agentxx_plugin_agent_create(const PluginxxHost* host, void** plugin_ctx) {
    EventCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* msg) noexcept {
            eventGuardLogger(raw)(msg);
        },
        -1,
        [&]() -> int {
            if (!host || !plugin_ctx) {
                return -1;
            }
            auto* ctx = new EventCtx();
            raw       = ctx;
            ctx->init(host);
            *plugin_ctx = ctx;
            return 0;
        }
    );
}

/// 挂载事务: 声明事件源、发布初始值、替会话登记等待
static int eventAgentSetup(EventCtx& ctx, const EventMountState& state) {
    using namespace agentxx::plugin;
    const auto& ref = state.ref;

    // 事件中枢不可用 (主程序关了 `event.enable` 或版本更早): 按"功能不可用"降级,
    // 不影响插件加载 —— 事件的发布方不在了, 只是没人等得到值
    if (!hasEventHub(ctx)) {
        ctx.log.info("example_event: event hub iface unavailable, plugin stays idle");
        return 0;
    }

    // 1. 声明一条流式事件 (多次发布, 由 end_event 结束; 留 4 条历史值供后注册者回放)
    {
        EventDefineOptions opt;
        opt.name         = "plugin.example_event.build";
        opt.description  = "示例: 构建进度 (插件发布, 模型可 watch)";
        opt.historyLimit = 4;
        const auto rc    = defineEvent(ctx, ref, opt);
        if (rc == AGENTXX_PLUGIN_EVENT_ERR_UNSUPPORTED) {
            // 表在、中枢没有 (例如 `event.enable=false`): 同样按降级处理
            ctx.log.info("example_event: event hub is not enabled, plugin stays idle");
            return 0;
        }
        if (rc != AGENTXX_PLUGIN_EVENT_OK) {
            ctx.log.info("example_event: define example_event.build rejected");
            return -1; // start 事务失败: 主程序按拒绝处理并回滚
        }
    }
    // 2. 声明一条单值事件 (首个值到达后自动结束, 适合"就绪/已完成"这类一次性通知)
    {
        EventDefineOptions opt;
        opt.name        = "plugin.example_event.ready";
        opt.description = "示例: 就绪通知 (单值事件, 首个值到达后自动结束)";
        opt.single      = true;
        if (defineEvent(ctx, ref, opt) != AGENTXX_PLUGIN_EVENT_OK) {
            return -1;
        }
    }

    // 3. 发布值: 单值事件发一次就结束; 流式事件保持打开, 等后续步骤继续发
    utilxx_base::Json ready = utilxx_base::Json::object();
    ready["ok"]             = true;
    ready["plugin"]         = "example_event";
    if (emitEventValue(ctx, ref, "plugin.example_event.ready", ready) != AGENTXX_PLUGIN_EVENT_OK) {
        return -1;
    }
    utilxx_base::Json step = utilxx_base::Json::object();
    step["step"]           = 1;
    step["total"]          = 2;
    if (emitEventValue(ctx, ref, "plugin.example_event.build", step) != AGENTXX_PLUGIN_EVENT_OK) {
        return -1;
    }
    ++ctx.emitted;

    // 4. 替某个会话登记等待 (配置给了 sessionId 才做): 每条值都投给那个会话
    //    - delivery 默认 next-step: 会话在跑一轮时插到下一次模型调用之前,
    //      空闲时提升为新一轮输入
    if (!state.sessionId.empty()) {
        EventWaitOptions opt;
        opt.event         = "plugin.example_event.build";
        opt.mode          = "each";
        opt.sessionId     = state.sessionId;
        opt.label         = "示例插件替会话等的构建进度";
        opt.notifyOnTimeout = false;
        int32_t    code   = 0;
        const auto waitId = registerEventWait(ctx, ref, opt, nullptr, &code);
        if (waitId.empty()) {
            ctx.log.info("example_event: register_wait rejected");
            return -1;
        }
        ctx.log.info("example_event: registered a wait for the session");
    }

    // 5. 自查: 查一条事件的状态、列一遍事件与等待 (示例插件的自检习惯)
    ctx.log.info("example_event: event sources declared and initial values published");
    return 0;
}

static void* eventAgentStart(
    EventCtx&                     ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error
) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_event start: notify required"
            );
        }
        return nullptr;
    }
    // 进程级启动: 本插件没有自管线程/定时器, 注册全在挂载阶段做
    ctx.log.info("example_event started");
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* eventAgentMount(
    EventCtx&                         ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*     notify,
    PluginxxString*                   error
) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_event mount: notify required"
            );
        }
        return nullptr;
    }
    if (nullptr == spec || !agentxx::plugin::specSizeOk<AgentxxPluginAgentMountSpec>(
                              spec->struct_size
                          )) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_event mount: mount spec is null"
            );
        }
        return nullptr;
    }

    auto* state = new EventMountState();
    state->ref  = agentxx::plugin::AgentRef(*spec);
    // 挂载配置: {"sessionId": "..."} —— 给了就替那个会话登记一个等待
    if (spec->config_json.data != nullptr && spec->config_json.size > 0) {
        try {
            const auto parsed = utilxx_base::Json::parse(std::string_view{
                spec->config_json.data,
                static_cast<size_t>(spec->config_json.size)
            });
            if (parsed.is_object()) {
                state->sessionId = parsed.value("sessionId", std::string{});
            }
        } catch (const std::exception& e) {
            // 配置写坏了不影响加载: 只是不替会话登记等待
            ctx.log.info("example_event: mount config is not valid JSON, skipped");
            (void)e;
        }
    }

    if (eventAgentSetup(ctx, *state) != 0) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_event mount: registration transaction failed"
            );
        }
        return nullptr;
    }
    ctx.log.info("example_event mounted");
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return state;
}

static void* eventAgentUnmount(
    EventCtx&                     ctx,
    void*                         mountUd,
    const PluginxxOperatorNotify* notify,
    PluginxxString*
) {
    // 事件与等待由主程序按归属 (plugin:example_event) 整批回收; 这里只释放本地状态
    ctx.log.info("example_event unmounted");
    delete static_cast<EventMountState*>(mountUd);
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void*
    eventAgentStop(EventCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    ctx.log.info("example_event stopped");
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(
    EventCtx,
    eventAgentStart,
    eventAgentStop,
    eventAgentMount,
    eventAgentUnmount
)

extern "C" PLUGINXX_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<EventCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(eventGuardLogger(ctx), [&] {
        delete ctx;
    });
}
