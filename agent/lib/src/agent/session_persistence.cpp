#include "agentxx/agent/session_persistence.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>

namespace agentxx {
namespace agent {

namespace fs = std::filesystem;

namespace {

/// sqlite 数据库存放目录名 (与 CodeGraph 索引库共用 ~/.agentxx/sqlite/)
static constexpr std::string_view kSqliteDirName = "sqlite";
/// 单个 threadId 目录段最大长度 (截断后含分隔符与 hash 尾缀)
/// - Windows 默认 MAX_PATH=260, 需控制单段长度
static constexpr size_t kMaxThreadDirLen = 96;

/// FNV-1a 64 位哈希 (截断用低 32 位 hex 输出)
/// - 仅用于清洗后目录名的短标识, 确定性跨平台一致
static uint64_t fnv1a64(std::string_view s) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : s) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

/// 获取用户主目录 (Unix: $HOME, Windows: %USERPROFILE%); 未设置返回空串
static std::string getUserHomeDir() {
#if XX_IS_WIN_D
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (!home || !*home) {
        return "";
    }
    return std::string{home};
}

/// 默认数据根目录: ~/.agentxx/sqlite/ (取不到主目录时回退系统临时目录)
static std::string defaultRootDir() {
    auto home = getUserHomeDir();
    if (!home.empty()) {
        return (fs::path(home) / agentxx::agent::AgentConfigStatic::agentxxDataDirPath
                / kSqliteDirName)
            .string();
    }
    return (fs::temp_directory_path() / agentxx::agent::AgentConfigStatic::agentxxDataDirPath
            / kSqliteDirName)
        .string();
}

/// Windows 保留设备名 (CON/PRN/AUX/NUL/COM1-9/LPT1-9, 忽略扩展名)
/// - 用作目录名会导致 Windows 无法创建, 需加前缀规避
#if XX_IS_WIN_D
static bool isWindowsReservedName(std::string_view seg) {
    std::string name{seg};
    auto        dot = name.find('.');
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }
    for (auto& c : name) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    static const char* kReserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    for (const auto* r : kReserved) {
        if (name == r) {
            return true;
        }
    }
    return false;
}
#endif

/// 单段清洗: 替换文件系统非法字符为 `_`
/// - Windows 非法字符: < > : " / \ | ? * 及 ASCII 0-31
static std::string sanitizeSegment(std::string_view seg) {
    std::string out;
    out.reserve(seg.size());
    for (char c : seg) {
        if (static_cast<unsigned char>(c) < 0x20 || c == '<' || c == '>' || c == ':'
            || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/// 会话消息状态 SQL (session.db)
static constexpr const char* kSessionSchema = R"sql(
CREATE TABLE IF NOT EXISTS view_message (
    seq  INTEGER PRIMARY KEY AUTOINCREMENT,
    json TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS llm_context (
    id   INTEGER PRIMARY KEY CHECK (id = 1),
    json TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
)sql";

/// share store SQL (share_store.db)
static constexpr const char* kShareStoreSchema = R"sql(
CREATE TABLE IF NOT EXISTS item (
    id    INTEGER PRIMARY KEY,
    value TEXT NOT NULL
);
)sql";

/// meta 键名
static constexpr std::string_view kMetaMsgIdCounter = "msgIdCounter";
static constexpr std::string_view kMetaModelName    = "modelName";

} // namespace

// ---------------------------------------------------------------------------
// SessionPersistence
// ---------------------------------------------------------------------------

SessionPersistence::SessionPersistence(std::string rootDir) :
    rootDir_(rootDir.empty() ? defaultRootDir() : std::move(rootDir)) {}

std::string SessionPersistence::sanitizeThreadId(std::string_view threadId) {
    if (threadId.empty()) {
        return "default";
    }
    auto seg = sanitizeSegment(threadId);
    // 空串 / "." / ".." 不能作为目录名 (路径穿越/上级目录)
    if (seg.empty() || seg == "." || seg == "..") {
        seg = "thread";
    }
    // 是否发生过改写 (需要附加哈希尾缀保证不同 threadId 不碰撞到同一目录)
    bool changed = (seg != threadId);
#if XX_IS_WIN_D
    if (isWindowsReservedName(seg)) {
        seg     = "t_" + seg;
        changed = true;
    }
#endif
    // 超长截断: 保留前部可读信息 + 8 位 hex hash 尾缀防碰撞
    if (seg.size() > kMaxThreadDirLen) {
        seg     = seg.substr(0, kMaxThreadDirLen - 9);
        changed = true;
    }
    if (changed) {
        seg += fmt::format("_{:08x}", static_cast<uint32_t>(fnv1a64(threadId) & 0xffffffffu));
    }
    return seg;
}

SessionPersistence::ThreadDbs& SessionPersistence::dbs(std::string_view threadId) {
    // 目录: {root}/{sanitizedThreadId}/
    auto dir = fs::path(rootDir_) / sanitizeThreadId(threadId);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        throw std::runtime_error{fmt::format(
            "SessionPersistence: create dir {} failed: {}",
            dir.string(),
            ec.message()
        )};
    }

    auto it = dbs_.find(threadId);
    if (it != dbs_.end()) {
        return *it->second;
    }
    auto dbs = std::make_shared<ThreadDbs>();
    // 打开失败 (权限/磁盘) 抛异常, 由上层 catchError 记录日志
    dbs->sessionDb.open((dir / "session.db").string());
    dbs->shareStoreDb.open((dir / "share_store.db").string());
    ensureSchema(dbs->sessionDb, dbs->shareStoreDb);
    auto [insertIt, _] = dbs_.emplace(std::string{threadId}, std::move(dbs));
    return *insertIt->second;
}

void SessionPersistence::ensureSchema(
    agentxx::util::SqliteDb& sessionDb,
    agentxx::util::SqliteDb& shareStoreDb
) {
    sessionDb.exec(kSessionSchema);
    shareStoreDb.exec(kShareStoreSchema);
}

bool SessionPersistence::threadDirExists(std::string_view threadId) const {
    std::error_code ec;
    return fs::exists(fs::path(rootDir_) / sanitizeThreadId(threadId), ec);
}

SessionPersistence::LoadedSession SessionPersistence::loadSession(std::string_view threadId) {
    std::lock_guard<std::mutex> lock(mutex_);
    LoadedSession               out;
    // 目录不存在 = 从未写入过, 直接返回空 (避免只读访问创建目录/空文件)
    if (!threadDirExists(threadId)) {
        return out;
    }
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db = dbs(threadId).sessionDb;

            // 展示历史 (按追加顺序)
            auto stmt = db.prepare("SELECT json FROM view_message ORDER BY seq");
            while (stmt.step()) {
                auto j = neograph::json::parse(stmt.columnText(0));
                out.viewMessages.push_back(ViewMessage::fromJson(j));
            }
            // LLM 上下文 (单行)
            auto ctxStmt = db.prepare("SELECT json FROM llm_context WHERE id = 1");
            if (ctxStmt.step()) {
                out.llmMessages = neograph::json::parse(ctxStmt.columnText(0));
            }
            // meta: msgIdCounter / modelName
            auto metaStmt = db.prepare("SELECT key, value FROM meta");
            while (metaStmt.step()) {
                auto key = metaStmt.columnText(0);
                if (key == kMetaMsgIdCounter) {
                    out.msgIdCounter = static_cast<uint64_t>(metaStmt.columnInt64(1));
                } else if (key == kMetaModelName) {
                    out.modelName = metaStmt.columnText(1);
                }
            }
            // 兜底: 老数据无 msgIdCounter 记录时按历史条数恢复
            // (历史 append-only, id 连续分配, 条数即最后序号)
            if (out.msgIdCounter == 0) {
                out.msgIdCounter = out.viewMessages.size();
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionPersistence: loadSession({}) failed: {}", threadId, errmsg);
            out = LoadedSession{};
            return false;
        }
    );
    return out;
}

void SessionPersistence::appendViewMessage(
    std::string_view   threadId,
    const ViewMessage& msg,
    uint64_t           msgIdCounter
) {
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db = dbs(threadId).sessionDb;
            db.beginImmediate();
            bool inTx = true;
            try {
                auto insert = db.prepare("INSERT INTO view_message(json) VALUES (?)");
                insert.bindText(1, msg.toJson().dump());
                insert.step();

                // UPSERT 计数: 新线程首条消息时 meta 不存在, 需 INSERT
                auto meta = db.prepare(
                    "INSERT INTO meta(key, value) VALUES (?, ?) "
                    "ON CONFLICT(key) DO UPDATE SET value = excluded.value"
                );
                meta.bindText(1, kMetaMsgIdCounter);
                meta.bindInt64(2, static_cast<int64_t>(msgIdCounter));
                meta.step();

                db.commit();
                inTx = false;
            } catch (...) {
                if (inTx) {
                    agentxx::util::catchError<bool>(
                        [&]() -> bool {
                            db.rollback();
                            return true;
                        },
                        [](std::string) -> bool { return false; }
                    );
                }
                throw;
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionPersistence: appendViewMessage({}) failed: {}", threadId, errmsg);
            return false;
        }
    );
}

