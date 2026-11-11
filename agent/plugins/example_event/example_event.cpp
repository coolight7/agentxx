/// example_event —— 广播插件示例 (通用表 `agentxx.broadcasts`)
///
/// 演示插件在广播面上能做的五件事:
/// 1. **声明自己的频道**: `plugin.example_event.build` (进度流) 与
///    `plugin.example_event.ready` (就绪值), 都保留当前值;
/// 2. **发布值**: 属主可以发, 别的插件发会被主程序拒 (`publish: owner`);
/// 3. **订阅**: 一条订阅自己的进度频道 (带结束回调), 一条订阅主程序的轮次通知
///    (`agentxx.agent.round.start`) —— 后者演示"订阅主程序的频道只读";
/// 4. **读当前值**: 发出去的值留在频道里, 后订阅者与随时读都能拿到;
/// 5. **结束流**: 卸载时主程序按归属回收订阅, 再次启用按 `mount` 重新声明。
///
/// 与已删除的 `agentxx.agent.event` 表的区别: 那张表把"事件"记在事件中枢里, 谁要值谁来
/// 登记等待 (值被投递进会话); 广播面只做"某频道的值变了", 收到值之后怎么用由订阅方决定。
///
/// 挂载配置 (可选): `{"logRounds": true}` —— 每收到一次轮次通知写一条日志
/// (默认只在第一次写, 免得刷屏)。
#include "agentxx/plugin/api/agentxx_broadcast_ids.h"
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
    /// 订阅句柄 (由主程序在卸载时回收; 这里只记着便于自查)
    PluginxxSubscription* buildSub  = nullptr;
    PluginxxSubscription* roundSub  = nullptr;
    /// 收到过多少条通知
    int                   buildSeen = 0;
    int                   roundSeen = 0;
    bool                  logRounds = false;
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
                agentxx::plugin::PluginStringView::fromCstr("2.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "Example broadcast plugin: declare channels, publish, subscribe, read, end"
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

/// 订阅回调: 收到一条值
static void PLUGINXX_CALL onBuildValue(const PluginxxStringView* payload, void* ud) {
    auto* state = static_cast<EventMountState*>(ud);
    if (nullptr == state) {
        return;
    }
    ++state->buildSeen;
    (void)payload;
}

/// 订阅回调: 频道被结束
static void PLUGINXX_CALL onBuildEnd(const PluginxxStringView* reason, void* ud) {
    auto* state = static_cast<EventMountState*>(ud);
    if (nullptr == state) {
        return;
    }
    (void)reason;
}

/// 订阅回调: 主程序的轮次开始通知
static void PLUGINXX_CALL onRoundStart(const PluginxxStringView* payload, void* ud) {
    auto* state = static_cast<EventMountState*>(ud);
    if (nullptr == state) {
        return;
    }
    ++state->roundSeen;
    (void)payload;
}

/// 挂载事务: 声明频道、发布初始值、订阅频道
static int eventAgentSetup(EventCtx& ctx, EventMountState& state) {
    using namespace agentxx::plugin;
    const auto* bc = ctx.iface.broadcasts;
    if (nullptr == bc || nullptr == bc->declare_broadcast || nullptr == bc->publish_broadcast) {
        ctx.log.info("example_event: broadcasts iface unavailable, plugin stays idle");
        return 0;
    }

    // 1. 声明两条自己的频道: 都保留当前值 (后订阅者能立刻拿到最后一条)
    if (!declarePluginBroadcast(ctx.host, bc, "plugin.example_event.build", false, true)) {
        ctx.log.info("example_event: declare example_event.build rejected");
        return -1;
    }
    if (!declarePluginBroadcast(ctx.host, bc, "plugin.example_event.ready", false, true)) {
        return -1;
    }

    // 2. 发布初始值
    utilxx_base::Json ready = utilxx_base::Json::object();
    ready["ok"]             = true;
    ready["plugin"]         = "example_event";
    if (!publishPluginBroadcast(ctx.host, bc, "plugin.example_event.ready", ready.dump(), false)) {
        return -1;
    }
    utilxx_base::Json step = utilxx_base::Json::object();
    step["step"]           = 1;
    step["total"]          = 2;
    if (!publishPluginBroadcast(ctx.host, bc, "plugin.example_event.build", step.dump(), false)) {
        return -1;
    }
    ++ctx.emitted;

    // 3. 读当前值: 有当前值的频道可以直接读, 不必等下一次发布
    if (bc->read_broadcast != nullptr) {
        PluginxxString out{nullptr, 0};
        auto           idSv = PluginStringView::fromCstr("plugin.example_event.build");
        if (bc->read_broadcast(ctx.host, &idSv, &out) == PLUGINXX_RESULT_OK
            && out.data != nullptr) {
            ctx.log.info("example_event: current value of the build channel read back");
            PluginString::free(ctx.host, &out);
        } else if (out.data != nullptr) {
            PluginString::free(ctx.host, &out);
        }
    }

    // 4. 订阅: 自己的进度频道 (带结束回调) + 主程序的轮次通知。
    //    轮次通知属主是主程序, 订阅它不需要先声明。
    const auto ref = state.ref.raw();
    if (bc->subscribe_broadcast_ex != nullptr) {
        auto       buildId = PluginStringView::fromCstr("plugin.example_event.build");
        state.buildSub     = bc->subscribe_broadcast_ex(
            ctx.host,
            &ref,
            &buildId,
            &onBuildValue,
            &onBuildEnd,
            &state,
            nullptr,
            nullptr
        );
        if (nullptr == state.buildSub) {
            ctx.log.info("example_event: subscribe the build channel rejected");
            return -1;
        }
    }
    if (bc->subscribe_broadcast != nullptr) {
        auto roundId = PluginStringView::from(
            broadcasts::AgentRoundStart.data(),
            broadcasts::AgentRoundStart.size()
        );
        state.roundSub = bc->subscribe_broadcast(
            ctx.host,
            &ref,
            &roundId,
            &onRoundStart,
            &state,
            nullptr,
            nullptr
        );
        // 轮次通知订阅不到不算失败: 主程序可能还没声明这个频道
        if (nullptr == state.roundSub) {
            ctx.log.info("example_event: round.start channel not available, skipped");
        }
    }

    ctx.log.info("example_event: channels declared, values published, subscriptions made");
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
    EventCtx&                          ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error
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
    if (nullptr == spec
        || !agentxx::plugin::specSizeOk<AgentxxPluginAgentMountSpec>(spec->struct_size)) {
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
    // 挂载配置: {"logRounds": true} —— 每收到一次轮次通知写一条日志
    if (spec->config_json.data != nullptr && spec->config_json.size > 0) {
        try {
            const auto parsed = utilxx_base::Json::parse(std::string_view{
                spec->config_json.data,
                static_cast<size_t>(spec->config_json.size)
            });
            if (parsed.is_object()) {
                state->logRounds = parsed.value("logRounds", false);
            }
        } catch (const std::exception&) {
            // 配置写坏了不影响加载: 只是不打开额外日志
            ctx.log.info("example_event: mount config is not valid JSON, skipped");
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
        delete state;
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
    // 频道与订阅由主程序按归属 (plugin:example_event) 整批回收, 这里只释放本地状态
    delete static_cast<EventMountState*>(mountUd);
    ctx.log.info("example_event unmounted");
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
