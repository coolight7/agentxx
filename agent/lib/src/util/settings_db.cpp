#include "agentxx/util/settings_db.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/util/exception.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <filesystem>
#include <system_error>

namespace agentxx {
namespace util {

namespace fs = std::filesystem;

namespace {

/// 全局设置表 schema (KV + 乐观版本)
static constexpr const char* kSettingsSchema = R"sql(
CREATE TABLE IF NOT EXISTS setting (
    key     TEXT PRIMARY KEY,
    value   TEXT NOT NULL,
    version INTEGER NOT NULL DEFAULT 0
);
)sql";

} // namespace

SettingsDb::SettingsDb(std::string dbPath) :
    dbPath_(
        dbPath.empty() ? agentxx::agent::AgentConfigStatic::getGlobalSettingsDbPath("")
                       : std::move(dbPath)
    ) {}

bool SettingsDb::ensureOpen() {
    if (db_.isOpen()) {
        return true;
    }
    // 懒创建父目录: 首次使用时才落盘 (与 SessionStore 行为一致)
    std::error_code ec;
    auto            dir = fs::path(dbPath_).parent_path();
    fs::create_directories(dir, ec);
    if (ec) {
        XX_LOGE("SettingsDb: create dir {} failed: {}", dir.string(), ec.message());
        return false;
    }
    return agentxx::util::catchError<bool>(
        [&]() -> bool {
            db_.open(dbPath_);
            db_.exec(kSettingsSchema);
            // 老库 (只有 key/value 两列) 补 version 列: 幂等, 已有数据版本从 0 起算
            bool hasVersion = false;
            {
                auto stmt = db_.prepare("PRAGMA table_info(setting)");
                while (stmt.step()) {
                    if (stmt.columnText(1) == "version") {
                        hasVersion = true;
                        break;
                    }
                }
            }
            if (!hasVersion) {
                db_.exec("ALTER TABLE setting ADD COLUMN version INTEGER NOT NULL DEFAULT 0");
                XX_LOGI("SettingsDb: migrated setting table (added version column)");
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SettingsDb: open {} failed: {}", dbPath_, errmsg);
            return false;
        }
    );
}

std::optional<std::string> SettingsDb::get(std::string_view key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureOpen()) {
        return std::nullopt;
    }
    return agentxx::util::catchError<std::optional<std::string>>(
        [&]() -> std::optional<std::string> {
            auto stmt = db_.prepare("SELECT value FROM setting WHERE key = ?");
            stmt.bindText(1, key);
            if (!stmt.step()) {
                return std::nullopt;
            }
            return std::optional<std::string>{stmt.columnText(0)};
        },
        [&](std::string errmsg) -> std::optional<std::string> {
            XX_LOGE("SettingsDb: get '{}' failed: {}", key, errmsg);
            return std::nullopt;
        }
    );
}

bool SettingsDb::set(std::string_view key, std::string_view value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureOpen()) {
        return false;
    }
    return agentxx::util::catchError<bool>(
        [&]() -> bool {
            // 无条件写入: 版本号在库中当前值上自增 (一次事务内读取+提交,
            // 与其它写入者并发时由 BEGIN IMMEDIATE 串行化)
            db_.beginImmediate();
            bool inTx = true;
            try {
                const int64_t current = versionLocked(key);
                writeLocked(key, value, current + 1);
                db_.commit();
                inTx = false;
            } catch (...) {
                if (inTx) {
                    agentxx::util::catchError<bool>(
                        [&]() -> bool {
                            db_.rollback();
                            return true;
                        },
                        [](std::string) -> bool {
                            return false;
                        }
                    );
                }
                throw;
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SettingsDb: set '{}' failed: {}", key, errmsg);
            return false;
        }
    );
}

std::optional<std::string> SettingsDb::getVersioned(std::string_view key, int64_t* outVersion) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (outVersion) {
        *outVersion = 0;
    }
    if (!ensureOpen()) {
        return std::nullopt;
    }
    return agentxx::util::catchError<std::optional<std::string>>(
        [&]() -> std::optional<std::string> {
            auto stmt = db_.prepare("SELECT value, version FROM setting WHERE key = ?");
            stmt.bindText(1, key);
            if (!stmt.step()) {
                return std::nullopt;
            }
            if (outVersion) {
                *outVersion = stmt.columnInt64(1);
            }
            return std::optional<std::string>{stmt.columnText(0)};
        },
        [&](std::string errmsg) -> std::optional<std::string> {
            XX_LOGE("SettingsDb: getVersioned '{}' failed: {}", key, errmsg);
            return std::nullopt;
        }
    );
}

