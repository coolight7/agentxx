/// agentxx_system —— 系统信息工具插件
#include "agentxx_system_plugin.h"
#include "system_impl.h"
#include <string>

using namespace agentxx_system_plugin;
using namespace agentxx::plugin;

namespace {

constexpr std::string_view kNameDatetime   = "agentxx_get_current_datetime";
constexpr std::string_view kDepictDatetime = "Get the current date, time, and Unix timestamp.";

} // namespace

struct SysPluginCtx : public PluginBase {};

/// 注册事务 (start 的实际内容)。
static int32_t sysSetup(SysPluginCtx& ctx) {
    auto schema = ctx.schema(kNameDatetime).build();

    fast_tool(ctx, kNameDatetime, kDepictDatetime, schema, [](std::string_view) -> std::string {
        return currentDatetimeExecute();
    });
    return 0;
}

static void* sysStart(
    SysPluginCtx&                      ctx,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error
) {
    if (!notify) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "agentxx_system start: notify required"
            );
        }
        return nullptr;
    }
    if (sysSetup(ctx) != 0) {
        if (error) {
            agentxx::plugin::PluginString::set(
                ctx.host,
                error,
                "agentxx_system start: registration failed"
            );
        }
        return nullptr;
    }
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

static void*
    sysStop(SysPluginCtx&, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*) {
    notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_EXPORT(
    SysPluginCtx,
    "agentxx_system",
    "1.0.0",
    "System info tools: current date/time with Unix timestamp",
    sysStart,
    sysStop
);
