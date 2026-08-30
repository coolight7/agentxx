#pragma once

#include "agentxx/util/exception.h"
#include "fmt/format.h"
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace agentxx {
namespace agent {

class AgentConfigStatic {
public:

    AgentConfigStatic(int _);

    /// 默认数据根目录名 (置于用户主目录下)
    inline static constexpr std::string_view agentxxDataDirPath = ".agentxx";

    /// yaml data_dir 关键字: 使用当前系统数据目录 (平台惯例)
    /// - Linux/macOS: ~/.agentxx/ (同 defaultDataDir)
    /// - Windows: %APPDATA%/agentxx/ (取不到 APPDATA 时回退 defaultDataDir)
    inline static constexpr std::string_view kDefaultDataDirKey = "default";

    /// 系统数据目录 (平台惯例):
    /// - Linux/macOS: ~/.agentxx/ (同 defaultDataDir)
    /// - Windows: {APPDATA}/agentxx/ (APPDATA 未设置时回退 defaultDataDir)
    /// - 供 yaml data_dir: default 关键字使用 (tui/cli 模式)
    inline static std::string systemDataDir() noexcept {
#if XX_IS_WIN_D
        const char* appdata = std::getenv("APPDATA");
        if (appdata && *appdata) {
            return (std::filesystem::path(appdata) / "agentxx").string();
        }
#endif
        return defaultDataDir();
    }

    /// 获取用户主目录 (Unix: $HOME, Windows: %USERPROFILE%); 未设置返回空串
    inline static std::string getUserHomeDir() noexcept {
#if XX_IS_WIN_D
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        return home && *home ? std::string{home} : std::string{};
    }

    /// 默认数据根目录: ~/.agentxx/ (取不到用户主目录时回退系统临时目录)
    inline static std::string defaultDataDir() noexcept {
        auto home = getUserHomeDir();
        if (!home.empty()) {
            return (std::filesystem::path(home) / agentxxDataDirPath).string();
        }
        std::error_code ec;
        auto            tmp = std::filesystem::temp_directory_path(ec);
        return (tmp / agentxxDataDirPath).string();
    }

    /// 数据根目录: dataDir 为空时使用默认 ~/.agentxx/
    /// - 注意: 该函数仅做路径解析 (供显式构造场景/默认值展示使用);
    ///   agent 是否落盘由调用方判断 dataDir 是否为空
    ///   (dataDir 为空 = 不持久化设置/会话/codegraph, 见 AgentConfig::dataDir)
    /// - 输入为相对路径时按当前工作目录解析为绝对路径, 并词法规范化
    inline static std::string getDataDir(std::string_view dataDir) noexcept {
        if (dataDir.empty()) {
            return defaultDataDir();
        }
        std::string s{dataDir};
        // P1-15: 相对路径 normalize (展开 ~/ 相对 CWD, 避免 lib 层拼接错误)
        // 复用 string_util 的绝对化逻辑, 此处仅做轻量处理避免头依赖循环
        if (!std::filesystem::path(s).is_absolute()) {
            std::error_code ec;
            auto            abs = std::filesystem::absolute(s, ec);
            if (!ec) {
                return abs.lexically_normal().generic_string();
            }
        }
        return std::filesystem::path(s).lexically_normal().generic_string();
    }

    /// sqlite 数据目录: {dataDir}/sqlite/
    /// - 全局设置/会话/codegraph 索引等 sqlite 数据统一存放于此
    inline static std::string getSqliteDir(std::string_view dataDir) noexcept {
        return (std::filesystem::path(getDataDir(dataDir)) / "sqlite").string();
    }

    /// 会话数据目录: {dataDir}/sqlite/sessions/
    /// - 每个会话数据: {sessions}/{sanitizedThreadId}/session.db (单库, 含 item)
    inline static std::string getSessionsDir(std::string_view dataDir) noexcept {
        return (std::filesystem::path(getSqliteDir(dataDir)) / "sessions").string();
    }

    /// 全局设置数据库路径: {dataDir}/sqlite/global.db
    inline static std::string getGlobalSettingsDbPath(std::string_view dataDir) noexcept {
        return (std::filesystem::path(getSqliteDir(dataDir)) / "global.db").string();
    }

    inline static std::string getResultPath(std::string_view parent) noexcept {
        return fmt::format("{}/results/{}", agentxxDataDirPath, parent);
    }

    inline static std::string getCurrentWorkPath() noexcept {
        return agentxx::util::catchError<std::string>(
            []() -> std::string {
                return std::filesystem::current_path().generic_string();
            },
            [](std::string errinfo) -> std::string {
                XX_LOGW("AgentConfigStatic::getCurrentWorkPath() faild: {}", errinfo);
                return "";
            }
        );
    }
};

} // namespace agent
} // namespace agentxx