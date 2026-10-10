/// example_feature —— 功能点插件示例 (agentxx.agent.feature 接口表)
///
/// 演示一个插件在功能点体系里能做的三件事:
/// 1. **声明自己的点** (`plugin.example_feature.beat`):
///    命名空间必须落在 `plugin.<自己>.*`, 声明期间恒可被任何一方调用;
/// 2. **为宿主点登记实现** (`agentxx.context.countTokens`):
///    插件实现排在 `plugin` 层 (默认优先级 0), 因此会先于核心实现生效;
///    摘除后自动回落到核心实现;
/// 3. **调用功能点** (`callFeature`): 只拿数据 —— 不写值缓存、不记置空、
///    不改调用方会话 / 不发提示 / 不落盘。
///
/// 另含一个异步形态的实现示例 (`plugin.example_feature.slow`): 用 kit 协程原语
/// (`co_await sleep`) 等待, 由插件协程驱动桥推进 (与 tool/hook 的写法一致)。
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "utilxx_base/json.h"

#include <chrono>
#include <string>
#include <string_view>

/// =====================================================================
/// 每实例上下文 (多实例契约: 状态只放这里, 不用可变全局)
/// =====================================================================

struct FeatureCtx : public agentxx::plugin::PluginBase {
    /// 本实例登记过实现的点 (stop 时无需反注册: 宿主随实例禁用/卸载摘除)
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
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "Example feature-point plugin: declare point / provide impls / call points"
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

static int featureAgentSetup(FeatureCtx& ctx) {
    using namespace agentxx::plugin;

    // 功能点接口表缺失 (宿主太旧 / 清单未声明): 按"功能不可用"降级, 不影响加载
    if (!ctx.iface.feature || !ctx.iface.feature->define_point) {
        ctx.log.info("example_feature: feature iface unavailable, plugin stays idle");
        return 0;
    }

    // 1. 声明自己的点 (id 必须落在 `plugin.example_feature.*`)
    {
        FeaturePointSpec spec;
        spec.id        = "plugin.example_feature.beat";
        spec.title     = "节拍检测";
        spec.depict    = "算出这段音频的 BPM 与节拍时刻";
        spec.argsDoc   = "path (本地文件路径), 可选 minBpm / maxBpm";
        spec.resultDoc = "{bpm, beats:[ms], confident}";
        if (defineFeaturePoint(ctx, spec) != 0) {
            return -1; // start 事务失败: 宿主按拒绝处理并回滚
        }
    }
    // 另一个点: 用来演示异步实现形态 (等待 60ms 后给值)
    {
        FeaturePointSpec spec;
        spec.id        = "plugin.example_feature.slow";
        spec.title     = "慢速计算";
        spec.depict    = "等待一小段时间后给出一个数值 (演示异步实现)";
        spec.argsDoc   = "{input} (可选, 缺省 1)";
        spec.resultDoc = "{counter, input}";
        if (defineFeaturePoint(ctx, spec) != 0) {
            return -1;
        }
    }
    // 第三个点: 声明"等实现方最多 50ms", 实现却要等 400ms —— 用来演示
    // "到点取消当次实现并按没意见继续" (调用方会看到 no_impl 而不是一直等)
    {
        FeaturePointSpec spec;
        spec.id            = "plugin.example_feature.tooslow";
        spec.title         = "超时演示";
        spec.depict        = "实现比点声明的时限慢, 用来看超时后的行为";
        spec.argsDoc       = "无";
        spec.resultDoc     = "{} (正常路径下拿不到值: 实现会被超时取消)";
        spec.implTimeoutMs = 50;
        if (defineFeaturePoint(ctx, spec) != 0) {
            return -1;
        }
    }

    // 2. 为自己的点登记实现 (同步形态: 必须快速返回, 不做 IO / 不等待)
    //    - 收到的是调用载荷 JSON: {point, args, request, caller, viaCall, identity, sessionId}
    //    - 返回文本会被 kit 包成回答: 返回完整 JSON 对象时按 `{"value": ...}` 处理
    if (provideFeature(ctx, "plugin.example_feature.beat", [](FeatureCtx& c, std::string_view call) {
            ++c.counter;
            const auto in   = utilxx_base::Json::parse(call);
            const auto args = in.value<utilxx_base::Json>("args", utilxx_base::Json::object());
            const auto path = args.value<std::string>("path", "");
            auto       out  = utilxx_base::Json::object();
            out["bpm"]       = 128;
            out["path"]      = path;
            out["beats"]     = utilxx_base::Json::array();
            out["confident"] = true;
            return out.dump();
        })
        != 0) {
        return -1;
    }

    // 3. 为自己的点登记异步实现 (协程形态: 用 kit 原语等待, 不阻塞 IO 线程)
    //    - 协程实现的返回值就是回答本身, 用 `featureAnswer` 包一层 (与同步形态一致)
    //    - 注意 `value` / `disable` / `verdict` 是回答的保留键: 业务数据里要用同名字段
    //      时自己包一层 `{"value": <数据>}` (这里改用 `input` 避开)
    if (provideFeature(
            ctx,
            "plugin.example_feature.slow",
            [](FeatureCtx& c, std::string_view call) -> agentxx::plugin::Task<std::string> {
                const auto in   = utilxx_base::Json::parse(call);
                const auto args = in.value<utilxx_base::Json>("args", utilxx_base::Json::object());
                // 等待 60ms: 这类等待让出驱动序列, 期间 IO 线程可继续处理其他工作
                co_await sleep(c, 60);
                auto out       = utilxx_base::Json::object();
                out["counter"] = ++c.counter;
                out["input"]   = args.value<int64_t>("input", 1);
                co_return featureAnswer(out.dump());
            }
        )
        != 0) {
        return -1;
    }

    // 4. 为"超时演示"点登记异步实现 (等 400ms; 点声明的时限只有 50ms)
    if (provideFeature(
            ctx,
            "plugin.example_feature.tooslow",
            [](FeatureCtx& c, std::string_view) -> agentxx::plugin::Task<std::string> {
                co_await sleep(c, 400);
                // 已经是完整回答 (`{"value":...}`) 时 featureAnswer 原样返回
                co_return featureAnswer(std::string{R"({"value":{"slept":true}})"});
            }
        )
        != 0) {
        return -1;
    }

    // 5. 为宿主点登记实现 (覆盖核心 token 估算: 固定值, 便于测试断言)
    if (provideFeature(ctx, "agentxx.context.countTokens", [](FeatureCtx&, std::string_view) {
            return std::string{R"({"tokens":424242})"};
        })
        != 0) {
        // 宿主点可能未装配 (如裸 PluginManager 环境): 不算致命, 记一条日志继续
        ctx.log.info("example_feature: host point agentxx.context.countTokens not declared yet");
    }

    return 0;
}

static void* featureAgentStart(FeatureCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString* error) {
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
    if (featureAgentSetup(ctx) != 0) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "example_feature start: registration transaction failed"
            );
        }
        return nullptr;
    }
    ctx.log.info("example_feature started");
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* featureAgentStop(FeatureCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    // 反注册由宿主在 stop 后统一撤销 (实现/点随实例走), 这里只给完成信号
    ctx.log.info("example_feature stopped");
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(FeatureCtx, featureAgentStart, featureAgentStop)

extern "C" PLUGINXX_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<FeatureCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(featureGuardLogger(ctx), [&] {
        delete ctx;
    });
}
