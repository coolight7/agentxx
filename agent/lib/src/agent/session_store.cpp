#include "agentxx/agent/session_store.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/util/container_util.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>

namespace agentxx {
namespace agent {

namespace fs = std::filesystem;

namespace {

/// 单个 sessionId 目录段最大长度 (截断后含分隔符与 hash 尾缀)
/// - Windows 默认 MAX_PATH=260, 需控制单段长度
static constexpr size_t kMaxSessionDataDirLen = 96;

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

/// 会话全量状态 SQL (session.db: view_message/llm_context/meta/store 单库)
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
CREATE TABLE IF NOT EXISTS store (
    id    INTEGER PRIMARY KEY,
    value TEXT NOT NULL
);
)sql";

/// meta 键名
static constexpr std::string_view kMetaMsgIdCounter = "msgIdCounter";
/// 会话元数据 (供会话列表展示): 原始 sessionId / 会话名称 / 最近活动时间
/// - sessionId: 目录名经 sanitizeSessionId 清洗后可能失真, 原始值单独存于 meta,
///   listSessions 恢复真实 sessionId; 老数据无此键时回退目录名
/// - title:    首条用户消息的单行预览 (仅首次写入, 后续不覆盖)
/// - lastActiveMs: 最近一条消息的开始时间戳 (毫秒), 每次追加消息时更新
static constexpr std::string_view kMetaSessionId    = "sessionId";
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
        const auto c    = static_cast<unsigned char>(line[i]);
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

void SessionStore::updateViewMessage(std::string_view sessionId, const ViewMessage& msg) {
    if (msg.id.empty()) {
        XX_LOGD("SessionStore: updateViewMessage({}) skipped (empty msg id)", sessionId);
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db = dbs(sessionId).sessionDb;
            // 按消息 id 定位行 (json1 json_extract; sqlite >= 3.38 内置)
            auto update
                = db.prepare("UPDATE view_message SET json = ? WHERE json_extract(json, '$.id') = ?"
                );
            update.bindText(1, msg.toJson().dump());
            update.bindText(2, msg.id);
            update.step();
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE(
                "SessionStore: updateViewMessage({}, id={}) failed: {}",
                sessionId,
                msg.id,
                errmsg
            );
            return false;
        }
    );
}

// ---------------------------------------------------------------------------
// SessionStore
// ---------------------------------------------------------------------------

SessionStore::SessionStore(std::string rootDir) :
    rootDir_(rootDir.empty() ? defaultRootDir() : std::move(rootDir)) {}

std::string SessionStore::sanitizeSessionId(std::string_view sessionId) {
    if (sessionId.empty()) {
        return "default";
    }
    auto seg = sanitizeSegment(sessionId);
    // 空串 / "." / ".." 不能作为目录名 (路径穿越/上级目录)
    if (seg.empty() || seg == "." || seg == "..") {
        seg = "session";
    }
    // 是否发生过改写 (需要附加哈希尾缀保证不同 sessionId 不碰撞到同一目录)
    bool changed = (seg != sessionId);
#if XX_IS_WIN_D
    if (isWindowsReservedName(seg)) {
        seg     = "t_" + seg;
        changed = true;
    }
#endif
    // 超长截断: 保留前部可读信息 + 8 位 hex hash 尾缀防碰撞
    if (seg.size() > kMaxSessionDataDirLen) {
        seg     = seg.substr(0, kMaxSessionDataDirLen - 9);
        changed = true;
    }
    if (changed) {
        seg += fmt::format("_{:08x}", static_cast<uint32_t>(fnv1a64(sessionId) & 0xffffffffu));
    }
    return seg;
}

SessionStore::SessionDbs& SessionStore::dbs(std::string_view sessionId) {
    // 目录: {root}/{sanitizedSessionId}/
    auto            dir = fs::path(rootDir_) / sanitizeSessionId(sessionId);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        throw std::runtime_error{
            fmt::format("SessionStore: create dir {} failed: {}", dir.string(), ec.message())
        };
    }

    auto it = dbs_.find(sessionId);
    if (it != dbs_.end()) {
        return *it->second;
    }
    auto dbs = std::make_shared<SessionDbs>();
    // 打开失败 (权限/磁盘) 抛异常, 由上层 catchError 记录日志
    dbs->sessionDb.open((dir / "session.db").string());
    ensureSchema(dbs->sessionDb);
    auto [insertIt, _] = util::insertHeterogeneous(dbs_, std::string{sessionId}, std::move(dbs));
    return *insertIt->second;
}

void SessionStore::ensureSchema(agentxx::util::SqliteDb& sessionDb) {
    sessionDb.exec(kSessionSchema);
}

