#include "agentxx/agent/session_persistence.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>

namespace agentxx {
namespace agent {

namespace fs = std::filesystem;

namespace {

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

/// 默认数据根目录: {dataDir}/sqlite/sessions/
/// - dataDir 为空时回退 ~/.agentxx/ (取不到用户主目录时回退系统临时目录)
static std::string defaultRootDir() {
    return agentxx::agent::AgentConfigStatic::getSessionsDir("");
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
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
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
        if (static_cast<unsigned char>(c) < 0x20 || c == '<' || c == '>' || c == ':' || c == '"'
            || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
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
/// 会话元数据 (供会话列表展示): 原始 threadId / 会话名称 / 最近活动时间
/// - threadId: 目录名经 sanitizeThreadId 清洗后可能失真, 原始值单独存于 meta,
///   listSessions 恢复真实 threadId; 老数据无此键时回退目录名
/// - title:    首条用户消息的单行预览 (仅首次写入, 后续不覆盖)
/// - lastActiveMs: 最近一条消息的开始时间戳 (毫秒), 每次追加消息时更新
static constexpr std::string_view kMetaThreadId     = "threadId";
static constexpr std::string_view kMetaTitle        = "title";
static constexpr std::string_view kMetaLastActiveMs = "lastActiveMs";

/// 会话名称预览: 取首行并截断到 max 个 UTF-8 字符 (避免弹窗展示过宽)
static std::string titlePreview(std::string_view s, size_t max = 60) {
    const auto  nl = s.find('\n');
    std::string line{(nl == std::string_view::npos) ? s : s.substr(0, nl)};
    // 去除行尾回车 (Windows 换行符 \r\n)
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    // 按 UTF-8 字符数截断 (中文字符为多字节, 不能按字节切)
    size_t count = 0;
    size_t i     = 0;
    while (i < line.size() && count < max) {
        const auto c = static_cast<unsigned char>(line[i]);
        size_t     step = 1;
        if (c >= 0xf0) {
            step = 4;
        } else if (c >= 0xe0) {
            step = 3;
        } else if (c >= 0xc0) {
            step = 2;
        }
        if (i + step > line.size()) {
            break;
        }
        i += step;
        ++count;
    }
    if (i < line.size()) {
        line.resize(i);
        line += "...";
    }
    return line;
}

} // namespace

void SessionPersistence::updateViewMessage(
    std::string_view   threadId,
    const ViewMessage& msg
) {
    if (msg.id.empty()) {
        XX_LOGD("SessionPersistence: updateViewMessage({}) skipped (empty msg id)", threadId);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db = dbs(threadId).sessionDb;
            // 按消息 id 定位行 (json1 json_extract; sqlite >= 3.38 内置)
            auto update = db.prepare(
                "UPDATE view_message SET json = ? WHERE json_extract(json, '$.id') = ?"
            );
            update.bindText(1, msg.toJson().dump());
            update.bindText(2, msg.id);
            update.step();
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE(
                "SessionPersistence: updateViewMessage({}, id={}) failed: {}",
                threadId,
                msg.id,
                errmsg
            );
            return false;
        }
    );
}

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
    auto            dir = fs::path(rootDir_) / sanitizeThreadId(threadId);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        throw std::runtime_error{
            fmt::format("SessionPersistence: create dir {} failed: {}", dir.string(), ec.message())
        };
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
            // meta: msgIdCounter
            auto metaStmt = db.prepare("SELECT key, value FROM meta");
            while (metaStmt.step()) {
                if (metaStmt.columnText(0) == kMetaMsgIdCounter) {
                    out.msgIdCounter = static_cast<uint64_t>(metaStmt.columnInt64(1));
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

std::vector<SessionInfo> SessionPersistence::listSessions() {
    std::vector<SessionInfo> out;
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            std::error_code ec;
            fs::path        root{rootDir_};
            if (!fs::exists(root, ec)) {
                // 根目录不存在 = 从未持久化过任何会话, 返回空列表
                return true;
            }
            for (const auto& entry : fs::directory_iterator(root, ec)) {
                if (ec || !entry.is_directory(ec)) {
                    continue;
                }
                // 独立临时连接读取 meta (只读; 不复用 dbs_ 缓存, 也不创建目录):
                // threadId 优先取 meta 中的原始值 (目录名经 sanitize 后可能失真),
                // 老数据无 meta.threadId 时回退目录名 (generateUniqueThreadId 生成的
                // id 仅含安全字符, sanitize 不改写, 目录名即原始 threadId)
                SessionInfo info;
                info.threadId = entry.path().filename().string();
                agentxx::util::catchError<bool>(
                    [&]() -> bool {
                        agentxx::util::SqliteDb db;
                        db.open((entry.path() / "session.db").string());
                        auto stmt = db.prepare("SELECT key, value FROM meta");
                        while (stmt.step()) {
                            const auto key = stmt.columnText(0);
                            if (key == kMetaThreadId) {
                                const auto tid = stmt.columnText(1);
                                if (!tid.empty()) {
                                    info.threadId = tid;
                                }
                            } else if (key == kMetaTitle) {
                                info.title = stmt.columnText(1);
                            } else if (key == kMetaLastActiveMs) {
                                info.lastActiveMs = stmt.columnInt64(1);
                            }
                        }
                        // 兜底: 老数据无 lastActiveMs meta 时, 取最新一条
                        // view_message 的开始时间戳 (json1 json_extract); 仍为 0
                        // (历史消息均无时间戳) 时回退 session.db 文件修改时间,
                        // 保证会话列表时间列不为空 (展示端对 0 显示 "-")
                        if (info.lastActiveMs <= 0) {
                            auto lastStmt = db.prepare(
                                "SELECT json_extract(json, '$.start_time_ms') FROM view_message "
                                "ORDER BY seq DESC LIMIT 1"
                            );
                            if (lastStmt.step() && !lastStmt.columnIsNull(0)) {
                                info.lastActiveMs = lastStmt.columnInt64(0);
                            }
                        }
                        if (info.lastActiveMs <= 0) {
                            std::error_code fec;
                            const auto      mtime
                                = fs::last_write_time(entry.path() / "session.db", fec);
                            if (!fec) {
                                info.lastActiveMs
                                    = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          mtime.time_since_epoch()
                                      )
                                          .count();
                            }
                        }
                        return true;
                    },
                    [&](std::string errmsg) -> bool {
                        XX_LOGD(
                            "SessionPersistence: listSessions read {} failed: {}",
                            entry.path().filename().string(),
                            errmsg
                        );
                        return false;
                    }
                );
                out.push_back(std::move(info));
            }
            // 按最近活动时间降序 (最新在前); 时间相同时按 threadId 字典序保证稳定顺序
            std::sort(out.begin(), out.end(), [](const SessionInfo& a, const SessionInfo& b) {
                if (a.lastActiveMs != b.lastActiveMs) {
                    return a.lastActiveMs > b.lastActiveMs;
                }
                return a.threadId < b.threadId;
            });
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionPersistence: listSessions failed: {}", errmsg);
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
                auto meta = db.prepare("INSERT INTO meta(key, value) VALUES (?, ?) "
                                       "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
                meta.bindText(1, kMetaMsgIdCounter);
                meta.bindInt64(2, static_cast<int64_t>(msgIdCounter));
                meta.step();

                // ---- 会话列表元数据 (供 listSessions 展示, 与消息同事务提交) ----
                // 原始 threadId (目录名经清洗后可能失真)
                meta.reset();
                meta.bindText(1, kMetaThreadId);
                meta.bindText(2, std::string{threadId});
                meta.step();
                // 最近活动时间: 取消息开始时间戳 (毫秒)
                if (msg.startTimeMs > 0) {
                    meta.reset();
                    meta.bindText(1, kMetaLastActiveMs);
                    meta.bindInt64(2, msg.startTimeMs);
                    meta.step();
                }
                // 会话名称: 首条用户消息的单行预览 (仅首次写入, 不覆盖)
                if (msg.role == ViewMessage::Role::User && !msg.text.empty()) {
                    auto title = titlePreview(msg.text);
                    if (!title.empty()) {
                        auto titleStmt = db.prepare(
                            "INSERT INTO meta(key, value) VALUES (?, ?) "
                            "ON CONFLICT(key) DO NOTHING"
                        );
                        titleStmt.bindText(1, kMetaTitle);
                        titleStmt.bindText(2, title);
                        titleStmt.step();
                    }
                }

                db.commit();
                inTx = false;
            } catch (...) {
                if (inTx) {
                    agentxx::util::catchError<bool>(
                        [&]() -> bool {
                            db.rollback();
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
            XX_LOGE("SessionPersistence: appendViewMessage({}) failed: {}", threadId, errmsg);
            return false;
        }
    );
}

void SessionPersistence::saveLlmMessages(
    std::string_view      threadId,
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
            XX_LOGE("SessionPersistence: saveLlmMessages({}) failed: {}", threadId, errmsg);
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
            auto& db   = dbs(threadId).shareStoreDb;
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
            auto& db   = dbs(threadId).shareStoreDb;
            auto  stmt = db.prepare("SELECT value FROM item WHERE id = ?");
            stmt.bindInt64(1, static_cast<int64_t>(id));
            if (stmt.step()) {
                out = stmt.columnText(0);
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE(
                "SessionPersistence: getShareStoreItem({}, {}) failed: {}",
                threadId,
                id,
                errmsg
            );
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
            auto& db   = dbs(threadId).shareStoreDb;
            auto  stmt = db.prepare("INSERT INTO item(id, value) VALUES (?, ?) "
                                    "ON CONFLICT(id) DO UPDATE SET value = excluded.value");
            stmt.bindInt64(1, static_cast<int64_t>(id));
            stmt.bindText(2, value);
            stmt.step();
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE(
                "SessionPersistence: setShareStoreItem({}, {}) failed: {}",
                threadId,
                id,
                errmsg
            );
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
            auto stmt = db.prepare("INSERT INTO item(id, value) "
                                   "VALUES ((SELECT COALESCE(MAX(id), 0) + 1 FROM item), ?)");
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
            auto& db   = dbs(threadId).shareStoreDb;
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
