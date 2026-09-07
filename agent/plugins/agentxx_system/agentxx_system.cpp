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

AGENTXX_PLUGIN_AGENT_EXPORT(
    SysPluginCtx,
    "agentxx_system",
    "1.0.0",
    "System info tools: current date/time with Unix timestamp",
    [](SysPluginCtx& ctx) -> int32_t {
        auto schema = ctx.schema(kNameDatetime).build();

        fast_tool(ctx, kNameDatetime, kDepictDatetime, schema, [](std::string_view) -> std::string {
            return currentDatetimeExecute();
        });

        return 0;
    }
);
