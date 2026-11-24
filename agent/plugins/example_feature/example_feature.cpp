/// example_feature —— 功能埋点插件示例 (实现行 = 能力行, 不再有单独的接口表)
///
/// 演示一个插件在功能埋点体系里能做的两件事:
/// 1. **声明并实现自己的功能埋点** (`plugin.example_feature.beat`):
///    第一次 `providePoint` 就顺带声明了它 (`impls: many` + `merge: first` + 只开放
///    impl 阶段), 之后同 id 的其它实现行都排在一条链上;
/// 2. **给主程序的功能埋点登记实现行** (`agentxx.agent.context.countTokens`):
///    登记带本次挂载的作用域, 因此只对被挂载的这个 agent 生效; 摘除后自动回落
///    到核心实现。
///
/// 只想"知道功能埋点被触发了"的插件不需要登记实现, 订阅同名广播频道即可
/// (见 `example_event`)。
///
/// 另含一个异步形态的实现行 (`plugin.example_feature.slow`): 用 kit 协程原语
/// (`co_await sleep`) 等待, 由插件协程驱动桥推进 (与 tool 的写法一致)。
///
/// 还演示**订阅通知型功能埋点**: 订阅 `agentxx.agent.point.turn.start`, 只关心"发生了"。
#include "agentxx/plugin/api/agentxx_point_ids.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/agent_plugin_kit.h"
#include "utilxx_base/json.h"

#include <chrono>
#include <string>
#include <string_view>

/// =====================================================================
/// 每实例上下文 (多实例契约: 状态只放这里, 不用可变全局)
/// =====================================================================

struct FeatureCtx : public agentxx::plugin::PluginBase {
    /// 实现行被调用的次数 (只是演示: 行随挂载撤销, 不需要自己维护登记表)
    int counter = 0;
};

static auto featureGuardLogger(FeatureCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx && ctx->host && ctx->iface.log && ctx->iface.log->log) {
            agentxx::plugin::logTo(ctx->host, ctx->iface.log, 4, "example_feature", msg);
        }
    };
}

extern "C" PLUGINXX_EXPORT int32_t agentxx_agent_plugin_get_info(PluginxxInfo* out) {
    return agentxx::plugin::detail::fillAgentPluginInfo(
        out,
        "example_feature",
        "2.0.0",
        "Example plugin: declare points / provide impl rows"
    );
}

/// ---------------- 注册事务 (start) ----------------

static void featureAgentStart(FeatureCtx&, const PluginxxOperatorNotify*, PluginxxString*);

extern "C" PLUGINXX_EXPORT void agentxx_agent_plugin_create(
    const PluginxxHost*           host,
    void**                        plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    agentxx::plugin::detail::createCtxEntry<FeatureCtx>(
        host,
        plugin_ctx,
        notify,
        err,
        "example_feature",
        [](FeatureCtx& ctx) -> std::string {
            return {};
        },
        [](FeatureCtx& c, const PluginxxOperatorNotify* n, PluginxxString* e) {
            (void)featureAgentStart(c, n, e);
        }
    );
}


