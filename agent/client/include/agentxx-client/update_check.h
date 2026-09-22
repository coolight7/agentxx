/// agentxx-client —— 启动更新检查 (GitHub Release)
///
/// TUI 启动后 (可经设置项 `启动时检查更新` 关闭) 查询项目最新发布版本, 若比当前
/// 运行版本新则在界面上提示 (Info 侧边栏底部 + toast)。
///
/// 设计要点:
/// - 只做一次 GET, 请求不跟随重定向 —— GitHub 的 `/releases/latest` 必定 302 到
///   `.../releases/tag/<tag>`, 从 Location 头即可取出最新版本标签与发布页 URL;
/// - 若对端直接返回 JSON (如 GitHub API 的 releases/latest), 回退读其中的
///   `tag_name` 字段, 使本模块也能用于 API 端点;
/// - 任何失败 (无网络/超时/解析失败) 都返回 `ok=false` 而不是抛异常: 更新检查是
///   附加提示, 不得影响正常启动与使用;
/// - 版本比较只按三段数字 (major.minor.patch), 忽略预发布后缀 (`-rc1` 等)。
#pragma once

#include "agentxx/version.h"
#include "utilxx_base/asio_error.h" // 全局 asio 别名 + AsioErrorCode/AsioSystemError
#include "asio/awaitable.hpp"
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace agentxx {
namespace client {

/// 最新发布页 URL (GitHub 会 302 到具体 tag 页; 见 [checkLatestRelease])
inline constexpr std::string_view kLatestReleaseUrl{
    "https://github.com/coolight7/agentxx/releases/latest"
};

/// 全部发布页 URL (更新提示的落点; 用户可复制/打开)
inline constexpr std::string_view kReleasesUrl{"https://github.com/coolight7/agentxx/releases"};

/// 三段式版本号 (major, minor, patch)
using VersionTriple = std::array<int, 3>;

/// 解析版本标签为三段式版本号
/// - 接受 `v1.2.3` / `1.2.3` / `v1.2` (缺位补 0) / `1.2.3-rc1` (忽略 `-` 之后);
///   前缀 `v`|`V` 与首尾空白被忽略
/// - 遇到非法字符 (非数字/`.`/-) 即认为标签不合法
///
/// - `return` 解析成功返回版本号; 无任何数字段时返回 nullopt
std::optional<VersionTriple> parseVersionTag(std::string_view tag) noexcept;

/// 比较两个版本号
/// - `return` a > b 返回正数, a == b 返回 0, a < b 返回负数
int compareVersion(const VersionTriple& a, const VersionTriple& b) noexcept;

/// 更新检查结果
struct UpdateCheckResult {
    /// 检查请求本身是否成功 (网络与解析均无错)
    bool ok = false;
    /// 是否存在比当前运行版本更新的发布 (仅当 ok 且两版本均可解析时可能为 true)
    bool hasUpdate = false;
    /// 最新发布标签 (如 "v0.2.0"; ok=false 时为空)
    std::string latestTag;
    /// 最新发布页 URL (可直接复制/打开; ok=false 时为空)
    std::string url;
    /// 最新版本号 (标签解析成功时有值)
    std::optional<VersionTriple> latestVersion;
    /// 失败原因 (ok=false 时非空)
    std::string error;
};
/// 查询指定 URL 的最新发布并与当前运行版本比较
/// - `url` 默认 [kLatestReleaseUrl]; 可传 GitHub API 端点 (`.../api.github.com/...`)
/// - 请求配置: 不跟随重定向 (需要读 Location), 连接/读块超时各 8 秒 —— 启动路径上
///   的探测不应长时间占用连接
/// - `hasUpdate` 按 `latestTag > agentxx::kVersion` 判定 (标签或当前版本无法解析时
///   保持 false, 即"不提示更新")
/// - `return` 结果; 失败时 `ok=false` 且 `error` 说明原因 (不抛异常)
asio::awaitable<UpdateCheckResult> checkLatestRelease(std::string_view url = kLatestReleaseUrl);

/// 查询 [kLatestReleaseUrl] 并与当前版本比较 (等价 `checkLatestRelease(kLatestReleaseUrl)`)
/// - `return` `hasUpdate=true` 表示存在更新版本 (顺带填好 `latestTag` / `url`)
asio::awaitable<UpdateCheckResult> checkLatestReleaseForCurrentVersion();

} // namespace client
} // namespace agentxx
