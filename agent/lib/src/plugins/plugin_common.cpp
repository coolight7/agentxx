/*
 * plugin_common.cpp —— 插件系统公共设施实现 (见 plugin_common.h)
 * - 原实现分别位于 plugin_manager.cpp / client_plugin_manager.cpp,
 *   提取后两侧共用, 避免行为漂移
 */
#include "agentxx/plugin/plugin_common.h"

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
    std::vector<std::string>&    optionalDepends
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
    auto entryPath = fixExt((dir / entry).lexically_normal().string());
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

} // namespace plugin
} // namespace agentxx