/// 挂载事务 (mount): 登记实现行与通知订阅 (**协程形态**)
/// - `ref` 为本次挂载的 agent 引用, 全部登记都记在该 agent 名下
/// - 每条登记都是“提交 + `co_await`”: 提交就已经生效, 等结果只为了拿到拒绝原因
static asio::awaitable<void>
    featureAgentSetup(FeatureCtx& ctx, agentxx::plugin::AgentRef ref) {
    using namespace agentxx::plugin;

    // 能力表缺失 (主程序太旧 / 清单未声明): 按"功能不可用"降级, 不影响加载
    if (!ctx.iface.capabilities || !ctx.iface.capabilities->register_capability) {
        ctx.log.info("example_feature: capabilities iface unavailable, plugin stays idle");
        co_return;
    }

    // 1. 自己的功能埋点: 同步实现行
    //    - 收到的是调用载荷 JSON: {point, args, request, caller, viaCall, identity, sessionId}
    //    - 返回值就是这次调用的结果正文; 想显式置空就回 `{"disable":true}`
    //    - `pointAnswer` 把业务 JSON 包成 `{"value": ...}` 形式 (与早期写法一致),
    //      已经带 `value` / `disable` 的会原样返回
    {
        auto r = co_await providePointAsync(
            ctx,
            ref,
            "plugin.example_feature.beat",
            [](FeatureCtx& c, std::string_view call) {
                ++c.counter;
                const auto in   = utilxx_base::Json::parse(call);
                const auto args = in.value<utilxx_base::Json>("args", utilxx_base::Json::object());
                auto       out  = utilxx_base::Json::object();
                out["bpm"]      = 128;
                out["path"]     = args.value<std::string>("path", "");
                out["beats"]    = utilxx_base::Json::array();
                out["confident"] = true;
                return pointAnswer(out.dump());
            }
        );
        if (!r.ok()) {
            // 挂载事务失败: 主程序按拒绝处理并回滚
            throw std::runtime_error{
                "example_feature: providePoint(beat) rejected: "
                + (r.accepted() ? std::string(r.text()) : r.startError)
            };
        }
    }

    // 2. 同一个点的异步实现行 (协程形态: 用 kit 原语等待, 不阻塞 IO 线程)
    //    - 协程的返回值就是回答本身, 用 `pointAnswer` 包一层 (与同步形态一致)
    //    - `value` / `disable` 是回答的保留键: 业务数据里要用同名字段时自己包一层
    {
        auto r = co_await providePointAsync(
            ctx,
            ref,
            "plugin.example_feature.slow",
            [](FeatureCtx& c, std::string_view call) -> asio::awaitable<std::string> {
                const auto in   = utilxx_base::Json::parse(call);
                const auto args = in.value<utilxx_base::Json>("args", utilxx_base::Json::object());
                // 等待 60ms: 这类等待让出驱动序列, 期间 IO 线程可继续处理其他工作
                co_await sleepAsync(c, 60);
                auto out       = utilxx_base::Json::object();
                out["counter"] = ++c.counter;
                out["input"]   = args.value<int64_t>("input", 1);
                co_return pointAnswer(out.dump());
            }
        );
        if (!r.ok()) {
            throw std::runtime_error{
                "example_feature: providePoint(slow) rejected: "
                + (r.accepted() ? std::string(r.text()) : r.startError)
            };
        }
    }

    // 3. 给主程序的功能埋点登记实现行 (固定 token 数, 便于测试断言)
    //    - 主程序点写全名; 行只对本挂载生效, 优先级默认 0
    //    - 点还没声明 (如裸 PluginManager 环境) 时登记被拒, 记一条日志继续
    {
        auto r = co_await providePointAsync(
            ctx,
            ref,
            agentxx::plugin::points::ContextCountTokens,
            [](FeatureCtx&, std::string_view) {
                return std::string{R"_({"value":{"tokens":424242}})_"};
            }
        );
        if (!r.ok()) {
            ctx.log.info(
                "example_feature: host point countTokens not declared yet: "
                + (r.accepted() ? std::string(r.text()) : r.startError)
            );
        }
    }

    // 4. 只想"知道某个点发生了"就不必登记实现: 订阅同名广播频道即可
    //    (通知型功能埋点 `agentxx.agent.point.*` 由主程序声明, 插件只订阅)
    //    - 一个插件可能同时挂在多个 agent 上: 载荷里带 `agentId`, 订阅侧自己过滤
    //    - 订阅跟着实例走: 禁用/卸载时由主程序自动退订
    if (ctx.iface.broadcasts && ctx.iface.broadcasts->subscribe_broadcast) {
        co_await subscribePluginBroadcastAsync(
            ctx,
            ref.ptr(),
            agentxx::plugin::points::TurnStart,
            [](const PluginxxStringView* payload, void* ud) {
                auto* c = static_cast<FeatureCtx*>(ud);
                if (c == nullptr) {
                    return;
                }
                const std::string_view text{
                    payload && payload->data ? payload->data : "",
                    payload ? static_cast<size_t>(payload->size) : 0
                };
                c->log.info("example_feature: turn.start " + std::string{text});
            },
            &ctx
        );
    }
    co_return;
}

static void featureAgentStart( 
    FeatureCtx&                   ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error
) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_feature start: notify required"
            );
        }
        return;
    }
    // 进程级启动: 本插件没有自管线程/定时器, 注册全在挂载阶段做
    ctx.log.info("example_feature started");
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return;
}

static void* featureAgentMount(
    FeatureCtx&                        ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error
) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_feature mount: notify required"
            );
        }
        return nullptr;
    }
    if (!ctx.iface.tasks || !ctx.iface.tasks->register_task) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_feature mount: host has no pluginxx.tasks (spawn unavailable)"
            );
        }
        return nullptr;
    }
    auto later = agentxx::plugin::finishLater(notify);
    // 注册事务交给协程: 实现行/订阅逐条"提交 + co_await", 全部生效后再放行挂载
    const agentxx::plugin::AgentRef agentRef(*spec);
    ctx.spawn([later, agentRef](FeatureCtx& c, agentxx::plugin::OpCtl&) -> asio::awaitable<void> {
        try {
            co_await featureAgentSetup(c, agentRef);
            c.log.info("example_feature mounted");
            later.ok();
        } catch (const std::exception& e) {
            later.failed(e.what());
        }
        co_return;
    });
    return later.provider();
}

static void featureAgentUnmount(
    FeatureCtx&, void*, const PluginxxOperatorNotify* notify, PluginxxString*
) {
    // 实现行与订阅由主程序按挂载记录撤销
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return;
}

static void    featureAgentStop(FeatureCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    // 反注册由主程序在 stop 后统一撤销 (实现行随实例走), 这里只给完成信号
    ctx.log.info("example_feature stopped");
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return;
}

AGENTXX_AGENT_PLUGIN_LIFECYCLE_EXPORT(
    FeatureCtx,
    featureAgentStop,
    featureAgentMount,
    featureAgentUnmount
)

extern "C" PLUGINXX_EXPORT void agentxx_agent_plugin_destroy(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               err
) {
    agentxx::plugin::detail::destroyCtxEntry<FeatureCtx>(
        plugin_ctx,
        notify,
        err,
        [](FeatureCtx* ctx) { delete ctx; }
    );
}
