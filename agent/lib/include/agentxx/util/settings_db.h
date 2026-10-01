#pragma once

#include "agentxx/util/sqlite.h"
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace agentxx {
namespace util {

/// 全局设置 SQLite 存储 (KV 表)
///
/// - 默认路径: {dataDir}/sqlite/global.db (dataDir 见 AgentConfig::dataDir,
///   为空时 ~/.agentxx/, 取不到用户主目录时回退系统临时目录)
/// - 表结构: setting(key TEXT PRIMARY KEY, value TEXT NOT NULL,
///   version INTEGER NOT NULL DEFAULT 0)
/// - 懒打开: 首次读写时自动创建父目录并打开数据库 (失败仅记日志)
/// - 线程安全: 内部互斥锁保护所有访问, 可从任意线程并发调用
/// - 失败语义: 设置持久化失败不致命 (丢失设置不影响运行),
///   set 系列返回 false / get 系列返回默认值, 均记录错误日志
///
/// 版本与并发 (计划 STO-11): 每次写入把该条目的 `version` 加一; 需要
/// "读-改-写"不丢更新的调用方用 [version] 读版本后再用 [setVersioned] 带
/// 期望版本提交, 期间有其它写入者提交过就返回冲突而不是静默覆盖。
class SettingsDb {
public:

    /// 写入结果状态
    enum class WriteStatus {
        Ok = 0,   ///< 写入成功 (返回的 version 为写入后的新版本)
        Conflict, ///< 版本冲突: 条目已被其它写入者更新, 本次未写入
        Failed,   ///< 打开或写库失败 (已记录日志)
    };

    /// 带版本写入的结果
    struct WriteResult {
        WriteStatus status  = WriteStatus::Failed;
        /// 版本号: Ok 时为写入后的新版本; Conflict 时为库中当前版本;
        /// Failed 时为 0
        int64_t     version = 0;
    };

    /// - [dbPath] 数据库文件路径; 为空使用默认 {dataDir}/sqlite/global.db
    explicit SettingsDb(std::string dbPath = "");

    SettingsDb(const SettingsDb&)            = delete;
    SettingsDb& operator=(const SettingsDb&) = delete;

    /// 当前数据库文件路径 (测试可校验路径)
    const std::string& dbPath() const noexcept {
        return dbPath_;
    }

    /// 读取条目; 不存在/打开失败返回 nullopt (仅记日志)
    std::optional<std::string> get(std::string_view key);

    /// 读取条目并输出其版本号 (供"读-改-写"流程)
    /// - `args`:
    ///     - [key] 条目键
    ///     - [outVersion] 版本号输出; 条目不存在时写 0 (可传 nullptr)
    /// - `return` 与 [get] 相同 (不存在/失败返回 nullopt)
    std::optional<std::string> getVersioned(std::string_view key, int64_t* outVersion);

    /// 条目当前版本号 (不存在/读取失败返回 0)
    int64_t version(std::string_view key);

    /// 覆盖/新增条目 (无条件写入), 版本号加一; 失败返回 false (仅记日志)
    bool set(std::string_view key, std::string_view value);

    /// 乐观写入: 仅当条目当前版本等于 [expectedVersion] 时写入
    /// - 条目不存在时其版本视为 0 (用 expectedVersion = 0 表示"新建")
    /// - 版本不等于期望值时返回 Conflict, 且不修改库中内容
    /// - 写入成功后版本号变为 expectedVersion + 1 (与 [set] 的递增规则一致)
    WriteResult
        setVersioned(std::string_view key, std::string_view value, int64_t expectedVersion);

    /// 读取整数 (非法/不存在返回默认值)
    int64_t getInt64(std::string_view key, int64_t def = 0);

    /// 写入整数 (以十进制文本存储)
    bool setInt64(std::string_view key, int64_t value);

    /// 读取布尔 (存储文本为 "1"/"0")
    /// - 条目不存在返回默认值; 已存在但文本非 "1" 时视为 false
    bool getBool(std::string_view key, bool def = false);

    /// 写入布尔 (存储文本为 "1"/"0")
    bool setBool(std::string_view key, bool value);

private:

    /// 懒打开数据库 + 建表/迁移 (幂等); 失败返回 false (已记录日志)
    bool ensureOpen();

    /// 读取条目版本 (调用方必须持有 [mutex_] 且库已打开); 不存在返回 0
    int64_t versionLocked(std::string_view key);

    /// 写入条目并指定新版本 (调用方必须持有 [mutex_] 且库已打开)
    void writeLocked(std::string_view key, std::string_view value, int64_t version);

    std::string dbPath_;
    std::mutex  mutex_;
    SqliteDb    db_;
};

} // namespace util
} // namespace agentxx
