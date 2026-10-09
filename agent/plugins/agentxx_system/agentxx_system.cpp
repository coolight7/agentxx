/// agentxx_system —— 系统信息工具插件
#include "agentxx_system_plugin.h"
#include "system_impl.h"
#include <string>

using namespace agentxx_system_plugin;
using namespace agentxx::plugin;

namespace {

constexpr std::string_view kNameDatetime   = "agentxx_get_current_datetime";
constexpr std::string_view kDepictDatetime = "Get the current date, time, and Unix timestamp.";

/// 连续相同调用重复检查: 达阈值时由宿主询问用户确认后再继续
/// (见 [XXToolBase::repeatCallCheck] 与 AGENTXX_PLUGIN_TOOL_FLAG_REPEAT_CALL_CHECK)
constexpr int32_t kRepeatCheck = AGENTXX_PLUGIN_TOOL_FLAG_REPEAT_CALL_CHECK;

} // namespace

struct SysPluginCtx : public PluginBase {};

/// 注册事务 (start 的实际内容)。
static int32_t sysSetup(SysPluginCtx& ctx) {
    auto schema = ctx.schema(kNameDatetime).build();

    fast_tool(
        ctx,
        kNameDatetime,
        kDepictDatetime,
        schema,
        [](std::string_view) -> std::string {
            return currentDatetimeExecute();
        },
        0,
        kRepeatCheck
    );
    return 0;
}

static void*
    sysStart(SysPluginCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString* error) {
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
    sysStop
);