bool SessionStore::sessionDataDirExists(std::string_view sessionId) const {
    std::error_code ec;
    return fs::exists(fs::path(rootDir_) / sanitizeSessionId(sessionId), ec);
}

SessionStore::LoadedSession SessionStore::loadSession(std::string_view sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    LoadedSession               out;
    // 目录不存在 = 从未写入过, 直接返回空 (避免只读访问创建目录/空文件)
    if (!sessionDataDirExists(sessionId)) {
        return out;
    }
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db = dbs(sessionId).sessionDb;

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
            XX_LOGE("SessionStore: loadSession({}) failed: {}", sessionId, errmsg);
            out = LoadedSession{};
            return false;
        }
    );
    return out;
}

/// 会话列表排序: 按最近活动时间降序 (最新在前); 时间相同时按 sessionId 字典序
/// 保证稳定顺序 (listSessions/listSessionsPage 共用)
static bool sessionNewerFirst(const SessionInfo& a, const SessionInfo& b) {
    if (a.lastActiveMs != b.lastActiveMs) {
        return a.lastActiveMs > b.lastActiveMs;
    }
    return a.sessionId < b.sessionId;
}

/// file_clock 时间戳 → unix 毫秒
/// - 以"两时钟当前时刻差"运行期锚定一次换算偏移, 避免依赖 clock_cast
///   (部分 libstdc++ 版本未实现); 偏移在进程生命周期内恒定 (NTP 微调可忽略)
/// - 供文件修改时间与 meta 中存储的 unix 毫秒时间戳比较/展示统一口径
static int64_t fileTimeToUnixMs(fs::file_time_type tp) {
    static const int64_t anchorDelta = [] {
        const auto fNow = fs::file_time_type::clock::now().time_since_epoch();
        const auto sNow = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(sNow).count()
               - std::chrono::duration_cast<std::chrono::milliseconds>(fNow).count();
    }();
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count()
           + anchorDelta;
}

/// 目录最近写入时刻启发式 (unix 毫秒): max(session.db, session.db-wal) 的修改时间。
/// SQLite 为 WAL 模式 (见 [sqlite.h](/agent/lib/include/agentxx/util/sqlite.h)),
/// 最近提交可能仍在 -wal 文件中未合并回主库, 仅 stat 主库会低估活动时间;
/// 取两者最大值近似最近写入时刻。
/// - 两个文件都不存在/不可读时返回 0 (排序时自然落在最后)
static int64_t sessionDirActivityHintMs(const fs::path& dir) {
    int64_t best = 0;
    for (const char* name : {"session.db", "session.db-wal"}) {
        std::error_code ec;
        const auto      t = fs::last_write_time(dir / name, ec);
        if (ec) {
            continue;
        }
        best = std::max(best, fileTimeToUnixMs(t));
    }
    return best;
}

/// 读取单个会话目录的 meta 摘要 (只读; 独立临时连接, 不复用 dbs_ 缓存, 也不创建目录):
/// sessionId 优先取 meta 中的原始值 (目录名经 sanitize 后可能失真),
/// 老数据无 meta.sessionId 时回退目录名 (generateUniqueSessionId 生成的
/// id 仅含安全字符, sanitize 不改写, 目录名即原始 sessionId)
/// - info.sessionId 须已预填目录名作回退值; 打开/读取失败返回 false (info 保持回退值)
static bool readSessionDirMeta(const fs::path& dir, SessionInfo& info) {
    return agentxx::util::catchError<bool>(
        [&]() -> bool {
            agentxx::util::SqliteDb db;
            db.open((dir / "session.db").string());
            auto stmt = db.prepare("SELECT key, value FROM meta");
            while (stmt.step()) {
                const auto key = stmt.columnText(0);
                if (key == kMetaSessionId) {
                    const auto tid = stmt.columnText(1);
                    if (!tid.empty()) {
                        info.sessionId = tid;
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
                auto lastStmt
                    = db.prepare("SELECT json_extract(json, '$.start_time_ms') FROM view_message "
                                 "ORDER BY seq DESC LIMIT 1");
                if (lastStmt.step() && !lastStmt.columnIsNull(0)) {
                    info.lastActiveMs = lastStmt.columnInt64(0);
                }
            }
            if (info.lastActiveMs <= 0) {
                info.lastActiveMs = sessionDirActivityHintMs(dir);
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGD(
                "SessionStore: read session meta {} failed: {}",
                dir.filename().string(),
                errmsg
            );
            return false;
        }
    );
}

std::vector<SessionInfo> SessionStore::listSessions() {
    std::vector<SessionInfo>    out;
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
                SessionInfo info;
                info.sessionId = entry.path().filename().string();
                readSessionDirMeta(entry.path(), info);
                out.push_back(std::move(info));
            }
            std::sort(out.begin(), out.end(), sessionNewerFirst);
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionStore: listSessions failed: {}", errmsg);
            return false;
        }
    );
    return out;
}

