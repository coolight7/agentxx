/// example_feature —— 扩展点插件示例 (实现行 = 能力行, 不再有单独的功能点接口表)
///
/// 演示一个插件在扩展点体系里能做的两件事:
/// 1. **声明并实现自己的扩展点** (`plugin.example_feature.beat`):
///    第一次 `providePoint` 就顺带声明了它 (`impls: many` + `merge: first` + 只开放
///    impl 阶段), 之后同 id 的其它实现行都排在一条链上;
/// 2. **给主程序的扩展点登记实现行** (`agentxx.agent.context.countTokens`):
///    登记带本次挂载的作用域, 因此只对被挂载的这个 agent 生效; 摘除后自动回落
///    到核心实现。
///
/// 只想"知道扩展点被触发了"的插件不需要登记实现, 订阅同名广播频道即可
/// (见 `example_event`)。
///
/// 另含一个异步形态的实现行 (`plugin.example_feature.slow`): 用 kit 协程原语
/// (`co_await sleep`) 等待, 由插件协程驱动桥推进 (与 tool 的写法一致)。
///
/// 还演示**钩子有序登记** (`agentxx.agent.hooks` 的表尾三成员): 以优先级 -10 登记一个
/// `AGENT_START` 处理器, 让它排在其它插件的钩子之前 (基础 `hook()` 只能按加载顺序排)。
#include "agentxx/plugin/api/agentxx_point_ids.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
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

extern "C" PLUGINXX_EXPORT const PluginxxInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const PluginxxInfo* {
            static const PluginxxInfo info{
                PLUGINXX_API_VERSION,
                0,
                agentxx::plugin::PluginStringView::fromCstr("example_feature"),
                agentxx::plugin::PluginStringView::fromCstr("2.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "Example plugin: declare points / provide impl rows"
                ),
            };
            return &info;
        }
    );
}

/// ---------------- 注册事务 (start) ----------------

extern "C" PLUGINXX_EXPORT int
    agentxx_plugin_agent_create(const PluginxxHost* host, void** plugin_ctx) {
    FeatureCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* msg) noexcept {
            featureGuardLogger(raw)(msg);
        },
        -1,
        [&]() -> int {
            if (!host || !plugin_ctx) {
                return -1;
            }
            auto* ctx = new FeatureCtx();
            raw       = ctx;
            ctx->init(host);
            *plugin_ctx = ctx;
            return 0;
        }
    );
}

