/// 插件数据目录规范的唯一实现 (见 agentxx/plugin/plugin_paths.h)
///
/// 为什么放在主程序而不是每个插件里: 目录布局要能整体改动 (例如以后加版本段),
/// 插件只认"主程序给我的那个目录", 不自己拼 `data_dir/xxx`。
#include "agentxx/plugin/plugin_paths.h"

#include "utilxx_base/string_util.h"

#include <cstdint>
#include <filesystem>
#include <system_error>

namespace agentxx {
namespace plugin {

namespace {

namespace fs = std::filesystem;

/// 插件数据/安装在数据根下的父目录名
constexpr std::string_view kPluginsDirName   = "plugins";
constexpr std::string_view kDataDirName      = "data";
constexpr std::string_view kInstalledDirName = "installed";

/// 数据根转本地路径 (空串返回空 path)
fs::path dataRootPath(std::string_view dataDir) {
    if (dataDir.empty()) {
        return {};
    }
    return utilxx_base::utf8ToPath(dataDir);
}

/// 拼一个子路径并输出为 UTF-8 正斜杠形式
std::string joinUtf8(const fs::path& base, std::string_view child) {
    if (base.empty()) {
        return {};
    }
    return utilxx_base::pathToUtf8Generic(base / utilxx_base::utf8ToPath(child));
}

/// 拼接原始 id 的短哈希 (8 位十六进制; 只用于避免不同 id 撞到同一目录)
std::string shortHash(std::string_view text) {
    uint32_t h = 2166136261u;
    for (const char c : text) {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    return fmt::format("{:08x}", h);
}

} // namespace

std::string pluginDataRoot(std::string_view dataDir) {
    const fs::path root = dataRootPath(dataDir);
    if (root.empty()) {
        return {};
    }
    return joinUtf8(root, kPluginsDirName) + "/" + std::string{kDataDirName};
}

std::string pluginInstallRoot(std::string_view dataDir) {
    const fs::path root = dataRootPath(dataDir);
    if (root.empty()) {
        return {};
    }
    return joinUtf8(root, kPluginsDirName) + "/" + std::string{kInstalledDirName};
}

std::string sanitizePluginId(std::string_view pluginId) {
    std::string out;
    out.reserve(pluginId.size() + 9);
    bool changed = false;
    for (const char c : pluginId) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                          || c == '.' || c == '_' || c == '-';
        if (keep) {
            out += c;
        } else {
            out += '_';
            changed = true;
        }
    }
    if (out.empty()) {
        return "_";
    }
    // `.` / `..` (以及全点号) 在文件系统上有特殊含义, 加前缀避开
    if (out.find_first_not_of('.') == std::string::npos) {
        return "_" + out;
    }
    // 原名被改过时追加原 id 的短哈希: `a/b` 与 `a_b` 不会落进同一目录
    if (changed) {
        out += "-" + shortHash(pluginId);
    }
    return out;
}

std::string pluginDataDirOf(std::string_view dataDir, std::string_view pluginId) {
    const std::string root = pluginDataRoot(dataDir);
    if (root.empty()) {
        return {};
    }
    return root + "/" + sanitizePluginId(pluginId);
}

std::string ensurePluginDataDir(
    std::string_view dataDir,
    std::string_view pluginId,
    std::string*     error
) {
    const std::string dir = pluginDataDirOf(dataDir, pluginId);
    if (error != nullptr) {
        error->clear();
    }
    if (dir.empty()) {
        if (error != nullptr) {
            *error = "data_dir is empty (no persistence)";
        }
        return {};
    }
    std::error_code ec;
    fs::create_directories(utilxx_base::utf8ToPath(dir), ec);
    if (ec) {
        if (error != nullptr) {
            *error = ec.message();
        }
        return {};
    }
    return dir;
}

} // namespace plugin
} // namespace agentxx
