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
/// - 表结构: setting(key TEXT PRIMARY KEY, value TEXT NOT NULL)
/// - 懒打开: 首次读写时自动创建父目录并打开数据库 (失败仅记日志)
/// - 线程安全: 内部互斥锁保护所有访问, 可从任意线程并发调用
/// - 失败语义: 设置持久化失败不致命 (丢失设置不影响运行),
///   set 系列返回 false / get 系列返回默认值, 均记录错误日志
class SettingsDb {
public:

    /// @param dbPath 数据库文件路径; 为空使用默认 {dataDir}/sqlite/global.db
    explicit SettingsDb(std::string dbPath = "");

    SettingsDb(const SettingsDb&)            = delete;
    SettingsDb& operator=(const SettingsDb&) = delete;

    /// 当前数据库文件路径 (测试可校验路径)
    const std::string& dbPath() const noexcept {
        return dbPath_;
    }

    /// 读取条目; 不存在/打开失败返回 nullopt (仅记日志)
    std::optional<std::string> get(std::string_view key);

    /// 覆盖/新增条目; 失败返回 false (仅记日志)
    bool set(std::string_view key, std::string_view value);

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

    /// 懒打开数据库 + 建表 (幂等); 失败返回 false (已记录日志)
    bool ensureOpen();

    std::string dbPath_;
    std::mutex  mutex_;
    SqliteDb    db_;
};

} // namespace util
} // namespace agentxx
