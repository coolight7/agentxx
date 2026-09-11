/// 反例（必须编译失败）：capability 业务只接受字符串类（同步）或 `Task<T>`（异步）。
/// 返回 `int` 不属于任何受支持签名。
#include "agentxx/plugin/api/plugin_kit.h"

using namespace agentxx::plugin;

struct Ctx : PluginBase {};

void useBadCapability(Ctx& ctx) {
    capability(ctx, "bad.cap", [](Ctx&, std::string_view, std::string_view) -> int {
        return 0;
    });
}
