/// 反例（必须编译失败）：`toolAsync` 的工具体只能是 `asio::awaitable<std::string>`
/// 或 `asio::awaitable<ToolResult>`。返回 `int` 不属于任何受支持的类型，必须编译期拒绝。
#include "agentxx/plugin/api/agent_plugin_kit.h"
#include "agentxx/plugin/api/client_plugin_kit.h"

using namespace agentxx::plugin;

struct Ctx : PluginBase {};

asio::awaitable<void> useBadToolReturn(Ctx& ctx) {
    co_await toolAsync(ctx, ScopeRef{}, "bad.tool", "d", "{}", [](Ctx&, std::string_view) -> int {
        return 0;
    });
    co_return;
}
