/// 反例（必须编译失败）：polled_tool 业务必须返回 `asio::awaitable<std::string>`
/// （同步返回值意味着它不需要受控轮询，应使用 blocking_tool/fast_tool）。
#include "agentxx/plugin/api/plugin_kit.h"

using namespace agentxx::plugin;

struct Ctx : PluginBase {};

void useBadPolledTool(Ctx& ctx) {
    polled_tool(
        ctx,
        "bad.polled",
        "d",
        "{}",
        [](Ctx&, std::string_view, std::string_view, std::string_view, const AgentxxPluginCancelToken*)
            -> std::string { return {}; }
    );
}
