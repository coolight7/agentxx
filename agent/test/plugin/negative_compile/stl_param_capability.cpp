/// 反例（必须编译失败）：跨 C ABI 边界不能传 STL 类型。
/// 能力实现体的参数只接受 `std::string_view`（method / 输入 JSON 视图）；写
/// `std::vector` 参数说明调用方误以为可以跨边界传标准库容器，必须编译期拒绝。
#include "agentxx/plugin/api/plugin_kit.h"

#include <string>
#include <vector>

using namespace agentxx::plugin;

struct Ctx : PluginBase {};

void useStlParam(Ctx& ctx) {
    capability(
        ctx,
        "bad.cap",
        [](Ctx&, std::string_view, const std::vector<std::string>&) -> std::string {
            return {};
        }
    );
}
