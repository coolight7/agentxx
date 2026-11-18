/// agentxx_system —— 系统信息工具插件
#include "agentxx_system_plugin.h"
#include "system_impl.h"
#include <string>

using namespace agentxx_system_plugin;
using namespace agentxx::plugin;

namespace {

constexpr std::string_view kNameDatetime   = "agentxx_get_current_datetime";
constexpr std::string_view kDepictDatetime = "Get the current date, time, and Unix timestamp.";

/// 连续相同调用重复检查: 达阈值时由主程序询问用户确认后再继续
/// (见 [XXToolBase::repeatCallCheck] 与 AGENTXX_PLUGIN_TOOL_FLAG_REPEAT_CALL_CHECK)
constexpr int32_t kRepeatCheck = AGENTXX_PLUGIN_TOOL_FLAG_REPEAT_CALL_CHECK;

} // namespace

struct SysPluginCtx : public PluginBase {};

/// 注册事务 (mount 的实际内容; 每个 agent 各自一份)。
static pluginxx::Task<void> sysSetupAsync(SysPluginCtx& ctx, ScopeRef ref) {
    auto schema = (co_await agentxx::plugin::schemaOf(ctx, kNameDatetime)).build();

    co_await fast_tool(
        ctx,
        ref,
        kNameDatetime,
        kDepictDatetime,
        schema,
        [](std::string_view) -> std::string {
            return currentDatetimeExecute();
        },
        0,
        kRepeatCheck
    );
    co_return;
}

static void*
    sysStart(SysPluginCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    (void)ctx;
    return nullptr;
}

static void* sysMount(
    SysPluginCtx&                      ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error
) {
    if (!notify) {
        if (error) {
            PluginString::set(ctx.host, error, "agentxx_system mount: notify required");
        }
        return nullptr;
    }
    auto later = finishLater(notify);
    if (!ctx.iface.tasks || !ctx.iface.tasks->register_task) {
        later.failed("agentxx_system mount: host has no pluginxx.tasks (spawn unavailable)");
        return later.provider();
    }
    const AgentRef agentRef(*spec);
    // 注册事务交给协程: 每个注册都是"提交 + co_await", 全部生效后再放行挂载
    ctx.spawn([later, agentRef](SysPluginCtx& c, OpCtl&) -> pluginxx::Task<void> {
        try {
            co_await sysSetupAsync(c, agentRef);
            later.ok();
        } catch (const std::exception& e) {
            later.failed(e.what());
        }
        co_return;
    });
    return later.provider();
}

static void*
    sysUnmount(SysPluginCtx&, void*, const PluginxxOperatorNotify* notify, PluginxxString*) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* sysStop(SysPluginCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_EXPORT(
    SysPluginCtx,
    "agentxx_system",
    "1.0.0",
    "System info tools: current date/time with Unix timestamp",
    sysStart,
    sysStop,
    sysMount,
    sysUnmount
);
