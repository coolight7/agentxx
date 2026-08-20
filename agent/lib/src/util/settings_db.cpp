#include "agentxx/util/settings_db.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include <filesystem>
#include <system_error>

namespace agentxx {
namespace util {

namespace fs = std::filesystem;

namespace {

/// 全局设置表 schema (KV)
static constexpr const char* kSettingsSchema = R"sql(
CREATE TABLE IF NOT EXISTS setting (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
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
            // INSERT OR REPLACE 语义 (主键冲突时覆盖)
            auto stmt = db_.prepare("INSERT INTO setting(key, value) VALUES(?, ?) "
                                    "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
            stmt.bindText(1, key);
            stmt.bindText(2, value);
            stmt.step();
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SettingsDb: set '{}' failed: {}", key, errmsg);
            return false;
        }
    );
}

int64_t SettingsDb::getInt64(std::string_view key, int64_t def) {
    auto v = get(key);
    if (!v.has_value()) {
        return def;
    }
    int64_t parsed = def;
    if (agentxx::util::parseNumberFromString(*v, parsed).ec != std::errc{}) {
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
