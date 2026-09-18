/// pluginxx 插件清单与名称推导实现 (见 pluginxx/host/manifest.h)
///
/// 原实现位于 libagentxx 的 plugin_common.cpp: 因 agent 侧与 client 侧插件宿主
/// 共用同一套清单解析/名称推导/入口路径解析, 提取到框架内核, 避免两侧行为漂移。
#include "pluginxx/host/manifest.h"

#include "pluginxx/api/abi.h"
#include "yaml-cpp/yaml.h"

#include <system_error>
#include <utility>

namespace pluginxx {

std::string_view pluginStringView2std(AgentxxPluginStringView str) {
    return std::string_view{str.data, static_cast<size_t>(str.size)};
}

namespace {
/// 提供者登记位 (函数内静态: 与任何静态初始化顺序无关)
BuiltinPluginProvider& providerSlot() noexcept {
    static BuiltinPluginProvider provider{};
    return provider;
}
} // namespace

void setBuiltinPluginProvider(BuiltinPluginProvider provider) noexcept {
    providerSlot() = provider;
}

BuiltinPluginProvider builtinPluginProvider() noexcept {
    return providerSlot();
}

const AgentxxPluginBuiltinInfo* findBuiltinPlugin(std::string_view name) {
    auto fn = providerSlot().plugins;
    if (!fn) {
        return nullptr;
    }
    size_t      count = 0;
    const auto* list  = fn(&count);
    if (!list) {
        return nullptr;
    }
    for (size_t i = 0; i < count; ++i) {
        if (pluginStringView2std(list[i].name) == name) {
            return &list[i];
        }
    }
    return nullptr;
}

const AgentxxPluginBuiltinManifest* findBuiltinManifest(std::string_view name) {
    auto fn = providerSlot().manifests;
    if (!fn) {
        return nullptr;
    }
    size_t      count = 0;
    const auto* list  = fn(&count);
    if (!list) {
        return nullptr;
    }
    for (size_t i = 0; i < count; ++i) {
        if (pluginStringView2std(list[i].name) == name) {
            return &list[i];
        }
    }
    return nullptr;
}

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

namespace {
/// 共享的 Node 解析逻辑 (文件与字符串共用)
bool parseManifestNode(
    const YAML::Node&            node,
    const std::filesystem::path& baseDir,
    const std::string&           logHint,
    std::string&                 name,
    std::string&                 entry,
    std::vector<std::string>&    depends,
    std::vector<std::string>&    optionalDepends,
    PluginManifestResources*     resources,
    PluginManifestInterfaces*    interfaces
) {
    try {
        name  = node["name"] ? node["name"].as<std::string>() : std::string{};
        entry = node["entry"] ? node["entry"].as<std::string>() : std::string{};
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
            // baseDir 为空时 (内置内嵌清单) 保持原样
            auto resolveRel = [&baseDir](const std::string& p) -> std::string {
                if (p.empty()) {
                    return {};
                }
                std::filesystem::path fp{p};
                if (fp.is_absolute()) {
                    return p;
                }
                if (baseDir.empty()) {
                    return p;
                }
                return (baseDir / fp).lexically_normal().string();
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
                            logHint
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
                                logHint,
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
            *interfaces   = PluginManifestInterfaces{};
            auto readList = [&logHint](
                                const YAML::Node&         section,
                                std::vector<std::string>& out,
                                std::string_view          what
                            ) {
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
                            logHint,
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
        XX_LOGE("Parse plugin manifest `{}` failed: {}", logHint, e.what());
        return false;
    }
    if (name.empty() || entry.empty()) {
        XX_LOGE("Plugin manifest `{}` invalid: name/entry required", logHint);
        return false;
    }
    return true;
}
} // namespace

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
        return parseManifestNode(
            node,
            dir,
            yamlPath.string(),
            name,
            entry,
            depends,
            optionalDepends,
            resources,
            interfaces
        );
    } catch (const std::exception& e) {
        XX_LOGE("Parse plugin manifest `{}` failed: {}", yamlPath.string(), e.what());
        return false;
    }
}

bool parsePluginManifestFromString(
    const std::string&           yamlStr,
    const std::filesystem::path& baseDir,
    std::string&                 name,
    std::string&                 entry,
    std::vector<std::string>&    depends,
    std::vector<std::string>&    optionalDepends,
    PluginManifestResources*     resources,
    PluginManifestInterfaces*    interfaces
) {
    if (yamlStr.empty()) {
        return false;
    }
    try {
        auto        node = YAML::Load(yamlStr);
        std::string hint = baseDir.empty() ? std::string{"<builtin manifest>"}
                                           : (baseDir / "plugin.yaml").string();
        return parseManifestNode(
            node,
            baseDir,
            hint,
            name,
            entry,
            depends,
            optionalDepends,
            resources,
            interfaces
        );
    } catch (const std::exception& e) {
        XX_LOGE("Parse builtin manifest failed: {}", e.what());
        return false;
    }
}

bool parseBuiltinManifest(
    std::string_view          pluginName,
    std::string&              name,
    std::string&              entry,
    std::vector<std::string>& depends,
    std::vector<std::string>& optionalDepends,
    PluginManifestResources*  resources,
    PluginManifestInterfaces* interfaces
) {
    auto* m = findBuiltinManifest(pluginName);
    if (!m || pluginStringView2std(m->yaml).empty()) {
        return false;
    }
    // 内嵌清单的资源相对路径无需按插件目录解析 (baseDir 为空)
    return parsePluginManifestFromString(
        std::string{pluginStringView2std(m->yaml)},
        std::filesystem::path{},
        name,
        entry,
        depends,
        optionalDepends,
        resources,
        interfaces
    );
}

std::string resolvePluginEntryPath(const std::filesystem::path& dir, const std::string& entry) {
    // 平台化: manifest 按 Linux 书写 (libfoo.so), Windows/macOS 下修正扩展名
    auto fixExt = [](std::string p) -> std::string {
#if XX_IS_WIN_D
        if (p.ends_with(".so")) {
            p.replace(p.size() - 3, 3, ".dll");
        }
#elif XX_IS_IOS_D || XX_IS_MACOS_D
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

} // namespace pluginxx
