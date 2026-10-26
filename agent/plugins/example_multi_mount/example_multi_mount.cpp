/// example_multi_mount —— 多挂载示例插件 (每 agent 一份状态)
///
/// 演示"一次加载、可被多个 agent 挂载":
/// - 进程级 (`start`) 什么都不注册, 只给出完成信号;
/// - 每份挂载 (`mount`) 各自建一份状态 (计数 + 本挂载的 config_json), 注册两个工具:
///     - `multi_mount_incr`: 计数 +1 并返回当前值 (状态按挂载隔离)
///     - `multi_mount_state`: 返回本挂载的 agentId / 挂载序号 / 计数 / config_json
/// - 拆挂载 (`unmount`) 释放该挂载的状态, 其他挂载不受影响。
///
/// 测试用它验证: 多 agent 各自的计数互不影响、重新挂载计数归零、
/// `plugin.mount[].args` 按 agent 覆盖。
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "fmt/format.h"
#include "utilxx_base/json.h"

#include <memory>
#include <string>
#include <string_view>

/// 每实例上下文 (进程一份)
struct MultiMountCtx : public agentxx::plugin::PluginBase {};

/// 一份挂载的状态
///
/// 工具回调经 `shared_ptr` 共享它: 拆挂载时主程序可能还持着工具对象
/// (进行中调用), 用引用计数保证回调里不会用到已释放的状态。
struct MountState {
    agentxx::plugin::AgentRef ref;
    std::string               configJson = "{}";
    int                       count      = 1;
};

static auto guardLogger(MultiMountCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx && ctx->host && ctx->iface.log && ctx->iface.log->log) {
            agentxx::plugin::logTo(ctx->host, ctx->iface.log, 4, "example_multi_mount", msg);
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
                agentxx::plugin::PluginStringView::fromCstr("example_multi_mount"),
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
                    "Example plugin mounted by several agents, one state per mount"
                ),
            };
            return &info;
        }
    );
}

extern "C" PLUGINXX_EXPORT int
    agentxx_plugin_agent_create(const PluginxxHost* host, void** plugin_ctx) {
    MultiMountCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* msg) noexcept {
            guardLogger(raw)(msg);
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx = std::make_unique<MultiMountCtx>();
            ctx->init(host);
            raw = ctx.get();
            // create 只构造与查询接口: 注册事务在 mount 里做 (那时才有挂载引用)
            if (!ctx->iface.tools || !ctx->iface.tools->register_tool) {
                return -1;
            }
            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

/// 进程级启动: 本插件没有线程/定时器/进程级注册
static void* multiMountStart(
    MultiMountCtx&, const PluginxxOperatorNotify* notify, PluginxxString*
) {
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return nullptr;
}

/// 进程级停止: 没有进程级资源需要回收, 只给完成信号
/// (各挂载的状态在 `unmount` 里释放)
static void* multiMountStop(
    MultiMountCtx&, const PluginxxOperatorNotify* notify, PluginxxString*
) {
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return nullptr;
}

/// 每个 agent 一次: 建本挂载的状态 + 注册工具
static void* multiMountForAgent(
    MultiMountCtx&                     ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error
) {
    if (!notify || !notify->done) {
        agentxx::plugin::PluginString::set(
            ctx.host,
            error,
            "example_multi_mount mount: notify required"
        );
        return nullptr;
    }
    if (!spec) {
        agentxx::plugin::PluginString::set(ctx.host, error, "mount spec is null");
        return nullptr;
    }
    auto state = std::make_shared<MountState>();
    state->ref = agentxx::plugin::AgentRef(*spec);
    if (spec->config_json.data && spec->config_json.size > 0) {
        state->configJson.assign(
            spec->config_json.data,
            static_cast<size_t>(spec->config_json.size)
        );
    }

    // 工具 1: 计数 +1 (计数按挂载隔离)
    agentxx::plugin::fast_tool(
        ctx,
        state->ref,
        "multi_mount_incr",
        "Increase this mount's counter and return the new value (example_multi_mount)",
        R"({"type":"object","properties":{"by":{"type":"integer","description":"increment step, default 1"}}})",
        [state](std::string_view argsJson) -> std::string {
            int step = 1;
            try {
                auto args = utilxx_base::Json::parse(
                    argsJson.empty() ? std::string{"{}"} : std::string{argsJson}
                );
                if (args.is_object() && args.contains("by") && args["by"].is_number_integer()) {
                    step = args["by"].get<int>();
                }
            } catch (const std::exception&) {
                // 参数不是合法 JSON 时按默认步长处理: 示例插件不为此加分支
            }
            state->count += step;
            return fmt::format(
                R"({{"agentId":"{}","seq":{},"count":{}}})",
                state->ref.id(),
                state->ref.seq(),
                state->count
            );
        }
    );

    // 工具 2: 本挂载的状态 (含 config_json, 用于验证 plugin.mount[].args 覆盖)
    agentxx::plugin::fast_tool(
        ctx,
        state->ref,
        "multi_mount_state",
        "Return this mount's agentId, mount sequence, counter and config JSON (example_multi_mount)",
        R"({"type":"object","properties":{}})",
        [state](std::string_view) -> std::string {
            return fmt::format(
                R"({{"agentId":"{}","seq":{},"count":{},"config":{}}})",
                state->ref.id(),
                state->ref.seq(),
                state->count,
                state->configJson.empty() ? std::string{"{}"} : state->configJson
            );
        }
    );

    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    // 返回 per-agent 状态: 拆挂载时原样交回
    return new std::shared_ptr<MountState>(state);
}

/// 拆挂载: 工具由主程序按挂载记录撤销, 这里只释放本挂载的状态
static void* multiMountForAgentEnd(
    MultiMountCtx&, void* mountUserData, const PluginxxOperatorNotify* notify, PluginxxString*
) {
    delete static_cast<std::shared_ptr<MountState>*>(mountUserData);
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(
    MultiMountCtx,
    multiMountStart,
    multiMountStop,
    multiMountForAgent,
    multiMountForAgentEnd
)

extern "C" PLUGINXX_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<MultiMountCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(guardLogger(ctx), [&] {
        if (!ctx) {
            return;
        }
        delete ctx;
    });
}
