/// 反例（必须编译失败）：capability 业务只接受字符串类（同步）或
/// `asio::awaitable<T>`（异步）。返回 `int` 不属于任何受支持签名。
#include "agentxx/plugin/api/agent_plugin_kit.h"
#include "agentxx/plugin/api/client_plugin_kit.h"

using namespace agentxx::plugin;

struct Ctx : PluginBase {};

asio::awaitable<void> useBadCapability(Ctx& ctx) {
    co_await capabilityAsync(ctx, "bad.cap", [](Ctx&, std::string_view, std::string_view) -> int {
        return 0;
    });
    co_return;
}
