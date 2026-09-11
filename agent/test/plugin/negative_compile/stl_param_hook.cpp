/// 反例（必须编译失败）：跨 C ABI 边界不能传 STL 类型。
/// hook 业务参数只接受 `std::string_view`（输入 JSON 视图）；写 `std::vector`
/// 参数说明调用方误以为可以跨边界传标准库容器，必须编译期拒绝。
#include "agentxx/plugin/api/plugin_kit.h"

#include <string>
#include <vector>

using namespace agentxx::plugin;

struct Ctx : PluginBase {};

void useStlParam(Ctx& ctx) {
    hook(
        ctx,
        AGENTXX_PLUGIN_HOOK_AGENT_START,
        [](Ctx&, const std::vector<std::string>&) -> void {}
    );
}