void SessionPersistence::saveLlmMessages(
    std::string_view    threadId,
    const neograph::json& llmMessages
) {
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db = dbs(threadId).sessionDb;
            db.beginImmediate();
            bool inTx = true;
            try {
                // 整表替换 (单行上下文)
                db.exec("DELETE FROM llm_context");
                auto insert = db.prepare("INSERT INTO llm_context(id, json) VALUES (1, ?)");
                insert.bindText(1, llmMessages.dump());
                insert.step();
                db.commit();
                inTx = false;
            } catch (...) {
                if (inTx) {
                    agentxx::util::catchError<bool>(
                        [&]() -> bool {
                            db.rollback();
                            return true;
                        },
                        [](std::string) -> bool { return false; }
                    );
                }
                throw;
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionPersistence: saveLlmMessages({}) failed: {}", threadId, errmsg);
            return false;
        }
    );
}

void SessionPersistence::saveModelName(std::string_view threadId, std::string_view modelName) {
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db = dbs(threadId).sessionDb;
            auto  stmt = db.prepare(
                "INSERT INTO meta(key, value) VALUES (?, ?) "
                "ON CONFLICT(key) DO UPDATE SET value = excluded.value"
            );
            stmt.bindText(1, kMetaModelName);
            stmt.bindText(2, modelName);
            stmt.step();
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionPersistence: saveModelName({}) failed: {}", threadId, errmsg);
            return false;
        }
    );
}