/// 挂载事务 (mount): 登记实现行与有序钩子
/// - `ref` 为本次挂载的 agent 引用, 全部登记都记在该 agent 名下
static int featureAgentSetup(FeatureCtx& ctx, const agentxx::plugin::ScopeRef& ref) {
    using namespace agentxx::plugin;

    // 能力表缺失 (主程序太旧 / 清单未声明): 按"功能不可用"降级, 不影响加载
    if (!ctx.iface.capabilities || !ctx.iface.capabilities->register_capability) {
        ctx.log.info("example_feature: capabilities iface unavailable, plugin stays idle");
        return 0;
    }

    // 1. 自己的扩展点: 同步实现行
    //    - 收到的是调用载荷 JSON: {point, args, request, caller, viaCall, identity, sessionId}
    //    - 返回值就是这次调用的结果正文; 想显式置空就回 `{"disable":true}`
    //    - `pointAnswer` 把业务 JSON 包成 `{"value": ...}` 形式 (与早期写法一致),
    //      已经带 `value` / `disable` 的会原样返回
    {
        std::string error;
        const int32_t rc = providePoint(
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
            },
            {},
            &error
        );
        if (rc != 0) {
            ctx.log.warn("example_feature: providePoint(beat) rejected: " + error);
            return -1; // 挂载事务失败: 主程序按拒绝处理并回滚
        }
    }

    // 2. 同一个点的异步实现行 (协程形态: 用 kit 原语等待, 不阻塞 IO 线程)
    //    - 协程的返回值就是回答本身, 用 `pointAnswer` 包一层 (与同步形态一致)
    //    - `value` / `disable` 是回答的保留键: 业务数据里要用同名字段时自己包一层
    {
        std::string   error;
        const int32_t rc = providePoint(
            ctx,
            ref,
            "plugin.example_feature.slow",
            [](FeatureCtx& c, std::string_view call) -> agentxx::plugin::Task<std::string> {
                const auto in   = utilxx_base::Json::parse(call);
                const auto args = in.value<utilxx_base::Json>("args", utilxx_base::Json::object());
                // 等待 60ms: 这类等待让出驱动序列, 期间 IO 线程可继续处理其他工作
                co_await sleep(c, 60);
                auto out       = utilxx_base::Json::object();
                out["counter"] = ++c.counter;
                out["input"]   = args.value<int64_t>("input", 1);
                co_return pointAnswer(out.dump());
            },
            {},
            &error
        );
        if (rc != 0) {
            ctx.log.warn("example_feature: providePoint(slow) rejected: " + error);
            return -1;
        }
    }

    // 3. 给主程序的扩展点登记实现行 (固定 token 数, 便于测试断言)
    //    - 主程序点写全名; 行只对本挂载生效, 优先级默认 0
    //    - 点还没声明 (如裸 PluginManager 环境) 时登记被拒, 记一条日志继续
    {
        std::string   error;
        const int32_t rc = providePoint(
            ctx,
            ref,
            agentxx::plugin::points::ContextCountTokens,
            [](FeatureCtx&, std::string_view) {
                return std::string{R"_({"value":{"tokens":424242}})_"};
            },
            {},
            &error
        );
        if (rc != 0) {
            ctx.log.info("example_feature: host point countTokens not declared yet: " + error);
        }
    }

    // 4. 有序登记一个钩子处理器 (`agentxx.agent.hooks` 的表尾三成员)
    //    - 优先级 -10: 排在默认优先级 (0) 的其它插件钩子之前
    //    - 同一个点可以登记多个 (各持一根句柄, 用 unhookEx 精确撤销); 不撤销也行:
    //      处理器跟着实例走, 禁用/卸载时由主程序自动摘除
    //    - 主程序不支持时返回 0, 这里按"功能不可用"降级 (加载照常成功)
    if (ctx.iface.hooks && ctx.iface.hooks->register_hook) {
        HookOptions opts;
        opts.priority        = -10;
        opts.ownerTag        = "example_feature";
        opts.depict          = "演示有序登记: 排在其它插件的钩子之前";
        const int64_t handle = hookEx(
            ctx,
            ref,
            AGENTXX_PLUGIN_HOOK_AGENT_START,
            [](FeatureCtx& c, AgentxxPluginHookPoint point, std::string_view) {
                // 载荷固定为 {sessionId, point}: 钩子只做通知, 需要更多信息请用扩展点
                c.log.info(
                    point == AGENTXX_PLUGIN_HOOK_AGENT_START
                        ? "example_feature hook: agent_start fired"
                        : "example_feature hook: unexpected point"
                );
            },
            opts
        );
        if (handle <= 0) {
            ctx.log.info("example_feature: ordered hook registration rejected by host");
        }
    } else {
        ctx.log.info("example_feature: agentxx.agent.hooks ordered members unavailable, skipped");
    }

    return 0;
}

static void* featureAgentStart(
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
        return nullptr;
    }
    // 进程级启动: 本插件没有自管线程/定时器, 注册全在挂载阶段做
    ctx.log.info("example_feature started");
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
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
    const agentxx::plugin::AgentRef ref(*spec);
    if (featureAgentSetup(ctx, ref) != 0) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_feature mount: registration transaction failed"
            );
        }
        return nullptr;
    }
    ctx.log.info("example_feature mounted");
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* featureAgentUnmount(
    FeatureCtx&, void*, const PluginxxOperatorNotify* notify, PluginxxString*
) {
    // 实现行与钩子由主程序按挂载记录撤销
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void*
    featureAgentStop(FeatureCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    // 反注册由主程序在 stop 后统一撤销 (实现行随实例走), 这里只给完成信号
    ctx.log.info("example_feature stopped");
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(
    FeatureCtx,
    featureAgentStart,
    featureAgentStop,
    featureAgentMount,
    featureAgentUnmount
)

extern "C" PLUGINXX_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<FeatureCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(featureGuardLogger(ctx), [&] {
        delete ctx;
    });
}
