/// 反例（必须编译失败）：tool 业务必须返回 `Task<T>`；返回普通值不是受支持签名。
#include "agentxx/plugin/api/plugin_kit.h"

using namespace agentxx::plugin;

struct Ctx : PluginBase {};

void useBadTool(Ctx& ctx) {
    tool(ctx, "bad.tool", "d", "{}", [](Ctx&, std::string_view, OpCtl) -> int { return 0; });
}