int64_t SettingsDb::version(std::string_view key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureOpen()) {
        return 0;
    }
    return agentxx::util::catchError<int64_t>(
        [&]() -> int64_t {
            return versionLocked(key);
        },
        [&](std::string errmsg) -> int64_t {
            XX_LOGE("SettingsDb: version '{}' failed: {}", key, errmsg);
            return 0;
        }
    );
}

int64_t SettingsDb::versionLocked(std::string_view key) {
    auto stmt = db_.prepare("SELECT version FROM setting WHERE key = ?");
    stmt.bindText(1, key);
    if (!stmt.step()) {
        return 0;
    }
    return stmt.columnInt64(0);
}

void SettingsDb::writeLocked(std::string_view key, std::string_view value, int64_t version) {
    // INSERT OR REPLACE 语义 (主键冲突时覆盖)
    auto stmt = db_.prepare(
        "INSERT INTO setting(key, value, version) VALUES(?, ?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value, version = excluded.version"
    );
    stmt.bindText(1, key);
    stmt.bindText(2, value);
    stmt.bindInt64(3, version);
    stmt.step();
}

SettingsDb::WriteResult
    SettingsDb::setVersioned(std::string_view key, std::string_view value, int64_t expectedVersion) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureOpen()) {
        return WriteResult{WriteStatus::Failed, 0};
    }
    return agentxx::util::catchError<WriteResult>(
        [&]() -> WriteResult {
            db_.beginImmediate();
            bool inTx = true;
            try {
                const int64_t current = versionLocked(key);
                if (current != expectedVersion) {
                    // 期间有其它写入者提交过: 不覆盖, 回滚后把当前版本返回给调用方
                    db_.rollback();
                    inTx = false;
                    XX_LOGD(
                        "SettingsDb: setVersioned '{}' conflict (expected={}, actual={})",
                        key,
                        expectedVersion,
                        current
                    );
                    return WriteResult{WriteStatus::Conflict, current};
                }
                const int64_t next = current + 1;
                writeLocked(key, value, next);
                db_.commit();
                inTx = false;
                return WriteResult{WriteStatus::Ok, next};
            } catch (...) {
                if (inTx) {
                    agentxx::util::catchError<bool>(
                        [&]() -> bool {
                            db_.rollback();
                            return true;
                        },
                        [](std::string) -> bool {
                            return false;
                        }
                    );
                }
                throw;
            }
        },
        [&](std::string errmsg) -> WriteResult {
            XX_LOGE("SettingsDb: setVersioned '{}' failed: {}", key, errmsg);
            return WriteResult{WriteStatus::Failed, 0};
        }
    );
}

int64_t SettingsDb::getInt64(std::string_view key, int64_t def) {
    auto v = get(key);
    if (!v.has_value()) {
        return def;
    }
    int64_t parsed = def;
    if (utilxx_base::parseNumberFromString(*v, parsed).ec != std::errc{}) {
        return def;
    }
    return parsed;
}

bool SettingsDb::setInt64(std::string_view key, int64_t value) {
    return set(key, std::to_string(value));
}

bool SettingsDb::getBool(std::string_view key, bool def) {
    auto v = get(key);
    if (!v.has_value()) {
        return def;
    }
    return *v == "1";
}

bool SettingsDb::setBool(std::string_view key, bool value) {
    return set(key, value ? "1" : "0");
}

} // namespace util
} // namespace agentxx
