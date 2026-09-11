/// SDK 反例编译检查的正向对照（必须编译成功）。
/// 作用：确认检查脚本使用的编译环境有效，避免"全部片段都编译失败"的假阳性。
#include "agentxx/plugin/api/plugin_kit.h"

using namespace agentxx::plugin;

struct Ctx : PluginBase {};

void usePositive(Ctx& ctx) {
    tool(
        ctx,
        "pos.tool",
        "d",
        "{}",
        [](Ctx&, std::string_view args, OpCtl) -> Task<std::string> {
            co_return std::string{args};
        }
    );
    hook(ctx, AGENTXX_PLUGIN_HOOK_AGENT_START, [](Ctx&, std::string_view) -> void {});
    capability(
        ctx,
        "pos.cap",
        [](Ctx&, std::string_view, std::string_view) -> std::string { return {}; }
    );
    graph_node(ctx, "pos.node", "{}", [](Ctx&, const RootRequest&) -> std::string {
        return "{}";
    });
}