SessionStore::SessionListPage
    SessionStore::listSessionsPage(int64_t beforeMs, std::string_view beforeId, uint32_t limit) {
    // limit == 0 全量路径: 复用 listSessions (其自行加锁, 须在取锁前调用避免重入)
    if (limit == 0) {
        SessionListPage page;
        page.sessions   = listSessions();
        page.totalCount = page.sessions.size();
        page.hasMore    = false;
        return page;
    }

    SessionListPage             page;
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            std::error_code ec;
            fs::path        root{rootDir_};
            if (!fs::exists(root, ec)) {
                // 根目录不存在 = 从未持久化过任何会话
                return true;
            }

            // ---- 阶段 1: 仅 stat 各目录的有效修改时间 (不打开数据库) ----
            // 得到近似活动顺序与总会话数; mtime 与 lastActiveMs 强相关但不完全
            // 一致 (tool 结果回填等只更新文件不改 meta), 故仅作读取顺序启发,
            // 绝不据此跳过目录 (保证结果精确)
            struct DirHint {
                fs::path dir;
                int64_t  hintMs = 0;
            };
            std::vector<DirHint> dirs;
            for (const auto& entry : fs::directory_iterator(root, ec)) {
                if (ec || !entry.is_directory(ec)) {
                    continue;
                }
                dirs.push_back({entry.path(), sessionDirActivityHintMs(entry.path())});
            }
            page.totalCount = dirs.size();
            std::sort(dirs.begin(), dirs.end(), [](const DirHint& a, const DirHint& b) {
                if (a.hintMs != b.hintMs) {
                    return a.hintMs > b.hintMs;
                }
                return a.dir.filename().string() < b.dir.filename().string();
            });

            // 游标过滤: 排序位置严格位于游标之后 (与 sessionNewerFirst 的降序全序
            // 一致: lastActiveMs 更小, 或同毫秒时 sessionId 更大)
            auto qualifies = [beforeMs, &beforeId](int64_t la, std::string_view sid) {
                if (beforeMs <= 0) {
                    return true;
                }
                if (la != beforeMs) {
                    return la < beforeMs;
                }
                return sid > beforeId;
            };

            // ---- 阶段 2: 按 hint 顺序逐个读取精确 meta 并收集 ----
            bool stoppedEarly = false;
            // 是否有符合游标的条目因页满被挤出本页 (排名低于边界, 属于后续页)
            bool overflowed = false;
            for (const auto& d : dirs) {
                // 安全早停: 本页已收满且当前目录的有效 mtime 严格早于页边界。
                // 正确性: lastActiveMs 为消息开始时间戳, 写入提交时刻恒 ≥ 它, 即
                // 有效 mtime ≥ lastActiveMs; 故 mtime 更早的目录其会话必然排在
                // 边界之后, 不可能进入本页。相等时不早停: 同毫秒会话按 id 升序
                // 排序, id 更小者仍可能排进本页
                if (page.sessions.size() >= static_cast<size_t>(limit)
                    && d.hintMs < page.sessions.back().lastActiveMs) {
                    stoppedEarly = true;
                    break;
                }
                SessionInfo info;
                info.sessionId = d.dir.filename().string();
                readSessionDirMeta(d.dir, info);
                if (!qualifies(info.lastActiveMs, info.sessionId)) {
                    continue;
                }
                page.sessions.push_back(std::move(info));
                // 达到/超出页大小时排序维持边界不变量 (早停判断依赖 back() 为
                // 当前页最末名); 超出部分排名低于边界不会进本页, 但确实存在,
                // 置 overflowed 保证 hasMore 语义正确。少量 mtime 乱序由排序纠正
                const auto want = static_cast<size_t>(limit);
                if (page.sessions.size() >= want) {
                    std::sort(page.sessions.begin(), page.sessions.end(), sessionNewerFirst);
                    if (page.sessions.size() > want) {
                        overflowed = true;
                        page.sessions.resize(want);
                    }
                }
            }
            std::sort(page.sessions.begin(), page.sessions.end(), sessionNewerFirst);
            // hasMore: 早停 = 边界之后还有未检查的目录; 溢出 = 检查过但有条目
            // 被挤出本页 (两者都意味着后续页非空)
            page.hasMore = stoppedEarly || overflowed;
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionStore: listSessionsPage failed: {}", errmsg);
            return false;
        }
    );
    return page;
}

