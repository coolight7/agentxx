/// agentxx 插件接口协商实现 (见 agentxx/plugin/plugin_interfaces.h)
///
/// 原实现位于 plugin_common.cpp: 接口协商需要知道本宿主实现的接口表目录, 属宿主
/// 领域, 因此留在 agentxx; 而与领域无关的清单解析/名称推导等通用设施已移至
/// cxx_pluginxx ([pluginxx::parsePluginManifest] 等)。
#include "agentxx/plugin/plugin_interfaces.h"

namespace agentxx {
namespace plugin {

bool sideCaresAboutInterface(std::string_view name, bool agentSide) {
    // "agentxx." 为本项目内置接口的保留命名空间; 按子前缀区分归属侧
    if (name.starts_with("agentxx.agent.")) {
        return agentSide;
    }
    if (name.starts_with("agentxx.client.")) {
        return !agentSide;
    }
    // 无前缀 / <vendor>.* / 其他 agentxx.* 子命名空间: 两侧都检查
    // (宿主不认识即不支持, 保守安全)
    return true;
}

InterfaceCheckResult checkInterfacesForSide(
    const pluginxx::PluginManifestInterfaces& decl,
    const InterfaceSet&                       hostSupported,
    bool                                      agentSide
) {
    InterfaceCheckResult out;
    auto                 check = [&](const std::vector<std::string>& list, bool required) {
        for (const auto& n : list) {
            if (!sideCaresAboutInterface(n, agentSide)) {
                continue; // 另一侧的声明与本侧无关
            }
            if (hostSupported.contains(n)) {
                continue;
            }
            if (required) {
                out.missingRequired.push_back(n);
            } else {
                out.missingOptional.push_back(n);
            }
        }
    };
    check(decl.require, true);
    check(decl.optional, false);
    out.satisfied = out.missingRequired.empty();
    return out;
}

RequiredEntrySides requiredEntrySides(const std::vector<std::string>& interfaces) {
    RequiredEntrySides out;
    for (const auto& n : interfaces) {
        if (n.starts_with("agentxx.agent.")) {
            out.agentEntry = true;
        } else if (n.starts_with("agentxx.client.")) {
            out.clientEntry = true;
        } else {
            // 无前缀 / vendor 前缀 / 其他 agentxx.* 子命名空间:
            // 保守视为两侧都可能依赖
            out.agentEntry  = true;
            out.clientEntry = true;
        }
    }
    return out;
}

} // namespace plugin
} // namespace agentxx
