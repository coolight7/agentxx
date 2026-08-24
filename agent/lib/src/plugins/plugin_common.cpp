/*
 * plugin_common.cpp —— 插件系统公共设施实现 (见 plugin_common.h)
 * - 原实现分别位于 plugin_manager.cpp / client_plugin_manager.cpp,
 *   提取后两侧共用, 避免行为漂移
 */
#include "agentxx/plugin/plugin_common.h"

#include "agentxx/plugin/client_plugin_api.h"
#include "agentxx/util/log.h"
#include "yaml-cpp/yaml.h"

#include <system_error>

namespace agentxx {
namespace plugin {

std::string pluginNameFromPath(const std::string& path) {
    auto base = std::filesystem::path(path).filename().string();
    // 去扩展名及其后版本号 (rfind: 兼容文件名内含扩展名片段, 如
    // my.plugin.so → my.plugin; libfoo.so.1.2 → libfoo)
    bool removedExt = false;
    for (const char* ext : {".dylib", ".so", ".dll"}) {
        auto pos = base.rfind(ext);
        if (pos != std::string::npos && pos > 0) {
            base.erase(pos);
            removedExt = true;
            break;
        }
    }
    // 去 lib 前缀 (仅当剥离过扩展名: 无扩展名的库/目录保持原名, 防误剥)
    if (removedExt && base.size() > 3 && base.compare(0, 3, "lib") == 0) {
        base.erase(0, 3);
    }
    return base;
}

bool parsePluginManifest(
    const std::filesystem::path& dir,
    std::string&                 name,
    std::string&                 entry,
    std::vector<std::string>&    depends,
    std::vector<std::string>&    optionalDepends,
    PluginManifestResources*     resources,
    PluginManifestInterfaces*    interfaces
) {
    auto            yamlPath = dir / "plugin.yaml";
    std::error_code ec;
    if (!std::filesystem::exists(yamlPath, ec)) {
        return false;
    }
    try {
        auto node = YAML::LoadFile(yamlPath.string());
        name      = node["name"] ? node["name"].as<std::string>() : std::string{};
        entry     = node["entry"] ? node["entry"].as<std::string>() : std::string{};
        if (node["depends"] && node["depends"].IsSequence()) {
            for (const auto& d : node["depends"]) {
                if (d.IsScalar()) {
                    depends.push_back(d.as<std::string>());
                }
            }
        }
        if (node["optional_depends"] && node["optional_depends"].IsSequence()) {
            for (const auto& d : node["optional_depends"]) {
                if (d.IsScalar()) {
                    optionalDepends.push_back(d.as<std::string>());
                }
            }
        }
        // ---- 资源声明段 (skill/memory/mcp; 键名与主配置 yaml 一致) ----
        // 相对路径按插件目录解析为绝对路径; 段缺失/非法项跳过并告警,
        // 不影响 manifest 合法性 (声明是可选增强)
        if (resources) {
            *resources = PluginManifestResources{};
            // 绝对路径原样保留; 相对路径按插件目录拼接并规范化
            auto resolveRel = [&dir](const std::string& p) -> std::string {
                if (p.empty()) {
                    return {};
                }
                std::filesystem::path fp{p};
                if (fp.is_absolute()) {
                    return p;
                }
                return (dir / fp).lexically_normal().string();
            };
            if (node["skill"] && node["skill"].IsSequence()) {
                for (const auto& s : node["skill"]) {
                    if (!s.IsScalar()) {
                        continue;
                    }
                    auto p = resolveRel(s.as<std::string>());
                    if (!p.empty()) {
                        resources->skillDirs.push_back(std::move(p));
                    }
                }
            }
            if (node["memory"] && node["memory"].IsSequence()) {
                for (const auto& m : node["memory"]) {
                    if (!m.IsScalar()) {
                        continue;
                    }
                    auto p = resolveRel(m.as<std::string>());
                    if (!p.empty()) {
                        resources->memoryFiles.push_back(std::move(p));
                    }
                }
            }
            if (node["mcp"] && node["mcp"].IsSequence()) {
                for (const auto& m : node["mcp"]) {
                    if (!m.IsMap()) {
                        continue;
                    }
                    auto ns  = m["namespace"] ? m["namespace"].as<std::string>() : std::string{};
                    auto url = m["url"] ? m["url"].as<std::string>() : std::string{};
                    if (ns.empty() || url.empty()) {
                        XX_LOGW(
                            "Plugin manifest `{}` mcp entry missing `namespace`/`url`, skipped",
                            yamlPath.string()
                        );
                        continue;
                    }
                    long long timeoutSec = 120; // 与主配置默认一致
                    if (m["timeout"]) {
                        try {
                            timeoutSec = m["timeout"].as<long long>();
                        } catch (const std::exception&) {
                            XX_LOGW(
                                "Plugin manifest `{}` mcp `{}` invalid timeout, using default",
                                yamlPath.string(),
                                ns
                            );
                        }
                    }
                    if (timeoutSec < 0) {
                        timeoutSec = 0;
                    }
                    // 重复命名空间: 后者覆盖前者 (与主配置 override 行为一致)
                    resources->mcpServers[ns] = PluginManifestResources::McpDecl{
                        .url       = url,
                        .timeoutMs = timeoutSec * 1000
                    };
                }
            }
        }
        // ---- 接口声明段 (require/optional; 见 PluginManifestInterfaces) ----
        // 段缺失/非法项跳过并告警, 不影响 manifest 合法性 (声明是可选增强);
        // 名称仅做非空校验, 语义 (前缀归属/宿主支持集比对) 由加载路径的
        // checkInterfacesForSide 处理 —— 解析与协商解耦, 第三方前缀天然合法
        if (interfaces) {
            *interfaces = PluginManifestInterfaces{};
            auto readList = [&yamlPath](const YAML::Node&      section,
                                        std::vector<std::string>& out,
                                        std::string_view          what) {
                if (!section || !section.IsSequence()) {
                    return;
                }
                for (const auto& item : section) {
                    if (!item.IsScalar()) {
                        continue;
                    }
                    auto n = item.as<std::string>();
                    if (n.empty()) {
                        XX_LOGW(
                            "Plugin manifest `{}` interfaces.{} has empty name, skipped",
                            yamlPath.string(),
                            what
                        );
                        continue;
                    }
                    out.push_back(std::move(n));
                }
            };
            if (node["interfaces"] && node["interfaces"].IsMap()) {
                readList(node["interfaces"]["require"], interfaces->require, "require");
                readList(node["interfaces"]["optional"], interfaces->optional, "optional");
            }
        }
    } catch (const std::exception& e) {
        XX_LOGE("Parse plugin manifest `{}` failed: {}", yamlPath.string(), e.what());
        return false;
    }
    if (name.empty() || entry.empty()) {
        XX_LOGE("Plugin manifest `{}` invalid: name/entry required", yamlPath.string());
        return false;
    }
    return true;
}

std::string resolvePluginEntryPath(const std::filesystem::path& dir, const std::string& entry) {
    // 平台化: manifest 按 Linux 书写 (libfoo.so), Windows/macOS 下修正扩展名
    auto fixExt = [](std::string p) -> std::string {
#if defined(_WIN32)
        if (p.ends_with(".so")) {
            p.replace(p.size() - 3, 3, ".dll");
        }
#elif defined(__APPLE__)
        if (p.ends_with(".so")) {
            p.replace(p.size() - 3, 3, ".dylib");
        }
#endif
        return p;
    };
    auto            entryPath = fixExt((dir / entry).lexically_normal().string());
    std::error_code ec;
    if (!std::filesystem::exists(entryPath, ec)) {
        // 多配置生成器 (MSVC Debug/Release): 产物位于配置子目录
        for (const char* cfg : {"Debug", "Release", "RelWithDebInfo", "MinSizeRel"}) {
            auto candidate = fixExt((dir / cfg / entry).lexically_normal().string());
            if (std::filesystem::exists(candidate, ec)) {
                entryPath = std::move(candidate);
                break;
            }
        }
    }
    return entryPath;
}

// ==================== 接口协商基础设施 ====================

namespace {

/// 能力位 ↔ 接口名映射条目
struct CapNameEntry {
    uint32_t    cap;
    const char* name;
};

/// AGENTXX_UI_CAP_* (低位段) → 接口名 (与 client_plugin_api.h 位定义一一对应)
constexpr CapNameEntry kUiCapNames[] = {
    {AGENTXX_UI_CAP_STATUS_ITEM,  "client.status_item"},
    {AGENTXX_UI_CAP_PANEL,        "client.panel"},
    {AGENTXX_UI_CAP_TOAST,        "client.toast"},
    {AGENTXX_UI_CAP_KEYBIND,      "client.keybind"},
    {AGENTXX_UI_CAP_PROMPT,       "client.prompt_modal"},
    {AGENTXX_UI_CAP_MSG_DECOR,    "client.msg_decor"},
    {AGENTXX_UI_CAP_INFO_SECTION, "client.info_section"},
};

/// AGENTXX_IFACE_* (高位段, 非展示类接口) → 接口名
constexpr CapNameEntry kIfaceCapNames[] = {
    {AGENTXX_IFACE_COMMAND, "client.command"},
};

} // namespace

bool sideCaresAboutInterface(std::string_view name, bool agentSide) {
    if (name.starts_with("agent.")) {
        return agentSide;
    }
    if (name.starts_with("client.")) {
        return !agentSide;
    }
    // 无前缀 / <vendor>.*: 两侧都检查 (宿主不认识即不支持, 保守安全)
    return true;
}

InterfaceCheckResult checkInterfacesForSide(
    const PluginManifestInterfaces& decl,
    const InterfaceSet&             hostSupported,
    bool                            agentSide
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
        if (n.starts_with("agent.")) {
            out.agentEntry = true;
        } else if (n.starts_with("client.")) {
            out.clientEntry = true;
        } else {
            // 无前缀 / vendor 前缀: 保守视为两侧都可能依赖
            out.agentEntry  = true;
            out.clientEntry = true;
        }
    }
    return out;
}

InterfaceSet clientHostInterfacesFromCaps(uint32_t caps) {
    InterfaceSet out;
    for (const auto& e : kUiCapNames) {
        if ((caps & e.cap) != 0) {
            out.insert(e.name);
        }
    }
    for (const auto& e : kIfaceCapNames) {
        if ((caps & e.cap) != 0) {
            out.insert(e.name);
        }
    }
    return out;
}

} // namespace plugin
} // namespace agentxx