// ---------------------------------------------------------------------------
// share store
// ---------------------------------------------------------------------------

SessionPersistence::LoadedShareStore SessionPersistence::loadShareStore(std::string_view threadId) {
    std::lock_guard<std::mutex> lock(mutex_);
    LoadedShareStore            out;
    // 目录不存在 = 从未写入过, 直接返回空
    if (!threadDirExists(threadId)) {
        return out;
    }
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db = dbs(threadId).shareStoreDb;
            auto  stmt = db.prepare("SELECT id, value FROM item ORDER BY id");
            while (stmt.step()) {
                out.items[static_cast<size_t>(stmt.columnInt64(0))] = stmt.columnText(1);
            }
            out.nextId = out.items.empty() ? 1 : (out.items.rbegin()->first + 1);
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionPersistence: loadShareStore({}) failed: {}", threadId, errmsg);
            out = LoadedShareStore{};
            return false;
        }
    );
    return out;
}

std::optional<std::string>
    SessionPersistence::getShareStoreItem(std::string_view threadId, size_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<std::string>  out;
    // 目录不存在 = 从未写入过, 直接返回 nullopt
    if (!threadDirExists(threadId)) {
        return out;
    }
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db  = dbs(threadId).shareStoreDb;
            auto  stmt = db.prepare("SELECT value FROM item WHERE id = ?");
            stmt.bindInt64(1, static_cast<int64_t>(id));
            if (stmt.step()) {
                out = stmt.columnText(0);
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionPersistence: getShareStoreItem({}, {}) failed: {}", threadId, id, errmsg);
            return false;
        }
    );
    return out;
}

void SessionPersistence::setShareStoreItem(
    std::string_view threadId,
    size_t           id,
    std::string_view value
) {
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db  = dbs(threadId).shareStoreDb;
            auto  stmt = db.prepare(
                "INSERT INTO item(id, value) VALUES (?, ?) "
                "ON CONFLICT(id) DO UPDATE SET value = excluded.value"
            );
            stmt.bindInt64(1, static_cast<int64_t>(id));
            stmt.bindText(2, value);
            stmt.step();
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionPersistence: setShareStoreItem({}, {}) failed: {}", threadId, id, errmsg);
            return false;
        }
    );
}

size_t SessionPersistence::addShareStoreItem(std::string_view threadId, std::string_view value) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t                      out = 0;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db = dbs(threadId).shareStoreDb;
            // 自增 id: 取现有最大 id + 1, 重启后延续; 与内存版
            // (ThreadShareStore::storeId 递增) 语义一致且更稳健
            auto stmt = db.prepare(
                "INSERT INTO item(id, value) "
                "VALUES ((SELECT COALESCE(MAX(id), 0) + 1 FROM item), ?)"
            );
            stmt.bindText(1, value);
            stmt.step();
            out = static_cast<size_t>(db.lastInsertRowid());
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionPersistence: addShareStoreItem({}) failed: {}", threadId, errmsg);
            return false;
        }
    );
    return out;
}

void SessionPersistence::removeShareStoreItem(std::string_view threadId, size_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db  = dbs(threadId).shareStoreDb;
            auto  stmt = db.prepare("DELETE FROM item WHERE id = ?");
            stmt.bindInt64(1, static_cast<int64_t>(id));
            stmt.step();
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE(
                "SessionPersistence: removeShareStoreItem({}, {}) failed: {}",
                threadId,
                id,
                errmsg
            );
            return false;
        }
    );
}

} // namespace agent
} // namespace agentxx
