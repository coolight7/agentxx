/// 反例（必须编译失败）：`tool` 的同步体只能返回 `std::string` / `ToolResult`。
/// 返回 `int` 不属于任何受支持的类型，必须编译期拒绝。
#include "agentxx/plugin/api/agent_plugin_kit.h"
#include "agentxx/plugin/api/client_plugin_kit.h"

using namespace agentxx::plugin;

struct Ctx : PluginBase {};

void useBadToolReturn(Ctx& ctx) {
    tool(ctx, ScopeRef{}, "bad.tool", "d", "{}", [](Ctx&, std::string_view) -> int {
        return 0;
    })
        .forget();
}