void SessionStore::appendViewMessage(
    std::string_view   sessionId,
    const ViewMessage& msg,
    uint64_t           msgIdCounter
) {
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db = dbs(sessionId).sessionDb;
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
                // 原始 sessionId (目录名经清洗后可能失真)
                meta.reset();
                meta.bindText(1, kMetaSessionId);
                meta.bindText(2, std::string{sessionId});
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
                        auto titleStmt = db.prepare("INSERT INTO meta(key, value) VALUES (?, ?) "
                                                    "ON CONFLICT(key) DO NOTHING");
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
            XX_LOGE("SessionStore: appendViewMessage({}) failed: {}", sessionId, errmsg);
            return false;
        }
    );
}

void SessionStore::saveLlmMessages(std::string_view sessionId, const neograph::json& llmMessages) {
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db = dbs(sessionId).sessionDb;
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
            XX_LOGE("SessionStore: saveLlmMessages({}) failed: {}", sessionId, errmsg);
            return false;
        }
    );
}

// ---------------------------------------------------------------------------
// share store (session.db store 表)
// ---------------------------------------------------------------------------

SessionStore::LoadedShareStore SessionStore::loadShareStore(std::string_view sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    LoadedShareStore            out;
    // 目录不存在 = 从未写入过, 直接返回空
    if (!sessionDataDirExists(sessionId)) {
        return out;
    }
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db   = dbs(sessionId).sessionDb;
            auto  stmt = db.prepare("SELECT id, value FROM store ORDER BY id");
            while (stmt.step()) {
                out.items[static_cast<size_t>(stmt.columnInt64(0))] = stmt.columnText(1);
            }
            out.nextId = out.items.empty() ? 1 : (out.items.rbegin()->first + 1);
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionStore: loadShareStore({}) failed: {}", sessionId, errmsg);
            out = LoadedShareStore{};
            return false;
        }
    );
    return out;
}

std::optional<std::string> SessionStore::getShareStoreItem(std::string_view sessionId, size_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<std::string>  out;
    // 目录不存在 = 从未写入过, 直接返回 nullopt
    if (!sessionDataDirExists(sessionId)) {
        return out;
    }
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db   = dbs(sessionId).sessionDb;
            auto  stmt = db.prepare("SELECT value FROM store WHERE id = ?");
            stmt.bindInt64(1, static_cast<int64_t>(id));
            if (stmt.step()) {
                out = stmt.columnText(0);
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionStore: getShareStoreItem({}, {}) failed: {}", sessionId, id, errmsg);
            return false;
        }
    );
    return out;
}

void SessionStore::setShareStoreItem(
    std::string_view sessionId,
    size_t           id,
    std::string_view value
) {
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db   = dbs(sessionId).sessionDb;
            auto  stmt = db.prepare("INSERT INTO store(id, value) VALUES (?, ?) "
                                    "ON CONFLICT(id) DO UPDATE SET value = excluded.value");
            stmt.bindInt64(1, static_cast<int64_t>(id));
            stmt.bindText(2, value);
            stmt.step();
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionStore: setShareStoreItem({}, {}) failed: {}", sessionId, id, errmsg);
            return false;
        }
    );
}

size_t SessionStore::addShareStoreItem(std::string_view sessionId, std::string_view value) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t                      out = 0;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db = dbs(sessionId).sessionDb;
            // 自增 id: 取现有最大 id + 1, 重启后延续; 与内存版
            // (SessionShareStore::storeId 递增) 语义一致且更稳健
            auto stmt = db.prepare("INSERT INTO store(id, value) "
                                   "VALUES ((SELECT COALESCE(MAX(id), 0) + 1 FROM store), ?)");
            stmt.bindText(1, value);
            stmt.step();
            out = static_cast<size_t>(db.lastInsertRowid());
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionStore: addShareStoreItem({}) failed: {}", sessionId, errmsg);
            return false;
        }
    );
    return out;
}

void SessionStore::removeShareStoreItem(std::string_view sessionId, size_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db   = dbs(sessionId).sessionDb;
            auto  stmt = db.prepare("DELETE FROM store WHERE id = ?");
            stmt.bindInt64(1, static_cast<int64_t>(id));
            stmt.step();
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionStore: removeShareStoreItem({}, {}) failed: {}", sessionId, id, errmsg);
            return false;
        }
    );
}

} // namespace agent
} // namespace agentxx
