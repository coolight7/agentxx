#pragma once

#include <string>
#include <string_view>

namespace agentxx {
namespace plugin {

/// 插件数据目录规范 (主程序侧唯一实现)
///
/// 目录布局 (都相对主程序的数据根 `data_dir`):
/// - `{data_dir}/plugins/data/{插件 id}/` 插件自己的持久化 (缓存 / 索引 / 下载 / 媒体)
/// - `{data_dir}/plugins/installed/`      以后"安装式插件"的落点 (预留, 当前没有写入方)
///
/// 约定:
/// - `data_dir` 为空 (主程序没配持久化) 时一律返回空串: 插件据此降级为不落盘,
///   **不**回退到 `~/.agentxx` —— 与 `AgentConfig::dataDir` "空 = 不落盘" 的语义一致
/// - 目录名只保留 `[A-Za-z0-9._-]`, 其它字符 (含路径分隔符与 `..`) 换成 `_`,
///   插件 id 因此不可能把数据写出插件数据根目录
/// - 目录只按插件 id 划分, **不按 agent 划分**: 同一个插件被多个 agent 挂载时仍是
///   同一份数据 (插件实例进程一份, 见 plugins.md 的多实例约定)
/// - 插件经 `get_own_info` 的 `dataDir` 字段拿自己的目录 (SDK 是
///   `pluginDataDirAsync()`), 不自己拼 `data_dir`

/// 插件数据根目录 `{data_dir}/plugins/data` (data_dir 为空返回空串)
std::string pluginDataRoot(std::string_view dataDir);

/// 插件安装根目录 `{data_dir}/plugins/installed` (预留; data_dir 为空返回空串)
std::string pluginInstallRoot(std::string_view dataDir);

/// 插件 id → 目录名 (安全化)
///
/// - 保留 ASCII 字母/数字与 `.` `_` `-`, 其余字符换成 `_`
/// - `..` 与全点号的名字会变成 `_` / 加上前缀, 不会逃出父目录
/// - 空 id 返回 `_`
/// - 只有 id 被改过时才追加 `-<原 id 的短哈希>`: 既保留可读前缀, 又避免
///   `a/b` 与 `a_b` 两个不同插件共用一个目录; 合法 id 的目录名不变
std::string sanitizePluginId(std::string_view pluginId);

/// 单个插件的数据目录 `{data_dir}/plugins/data/{目录名}` (data_dir 为空返回空串)
std::string pluginDataDirOf(std::string_view dataDir, std::string_view pluginId);

/// 取插件数据目录并确保它存在 (幂等)
///
/// - `return` 插件数据目录 (绝对化、正斜杠); `data_dir` 为空或建目录失败返回空串
/// - `args`:
///     - [dataDir] 主程序数据根 (`AgentConfig::dataDir`)
///     - [pluginId] 插件 id (内部会安全化)
///     - [error] 失败原因 (可为空指针)
std::string ensurePluginDataDir(
    std::string_view dataDir,
    std::string_view pluginId,
    std::string*     error = nullptr
);

} // namespace plugin
} // namespace agentxx
