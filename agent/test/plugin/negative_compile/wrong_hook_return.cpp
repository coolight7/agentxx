/// 反例（必须编译失败）：hook 业务只接受 `void`（同步）或 `Task<T>`（异步）。
/// 返回 `int` 属于签名不匹配，F19 要求严格分发并编译失败。
#include "agentxx/plugin/api/plugin_kit.h"

using namespace agentxx::plugin;

struct Ctx : PluginBase {};

void useBadHook(Ctx& ctx) {
    hook(ctx, AGENTXX_PLUGIN_HOOK_AGENT_START, [](Ctx&, std::string_view) -> int {
        return 0;
    });
}
