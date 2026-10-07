#include "agentxx/agent/session_store.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/util/exception.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/hash.h"
#include "utilxx_base/log.h"
#include "utilxx_base/path_sanitize.h"
#include "utilxx_base/string_util.h"
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
/// - 统一使用 utilxx_base::hash::fnv1a64 算法
using utilxx_base::hash::fnv1a64;

/// 默认数据根目录: {dataDir}/sqlite/sessions/
/// - dataDir 为空时回退 ~/.agentxx/ (取不到用户主目录时回退系统临时目录)
static std::string defaultRootDir() {
    return agentxx::agent::AgentConfigStatic::getSessionsDir("");
}

/// 会话全量状态 SQL (session.db: view_message/llm_context/meta/store/usage 单库)
/// 表结构 (幂等)
/// - view_message: seq 自增主键 + json; msg_id 为消息 id 的独立列 (带索引),
///   供 updateViewMessage 走索引定位 (如果用 json_extract(json,'$.id') 全表
///   扫描 + 逐行 JSON 解析, 长会话 (数千条) 下每次 tool 结果回填都要重扫一遍)
/// - 老库 (无 msg_id 列) 由 [ensureViewMessageMsgIdColumn] 迁移补齐
/// - usage: 每次模型调用的用量记录 (成功/失败各一行), 供界面与诊断聚合
static constexpr const char* kSessionSchema = R"sql(
CREATE TABLE IF NOT EXISTS view_message (
    seq    INTEGER PRIMARY KEY AUTOINCREMENT,
    json   TEXT NOT NULL,
    msg_id TEXT
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
CREATE TABLE IF NOT EXISTS usage (
    id                   INTEGER PRIMARY KEY AUTOINCREMENT,
    time_ms              INTEGER NOT NULL,
    model                TEXT NOT NULL,
    prompt_tokens        INTEGER NOT NULL DEFAULT 0,
    completion_tokens    INTEGER NOT NULL DEFAULT 0,
    total_tokens         INTEGER NOT NULL DEFAULT 0,
    cached_prompt_tokens INTEGER NOT NULL DEFAULT 0,
    cache_write_prompt_tokens INTEGER NOT NULL DEFAULT 0,
    reasoning_tokens     INTEGER NOT NULL DEFAULT 0,
    ok                   INTEGER NOT NULL DEFAULT 1,
    error_kind           TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_usage_time ON usage(time_ms);
CREATE TABLE IF NOT EXISTS session_input (
    id           TEXT PRIMARY KEY,
    payload      TEXT NOT NULL,
    delivery     TEXT NOT NULL DEFAULT '',
    status       TEXT NOT NULL DEFAULT 'admitted',
    admitted_seq INTEGER NOT NULL DEFAULT 0,
    promoted_seq INTEGER NOT NULL DEFAULT 0,
    created_ms   INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_session_input_status ON session_input(status);
)sql";

/// meta 键名
static constexpr std::string_view kMetaMsgIdCounter = "msgIdCounter";
/// schema 结构版本 (见 [SessionStore::kSchemaVersion]; 迁移步骤见 applyMigrationStep)
static constexpr std::string_view kMetaSchemaVersion = "schema_version";
/// 会话元数据 (供会话列表展示): 原始 sessionId / 会话名称 / 最近活动时间
/// - sessionId: 目录名经 sanitizeSessionId 清洗后可能失真, 原始值单独存于 meta,
///   listSessions 恢复真实 sessionId; 老数据无此键时回退目录名
/// - title:    首条用户消息的单行预览 (仅首次写入, 后续不覆盖)
/// - lastActiveMs: 最近一条消息的开始时间戳 (毫秒), 每次追加消息时更新
static constexpr std::string_view kMetaSessionId    = "sessionId";
static constexpr std::string_view kMetaTitle        = "title";
static constexpr std::string_view kMetaLastActiveMs = "lastActiveMs";
/// 标题来源: "auto" (首条用户消息预览) / "user" (用户改名); 老数据无该键
static constexpr std::string_view kMetaTitleSource  = "titleSource";
/// 展示历史持久化序号计数 (计划 STO-4; 见 SessionStore::LoadedSession::lastViewSeq)
static constexpr std::string_view kMetaViewSeqCounter = "viewSeqCounter";

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

/// 持久化轻量化：剥离附件的 dataUrl (Base64 数据)，仅保留文件路径与元数据
ViewMessage stripAttachmentDataUrl(const ViewMessage& msg) {
    if (msg.attachments.empty()) {
        return msg;
    }
    ViewMessage copy = msg;
    for (auto& att : copy.attachments) {
        att.dataUrl.clear();
    }
    return copy;
}

/// 序列化 JSON 文本并保证其 UTF-8 合法 (入库前统一入口):
/// - Json::dump 对 >= 0x80 的字节原样透传 (仅转义控制字符/引号/反斜杠), 若字符串
///   内容含非法 UTF-8 (如工具输出的 GBK/二进制文本), 落库文本即为非法 UTF-8 JSON;
///   读取端 (simdjson) 要求 UTF-8, 解析该行会抛异常 —— 表现为会话历史/上下文
///   部分乃至整体无法恢复 (数据丢失)
/// - 此处对非法序列按 U+FFFD 修复 (仅替换本就非法的字节, 合法文本原样保留),
///   保证落库内容始终可被解析
std::string dumpJsonUtf8(const utilxx_base::Json& j) {
    std::string text = j.dump();
    if (!utilxx_base::utf8IsAvail(text)) {
        // 返回值表示"是否真的发生过替换" (此处仅关心修复后的文本内容)
        (void)utilxx_base::utf8Repair(text);
    }
    return text;
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
            auto& db      = dbs(sessionId).sessionDb;
            auto  payload = dumpJsonUtf8(stripAttachmentDataUrl(msg).toJson());

            // 主路径: 按 msg_id 索引列定位 (idx_view_message_msg_id)
            int64_t affected = 0;
            {
                auto update = db.prepare("UPDATE view_message SET json = ? WHERE msg_id = ?");
                update.bindText(1, payload);
                update.bindText(2, msg.id);
                update.step();
                auto changed = db.prepare("SELECT changes()");
                if (changed.step()) {
                    affected = changed.columnInt64(0);
                }
            }
            if (affected == 0) {
                // 兜底: 历史行 msg_id 为空 (老库迁移前写入且 json 中无 id) ——
                // 用 json_extract 定位并顺带回填 msg_id (下次即走索引)
                auto update = db.prepare("UPDATE view_message SET json = ?, msg_id = ? "
                                         "WHERE msg_id IS NULL AND json_extract(json, '$.id') = ?");
                update.bindText(1, payload);
                update.bindText(2, msg.id);
                update.bindText(3, msg.id);
                update.step();
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE(
                "SessionStore: updateViewMessage({}, id={}) failed: {}",
                sessionId,
                msg.id,
                errmsg
            );
            // 记录写失败原因 (计划 STO-9; 只记根因, 见 appendViewMessage 处注释)
            lastWriteError_ = errmsg;
            return false;
        }
    );
}

// ---------------------------------------------------------------------------
// SessionStore
// ---------------------------------------------------------------------------

SessionStore::SessionStore(std::string rootDir, bool enableWriterLease) :
    rootDir_(rootDir.empty() ? defaultRootDir() : std::move(rootDir)),
    enableWriterLease_(enableWriterLease) {}

std::string SessionStore::sanitizeSessionId(std::string_view sessionId) {
    if (sessionId.empty()) {
        return "default";
    }
    auto seg = utilxx_base::sanitizeFsSegment(sessionId);
    // 空串 / "." / ".." 不能作为目录名 (路径穿越/上级目录)
    if (seg.empty() || seg == "." || seg == "..") {
        seg = "session";
    }
    // 是否发生过改写 (需要附加哈希尾缀保证不同 sessionId 不碰撞到同一目录)
    bool changed = (seg != sessionId);
#if XX_IS_WIN_D
    if (utilxx_base::isWindowsReservedName(seg)) {
        seg     = "t_" + seg;
        changed = true;
    }
#endif
    // 超长截断: 保留前部可读信息, 最终由下方统一附加 8 位 hex hash 尾缀防碰撞
    // (尾缀占 9 字符 "_" + 8 hex, 故此处先让出; 哈希取自原始 sessionId,
    //  与截断/清洗结果无关, 保证同一会话稳定映射到同一目录)
    if (seg.size() > kMaxSessionDataDirLen) {
        seg     = utilxx_base::truncateFsSegment(seg, kMaxSessionDataDirLen - 9);
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
        auto reason = fmt::format("create dir {} failed: {}", dir.string(), ec.message());
        lastWriteError_ = reason;
        throw std::runtime_error{"SessionStore: " + reason};
    }

    auto it = dbs_.find(sessionId);
    if (it != dbs_.end()) {
        it->second.lastUseSeq = ++dbsUseSeq_; // 刷新 LRU 位置
        return *it->second.dbs;
    }
    auto entry = DbsEntry{};
    entry.dbs  = std::make_shared<SessionDbs>();
    // 跨进程写租约: 同一会话目录同时只允许一个进程写入 (读操作不经本函数)
    // - 租约被其它进程持有时抛异常 → 写操作明确失败并给出原因, 不覆盖对方数据
    // - 同一进程内可重入 (引用计数), 便于多实例/测试共用同一目录
    if (enableWriterLease_) {
        std::string leaseErr;
        entry.dbs->writerLease = SessionWriterLease::acquire(dir, &leaseErr);
        if (!entry.dbs->writerLease) {
            auto reason = fmt::format("session dir is busy: {}", leaseErr);
            lastWriteError_ = reason;
            XX_LOGE("SessionStore: {} ({})", reason, sessionId);
            throw std::runtime_error{"SessionStore: " + reason};
        }
    }
    // 打开失败 (权限/磁盘) 抛异常, 由上层 catchError 记录日志
    const auto dbFile = (dir / "session.db").string();
    entry.dbs->sessionDb.open(dbFile);
    try {
        ensureSchema(entry.dbs->sessionDb, dbFile);
    } catch (...) {
        // 迁移失败/库版本过高: 释放已获取的连接与租约, 记录原因
        lastWriteError_ = fmt::format("open session db {} failed (see log)", dbFile);
        entry.dbs->sessionDb.close();
        entry.dbs->writerLease.reset();
        throw;
    }
    lastWriteError_.clear();
    entry.lastUseSeq = ++dbsUseSeq_;
    auto [insertIt, _]
        = utilxx_base::insertHeterogeneous(dbs_, std::string{sessionId}, std::move(entry));
    // 连接数上限 (LRU 淘汰; 刚插入的条目为最新, 不会被淘汰)
    evictLruDbs();
    return *insertIt->second.dbs;
}

std::string SessionStore::lastWriteError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastWriteError_;
}

void SessionStore::evictLruDbs() {
    while (dbs_.size() > kMaxOpenSessionDbs) {
        auto lru = dbs_.begin();
        for (auto it = dbs_.begin(); it != dbs_.end(); ++it) {
            if (it->second.lastUseSeq < lru->second.lastUseSeq) {
                lru = it;
            }
        }
        XX_LOGD(
            "SessionStore: closing least-recently-used session db '{}' (open={} > max={})",
            lru->first,
            dbs_.size(),
            kMaxOpenSessionDbs
        );
        // 显式 close (WAL 自动 checkpoint), 再从缓存移除
        // (移除条目同时释放写租约: 租约随对象析构回收)
        lru->second.dbs->sessionDb.close();
        lru->second.dbs->writerLease.reset();
        dbs_.erase(lru);
    }
}

int SessionStore::readSchemaVersion(agentxx::util::SqliteDb& sessionDb) {
    // meta 表可能尚未建立 (全新库/极老库): 读失败按版本 0 处理
    return agentxx::util::catchError<int>(
        [&]() -> int {
            auto stmt = sessionDb.prepare("SELECT value FROM meta WHERE key = ?");
            stmt.bindText(1, kMetaSchemaVersion);
            if (!stmt.step()) {
                return 0;
            }
            return static_cast<int>(stmt.columnInt64(0));
        },
        [&](std::string) -> int {
            return 0;
        }
    );
}

void SessionStore::writeSchemaVersion(agentxx::util::SqliteDb& sessionDb, int version) {
    auto stmt = sessionDb.prepare("INSERT INTO meta(key, value) VALUES(?, ?) "
                                  "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    stmt.bindText(1, kMetaSchemaVersion);
    stmt.bindInt64(2, version);
    stmt.step();
}

void SessionStore::backupDbFile(
    agentxx::util::SqliteDb& sessionDb,
    const std::string&       dbFilePath,
    int                      fromVersion
) {
    if (dbFilePath.empty()) {
        return;
    }
    // 全新库 (没有任何表) 无需备份, 避免产生无意义的文件
    bool hasTable = false;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto stmt = sessionDb.prepare("SELECT count(*) FROM sqlite_master WHERE type = 'table'");
            if (stmt.step()) {
                hasTable = stmt.columnInt64(0) > 0;
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGW("SessionStore: inspect schema before migration failed: {}", errmsg);
            return false;
        }
    );
    if (!hasTable) {
        return;
    }
    // 备份前把 WAL 内容合并回主库, 否则复制出的文件缺最近提交
    (void)agentxx::util::catchError<bool>(
        [&]() -> bool {
            sessionDb.exec("PRAGMA wal_checkpoint(FULL)");
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGW("SessionStore: wal_checkpoint before backup failed: {}", errmsg);
            return false;
        }
    );
    const auto src = fs::path{dbFilePath};
    const auto dst = fs::path{dbFilePath + fmt::format(".bak.v{}", fromVersion)};
    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        // 备份失败不阻断迁移: 记录后继续 (迁移本身每步独立事务, 幂等)
        XX_LOGW(
            "SessionStore: backup {} -> {} failed: {}",
            src.string(),
            dst.string(),
            ec.message()
        );
        return;
    }
    XX_LOGI(
        "SessionStore: schema migration backup written: {} (from version {})",
        dst.string(),
        fromVersion
    );
}

void SessionStore::ensureSchema(agentxx::util::SqliteDb& sessionDb, const std::string& dbFilePath) {
    const int current = readSchemaVersion(sessionDb);
    if (current > kSchemaVersion) {
        // 高版本库 (由更新的程序写入) 拒绝打开: 旧程序不认识新结构, 继续写会毁数据
        throw std::runtime_error{fmt::format(
            "SessionStore: session db schema version {} is newer than supported {} ({}); "
            "open it with a newer agentxx build",
            current,
            kSchemaVersion,
            dbFilePath
        )};
    }
    if (current == kSchemaVersion) {
        return;
    }
    // 迁移前备份 (老库才有内容; 全新库直接建表)
    backupDbFile(sessionDb, dbFilePath, current);
    for (int step = current + 1; step <= kSchemaVersion; ++step) {
        // 每一步独立事务: 中途失败已提交的步骤保持有效, 下次打开从该版本续做
        sessionDb.beginImmediate();
        bool inTx = true;
        try {
            applyMigrationStep(sessionDb, step);
            writeSchemaVersion(sessionDb, step);
            sessionDb.commit();
            inTx = false;
        } catch (...) {
            if (inTx) {
                agentxx::util::catchError<bool>(
                    [&]() -> bool {
                        sessionDb.rollback();
                        return true;
                    },
                    [](std::string) -> bool {
                        return false;
                    }
                );
            }
            throw;
        }
        XX_LOGI("SessionStore: schema migrated to version {} ({})", step, dbFilePath);
    }
}

void SessionStore::applyMigrationStep(agentxx::util::SqliteDb& sessionDb, int step) {
    switch (step) {
        case 1:
            // v1: 基线结构 (四张老表 + msg_id 列/索引 + 用量记录表)
            // - 老库 (无 schema_version 记录) 走这一步补齐缺失的表与列
            // - 建表全部 IF NOT EXISTS, 重复执行安全
            sessionDb.exec(kSessionSchema);
            ensureViewMessageMsgIdColumn(sessionDb);
            break;
        case 2:
            // v2: 输入收件箱 (session_input 表; 计划 LOOP-1)
            // - 老库补表; 新库建表时已含 (IF NOT EXISTS 幂等)
            // - 无需数据回填: 老库中的输入要么已经执行完 (不在库内), 要么随进程
            //   退出丢失 (老版本不持久化输入)
            sessionDb.exec(
                "CREATE TABLE IF NOT EXISTS session_input ("
                "    id           TEXT PRIMARY KEY,"
                "    payload      TEXT NOT NULL,"
                "    delivery     TEXT NOT NULL DEFAULT '',"
                "    status       TEXT NOT NULL DEFAULT 'admitted',"
                "    admitted_seq INTEGER NOT NULL DEFAULT 0,"
                "    promoted_seq INTEGER NOT NULL DEFAULT 0,"
                "    created_ms   INTEGER NOT NULL DEFAULT 0"
                ")"
            );
            sessionDb.exec(
                "CREATE INDEX IF NOT EXISTS idx_session_input_status ON session_input(status)"
            );
            break;
        case 3:
            // v3: 用量记录补"缓存写入量"列 (cache_write_prompt_tokens; 计划 LLM-8)
            // - Anthropic 的 cache_creation_input_tokens (写入 prompt 缓存的量) 与
            //   cached_prompt_tokens (命中缓存的读取量) 分开记账, 便于评估
            //   `cache_control` 断点的收益
            // - 老库补列 (默认 0); 新库建表时已含该列 (幂等)
            ensureUsageCacheWriteColumn(sessionDb);
            break;
        default:
            throw std::runtime_error{
                fmt::format("SessionStore: unknown schema migration step {}", step)
            };
    }
}

/// 迁移: 保证 view_message 有 msg_id 列与索引 (幂等)
/// - 新库: CREATE TABLE 已含该列, 此处只补索引
/// - 老库 (无该列): ALTER 增加列 → 从 json 回填 → 建索引; 已有数据不受影响
///   (回填只补 msg_id, 不触碰 json 内容)
void SessionStore::ensureViewMessageMsgIdColumn(agentxx::util::SqliteDb& sessionDb) {
    bool hasMsgId = false;
    {
        auto stmt = sessionDb.prepare("PRAGMA table_info(view_message)");
        while (stmt.step()) {
            // 列信息: cid, name, type, notnull, dflt_value, pk
            if (stmt.columnText(1) == "msg_id") {
                hasMsgId = true;
                break;
            }
        }
    }
    if (false == hasMsgId) {
        sessionDb.exec("ALTER TABLE view_message ADD COLUMN msg_id TEXT");
        XX_LOGI("SessionStore: view_message migrated (added msg_id column), backfilling ...");
        sessionDb.exec(
            "UPDATE view_message SET msg_id = json_extract(json, '$.id') WHERE msg_id IS NULL"
        );
    }
    sessionDb.exec("CREATE INDEX IF NOT EXISTS idx_view_message_msg_id ON view_message(msg_id)");
}

/// 迁移: 保证 usage 有 cache_write_prompt_tokens 列 (幂等; 计划 LLM-8)
/// - 新库: CREATE TABLE 已含该列, 此处什么都不用做
/// - 老库 (无该列): ALTER 增加列并默认 0 (历史记录没有缓存写入量, 不猜测回填)
void SessionStore::ensureUsageCacheWriteColumn(agentxx::util::SqliteDb& sessionDb) {
    bool hasColumn = false;
    {
        auto stmt = sessionDb.prepare("PRAGMA table_info(usage)");
        while (stmt.step()) {
            // 列信息: cid, name, type, notnull, dflt_value, pk
            if (stmt.columnText(1) == "cache_write_prompt_tokens") {
                hasColumn = true;
                break;
            }
        }
    }
    if (false == hasColumn) {
        sessionDb.exec(
            "ALTER TABLE usage ADD COLUMN cache_write_prompt_tokens INTEGER NOT NULL DEFAULT 0"
        );
        XX_LOGI("SessionStore: usage table migrated (added cache_write_prompt_tokens column)");
    }
}

bool SessionStore::sessionDataDirExists(std::string_view sessionId) const {
    std::error_code ec;
    return fs::exists(fs::path(rootDir_) / sanitizeSessionId(sessionId), ec);
}

SessionStore::LoadedSession SessionStore::loadSession(std::string_view sessionId) {
    LoadedSession out;
    // 只读路径: 用临时连接直接读文件, 不获取写租约
    // - 另一进程正在写该会话 (持有写租约) 时, 本进程仍可读取历史, 不会因
    //   读操作而互相阻塞; 写操作才需要租约 (见 [dbs])
    // - 文件不存在 = 从未写入过, 直接返回空 (避免只读访问创建目录/空文件)
    const auto      dbFile = fs::path(rootDir_) / sanitizeSessionId(sessionId) / "session.db";
    std::error_code ec;
    if (!fs::exists(dbFile, ec)) {
        return out;
    }
    agentxx::util::SqliteDb db;
    const auto              opened = agentxx::util::catchError<bool>(
        [&]() -> bool {
            db.open(dbFile.string());
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE(
                "SessionStore: loadSession({}) open {} failed: {}",
                sessionId,
                dbFile.string(),
                errmsg
            );
            return false;
        }
    );
    if (!opened) {
        return out;
    }
    // 库版本高于本程序支持的版本时拒绝读取: 新结构可能已改变表/列语义, 按老结构
    // 解析只会得到"看起来空"的错误历史; 这里明确失败并提示用更新的版本打开
    // (写路径同样拒绝, 见 [ensureSchema])
    if (const int version = readSchemaVersion(db); version > kSchemaVersion) {
        XX_LOGE(
            "SessionStore: loadSession({}) refused: session db schema version {} is newer than "
            "supported {} ({})",
            sessionId,
            version,
            kSchemaVersion,
            dbFile.string()
        );
        return out;
    }
    // 分区恢复 (崩溃/断电后尽量少丢数据):
    // - 单行 JSON 解析失败 (历史遗留脏数据/编码异常) 只跳过该行, 不再整体丢弃
    //   —— 原实现任一异常都会把 LoadedSession 重置为空, 表现为"会话数据全部
    //   丢失", 且后续 saveLlmMessages 会用新上下文覆盖库内旧数据 (不可恢复)
    // - 展示历史 / LLM 上下文 / meta 三段各自独立捕获, 一段失败不影响其余
    //   (三段共用上面打开的只读连接 [db])
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            // 展示历史 (按追加顺序)
            auto stmt = db.prepare("SELECT seq, json FROM view_message ORDER BY seq");
            while (stmt.step()) {
                const auto jsonText = stmt.columnText(1);
                agentxx::util::catchError<bool>(
                    [&]() -> bool {
                        auto j = utilxx_base::Json::parse(jsonText);
                        out.viewMessages.push_back(ViewMessage::fromJson(j));
                        return true;
                    },
                    [&](std::string errmsg) -> bool {
                        XX_LOGW(
                            "SessionStore: loadSession({}) 跳过无法解析的历史消息 (seq={}): {}",
                            sessionId,
                            stmt.columnInt64(0),
                            errmsg
                        );
                        return false;
                    }
                );
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE(
                "SessionStore: loadSession({}) 读取展示历史失败 (保留已恢复部分): {}",
                sessionId,
                errmsg
            );
            return false;
        }
    );
    // meta: msgIdCounter (失败时按历史条数兜底, 不丢弃历史)
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto stmt = db.prepare("SELECT key, value FROM meta");
            while (stmt.step()) {
                if (stmt.columnText(0) == kMetaMsgIdCounter) {
                    out.msgIdCounter = static_cast<uint64_t>(stmt.columnInt64(1));
                } else if (stmt.columnText(0) == kMetaViewSeqCounter) {
                    out.lastViewSeq = static_cast<uint64_t>(stmt.columnInt64(1));
                }
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionStore: loadSession({}) 读取 meta 失败: {}", sessionId, errmsg);
            return false;
        }
    );
    // 兜底: 老数据无 msgIdCounter 记录时按历史条数恢复
    // (历史 append-only, id 连续分配, 条数即最后序号)
    if (out.msgIdCounter == 0) {
        out.msgIdCounter = out.viewMessages.size();
    }
    // 兜底: 老数据无 viewSeqCounter 记录时取库内最大行序号 (Session::restore 亦会
    // 按历史条数兜底, 二者一致: 老库行序号即追加顺序 1..N)
    if (out.lastViewSeq == 0) {
        agentxx::util::catchError<bool>(
            [&]() -> bool {
                auto stmt = db.prepare("SELECT COALESCE(MAX(seq), 0) FROM view_message");
                if (stmt.step()) {
                    out.lastViewSeq = static_cast<uint64_t>(stmt.columnInt64(0));
                }
                return true;
            },
            [&](std::string) -> bool {
                return false;
            }
        );
    }
    // LLM 上下文 (单行; 解析失败时保留空上下文, 展示历史不受影响)
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto stmt = db.prepare("SELECT json FROM llm_context WHERE id = 1");
            if (stmt.step()) {
                out.llmMessages = utilxx_base::Json::parse(stmt.columnText(0));
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE(
                "SessionStore: loadSession({}) 读取 LLM 上下文失败 (按空上下文恢复): {}",
                sessionId,
                errmsg
            );
            out.llmMessages = utilxx_base::Json::array();
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
/// - 供文件修改时间与 meta 中存储的 unix 毫秒时间戳比较/展示使用同一套规则
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
                auto lastStmt = db.prepare(
                    "SELECT COALESCE(json_extract(json, '$.startTimeMs'), json_extract(json, '$.start_time_ms')) FROM view_message "
                    "ORDER BY seq DESC LIMIT 1"
                );
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
    uint64_t           msgIdCounter,
    uint64_t           seq
) {
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db = dbs(sessionId).sessionDb;
            db.beginImmediate();
            bool inTx = true;
            try {
                // 显式序号 (计划 STO-4): seq > 0 时按调用方给的展示历史序号写入
                // (会话内单调递增, 与内存中的 viewMessages 一一对应); seq == 0
                // (老调用方) 才用库内自增
                auto insert = seq > 0
                                  ? db.prepare(
                                        "INSERT INTO view_message(seq, json, msg_id) VALUES (?, ?, ?)"
                                    )
                                  : db.prepare("INSERT INTO view_message(json, msg_id) VALUES (?, ?)");
                if (seq > 0) {
                    insert.bindInt64(1, static_cast<int64_t>(seq));
                    insert.bindText(2, dumpJsonUtf8(stripAttachmentDataUrl(msg).toJson()));
                    insert.bindText(3, msg.id);
                } else {
                    insert.bindText(1, dumpJsonUtf8(stripAttachmentDataUrl(msg).toJson()));
                    insert.bindText(2, msg.id);
                }
                insert.step();

                // UPSERT 计数: 新线程首条消息时 meta 不存在, 需 INSERT
                auto meta = db.prepare("INSERT INTO meta(key, value) VALUES (?, ?) "
                                       "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
                meta.bindText(1, kMetaMsgIdCounter);
                meta.bindInt64(2, static_cast<int64_t>(msgIdCounter));
                meta.step();

                // 展示历史序号计数 (计划 STO-4; 供重启后继续编号与增量补拉)
                if (seq > 0) {
                    meta.reset();
                    meta.bindText(1, kMetaViewSeqCounter);
                    meta.bindInt64(2, static_cast<int64_t>(seq));
                    meta.step();
                }

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
                        // 标题来源: 自动标题 (用户改名会覆盖为 "user", 见 setSessionTitle)
                        titleStmt.reset();
                        titleStmt.bindText(1, kMetaTitleSource);
                        titleStmt.bindText(2, "auto");
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
            // 记录写失败原因 (计划 STO-9: 会话侧据此提示"消息未落盘")
            // - 只记根因 (不含操作名): 同一次故障下"追加消息"与"保存上下文"两条
            //   写路径的失败原因一致, 会话侧的提示去重才能合并成一条
            lastWriteError_ = errmsg;
            return false;
        }
    );
}

void SessionStore::saveLlmMessages(
    std::string_view         sessionId,
    const utilxx_base::Json& llmMessages
) {
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
                insert.bindText(1, dumpJsonUtf8(llmMessages));
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
            // 记录写失败原因 (计划 STO-9; 只记根因, 见 appendViewMessage 处注释)
            lastWriteError_ = errmsg;
            return false;
        }
    );
}

// ---------------------------------------------------------------------------
// 展示历史增量补拉 (计划 STO-4)
// ---------------------------------------------------------------------------

std::vector<SessionStore::ViewMessageRow> SessionStore::loadViewMessagesAfter(
    std::string_view sessionId,
    uint64_t         afterSeq,
    size_t           limit
) {
    std::vector<ViewMessageRow> out;
    // 只读路径: 目录不存在直接返回空 (不创建目录/不取写租约)
    if (!sessionDataDirExists(sessionId)) {
        return out;
    }
    const auto dbFile = fs::path(rootDir_) / sanitizeSessionId(sessionId) / "session.db";
    agentxx::util::SqliteDb db;
    const auto             opened = agentxx::util::catchError<bool>(
        [&]() -> bool {
            db.open(dbFile.string());
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGW(
                "SessionStore: loadViewMessagesAfter({}) open {} failed: {}",
                sessionId,
                dbFile.string(),
                errmsg
            );
            return false;
        }
    );
    if (!opened) {
        return out;
    }
    if (const int version = readSchemaVersion(db); version > kSchemaVersion) {
        XX_LOGE(
            "SessionStore: loadViewMessagesAfter({}) refused: schema version {} is newer than "
            "supported {}",
            sessionId,
            version,
            kSchemaVersion
        );
        return out;
    }
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto stmt = limit > 0
                            ? db.prepare("SELECT seq, json FROM view_message WHERE seq > ? "
                                         "ORDER BY seq LIMIT ?")
                            : db.prepare("SELECT seq, json FROM view_message WHERE seq > ? "
                                         "ORDER BY seq");
            stmt.bindInt64(1, static_cast<int64_t>(afterSeq));
            if (limit > 0) {
                stmt.bindInt64(2, static_cast<int64_t>(limit));
            }
            while (stmt.step()) {
                ViewMessageRow row;
                row.seq = static_cast<uint64_t>(stmt.columnInt64(0));
                // 单行解析失败只跳过该行 (与 loadSession 同一容错策略)
                const auto jsonText = stmt.columnText(1);
                agentxx::util::catchError<bool>(
                    [&]() -> bool {
                        row.message = ViewMessage::fromJson(utilxx_base::Json::parse(jsonText));
                        out.push_back(std::move(row));
                        return true;
                    },
                    [&](std::string errmsg) -> bool {
                        XX_LOGW(
                            "SessionStore: loadViewMessagesAfter({}) 跳过无法解析的消息 "
                            "(seq={}): {}",
                            sessionId,
                            row.seq,
                            errmsg
                        );
                        return false;
                    }
                );
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE(
                "SessionStore: loadViewMessagesAfter({}) failed: {}",
                sessionId,
                errmsg
            );
            return false;
        }
    );
    return out;
}

// ---------------------------------------------------------------------------
// 输入收件箱 (session.db session_input 表; 计划 LOOP-1)
// ---------------------------------------------------------------------------
void SessionStore::addSessionInput(std::string_view sessionId, const SessionInputRecord& record) {
    if (record.id.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db = dbs(sessionId).sessionDb;
            auto  insert = db.prepare(
                "INSERT INTO session_input(id, payload, delivery, status, admitted_seq, "
                "promoted_seq, created_ms) VALUES (?, ?, ?, ?, ?, ?, ?) "
                "ON CONFLICT(id) DO UPDATE SET payload = excluded.payload, "
                "delivery = excluded.delivery, status = excluded.status"
            );
            insert.bindText(1, record.id);
            insert.bindText(2, record.payload);
            insert.bindText(3, record.delivery);
            insert.bindText(4, record.status.empty() ? std::string{SessionInputStatus::Admitted}
                                                     : record.status);
            insert.bindInt64(5, record.admittedSeq);
            insert.bindInt64(6, record.promotedSeq);
            insert.bindInt64(7, record.createdMs);
            insert.step();
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionStore: addSessionInput({}) failed: {}", sessionId, errmsg);
            return false;
        }
    );
}

void SessionStore::markSessionInputPromoted(
    std::string_view sessionId,
    std::string_view id,
    uint64_t         promotedSeq
) {
    if (id.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db     = dbs(sessionId).sessionDb;
            auto  update = db.prepare(
                "UPDATE session_input SET status = ?, promoted_seq = ? WHERE id = ?"
            );
            update.bindText(1, std::string{SessionInputStatus::Promoted});
            update.bindInt64(2, static_cast<int64_t>(promotedSeq));
            update.bindText(3, id);
            update.step();
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionStore: markSessionInputPromoted({}) failed: {}", sessionId, errmsg);
            return false;
        }
    );
}

void SessionStore::setSessionInputStatus(
    std::string_view sessionId,
    std::string_view id,
    std::string_view status
) {
    if (id.empty() || status.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db     = dbs(sessionId).sessionDb;
            auto  update = db.prepare("UPDATE session_input SET status = ? WHERE id = ?");
            update.bindText(1, status);
            update.bindText(2, id);
            update.step();
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionStore: setSessionInputStatus({}) failed: {}", sessionId, errmsg);
            return false;
        }
    );
}

std::vector<SessionStore::SessionInputRecord>
    SessionStore::listSessionInputs(std::string_view sessionId, std::string_view statusFilter) {
    std::vector<SessionInputRecord> out;
    // 只读路径: 目录不存在 = 从未写入过, 直接返回空 (不创建目录/空库)
    // - 端点启动时的收件箱恢复走本方法: 若在此建库, 仅连接、还没发过消息的会话
    //   也会在磁盘留下空会话, 会话列表里就多出一条无内容条目
    if (!sessionDataDirExists(sessionId)) {
        return out;
    }
    std::lock_guard<std::mutex>     lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db = dbs(sessionId).sessionDb;
            auto  stmt
                = statusFilter.empty()
                      ? db.prepare("SELECT id, payload, delivery, status, admitted_seq, "
                                   "promoted_seq, created_ms FROM session_input ORDER BY created_ms, id")
                      : db.prepare("SELECT id, payload, delivery, status, admitted_seq, "
                                   "promoted_seq, created_ms FROM session_input WHERE status = ? "
                                   "ORDER BY created_ms, id");
            if (!statusFilter.empty()) {
                stmt.bindText(1, statusFilter);
            }
            while (stmt.step()) {
                SessionInputRecord record;
                record.id          = stmt.columnText(0);
                record.payload     = stmt.columnText(1);
                record.delivery    = stmt.columnText(2);
                record.status      = stmt.columnText(3);
                record.admittedSeq = stmt.columnInt64(4);
                record.promotedSeq = stmt.columnInt64(5);
                record.createdMs   = stmt.columnInt64(6);
                out.push_back(std::move(record));
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionStore: listSessionInputs({}) failed: {}", sessionId, errmsg);
            return false;
        }
    );
    return out;
}

// ---------------------------------------------------------------------------
// 用量记录 (session.db usage 表)
// ---------------------------------------------------------------------------

void SessionStore::addUsage(std::string_view sessionId, const UsageRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db     = dbs(sessionId).sessionDb;
            auto  insert = db.prepare(
                "INSERT INTO usage(time_ms, model, prompt_tokens, completion_tokens, "
                "total_tokens, cached_prompt_tokens, cache_write_prompt_tokens, "
                "reasoning_tokens, ok, error_kind) "
                "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
            );
            insert.bindInt64(1, record.timeMs);
            insert.bindText(2, record.model);
            insert.bindInt64(3, record.promptTokens);
            insert.bindInt64(4, record.completionTokens);
            insert.bindInt64(5, record.totalTokens);
            insert.bindInt64(6, record.cachedPromptTokens);
            insert.bindInt64(7, record.cacheWritePromptTokens);
            insert.bindInt64(8, record.reasoningTokens);
            insert.bindInt64(9, record.ok ? 1 : 0);
            insert.bindText(10, record.errorKind);
            insert.step();
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE(
                "SessionStore: addUsage({}, model={}) failed: {}",
                sessionId,
                record.model,
                errmsg
            );
            return false;
        }
    );
}

SessionStore::UsageSummary SessionStore::usageSummary(std::string_view sessionId) {
    UsageSummary out;
    // 目录不存在 = 从未写入过, 直接返回全 0 (不创建目录/空库)
    if (!sessionDataDirExists(sessionId)) {
        return out;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db   = dbs(sessionId).sessionDb;
            auto  stmt = db.prepare(
                "SELECT count(*), "
                "COALESCE(sum(CASE WHEN ok = 0 THEN 1 ELSE 0 END), 0), "
                "COALESCE(sum(prompt_tokens), 0), COALESCE(sum(completion_tokens), 0), "
                "COALESCE(sum(total_tokens), 0), COALESCE(sum(cached_prompt_tokens), 0), "
                "COALESCE(sum(reasoning_tokens), 0), "
                "COALESCE(sum(cache_write_prompt_tokens), 0) FROM usage"
            );
            if (stmt.step()) {
                out.calls                  = stmt.columnInt64(0);
                out.failedCalls            = stmt.columnInt64(1);
                out.promptTokens           = stmt.columnInt64(2);
                out.completionTokens       = stmt.columnInt64(3);
                out.totalTokens            = stmt.columnInt64(4);
                out.cachedPromptTokens     = stmt.columnInt64(5);
                out.reasoningTokens        = stmt.columnInt64(6);
                out.cacheWritePromptTokens = stmt.columnInt64(7);
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionStore: usageSummary({}) failed: {}", sessionId, errmsg);
            return false;
        }
    );
    return out;
}

std::vector<SessionStore::UsageRecord>
    SessionStore::recentUsage(std::string_view sessionId, size_t limit) {
    std::vector<UsageRecord> out;
    if (limit == 0 || !sessionDataDirExists(sessionId)) {
        return out;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db   = dbs(sessionId).sessionDb;
            auto  stmt = db.prepare(
                "SELECT time_ms, model, prompt_tokens, completion_tokens, total_tokens, "
                "cached_prompt_tokens, reasoning_tokens, ok, error_kind, "
                "cache_write_prompt_tokens FROM usage "
                "ORDER BY id DESC LIMIT ?"
            );
            stmt.bindInt64(1, static_cast<int64_t>(limit));
            while (stmt.step()) {
                UsageRecord rec;
                rec.timeMs                  = stmt.columnInt64(0);
                rec.model                   = stmt.columnText(1);
                rec.promptTokens            = stmt.columnInt64(2);
                rec.completionTokens        = stmt.columnInt64(3);
                rec.totalTokens             = stmt.columnInt64(4);
                rec.cachedPromptTokens      = stmt.columnInt64(5);
                rec.reasoningTokens         = stmt.columnInt64(6);
                rec.ok                      = stmt.columnInt64(7) != 0;
                rec.errorKind               = stmt.columnText(8);
                rec.cacheWritePromptTokens  = stmt.columnInt64(9);
                out.push_back(std::move(rec));
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionStore: recentUsage({}) failed: {}", sessionId, errmsg);
            return false;
        }
    );
    return out;
}

// ---------------------------------------------------------------------------
// 会话标题与检索 (meta.title / meta.titleSource)
// ---------------------------------------------------------------------------

std::string SessionStore::readMetaValue(std::string_view sessionId, std::string_view key) const {
    const auto      dbFile = fs::path(rootDir_) / sanitizeSessionId(sessionId) / "session.db";
    std::error_code ec;
    if (!fs::exists(dbFile, ec)) {
        return {};
    }
    std::string out;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            agentxx::util::SqliteDb db;
            db.open(dbFile.string());
            auto stmt = db.prepare("SELECT value FROM meta WHERE key = ?");
            stmt.bindText(1, key);
            if (stmt.step()) {
                out = stmt.columnText(0);
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGD("SessionStore: readMetaValue({}, {}) failed: {}", sessionId, key, errmsg);
            return false;
        }
    );
    return out;
}

std::string SessionStore::sessionTitle(std::string_view sessionId) {
    return readMetaValue(sessionId, kMetaTitle);
}

std::string SessionStore::sessionTitleSource(std::string_view sessionId) {
    return readMetaValue(sessionId, kMetaTitleSource);
}

bool SessionStore::setSessionTitle(std::string_view sessionId, std::string_view title) {
    if (title.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db = dbs(sessionId).sessionDb;
            db.beginImmediate();
            bool inTx = true;
            try {
                // 标题与来源一起提交: 界面据此区分"自动标题/用户改名"
                auto stmt = db.prepare("INSERT INTO meta(key, value) VALUES (?, ?) "
                                       "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
                stmt.bindText(1, kMetaTitle);
                stmt.bindText(2, title);
                stmt.step();
                stmt.reset();
                stmt.bindText(1, kMetaTitleSource);
                stmt.bindText(2, "user");
                stmt.step();
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
            XX_LOGE("SessionStore: setSessionTitle({}) failed: {}", sessionId, errmsg);
            return false;
        }
    );
}

/// 转义 LIKE 关键词中的通配符 (`\` `%` `_`), 使子串匹配语义严格
static std::string escapeLikeKeyword(std::string_view keyword) {
    std::string out;
    out.reserve(keyword.size() + 8);
    for (const char c : keyword) {
        if (c == '\\' || c == '%' || c == '_') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

/// 从命中文本中截取关键词附近的片段 (用于检索结果展示)
/// - 关键词大小写不敏感定位 (ASCII); 找不到时取文本开头
/// - 片段长度上限约 120 字节, 两端被截断时加省略号
static std::string makeSearchSnippet(std::string_view text, std::string_view keyword) {
    if (text.empty()) {
        return {};
    }
    auto lower = [](std::string_view s) {
        std::string out{s};
        for (auto& c : out) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c + ('a' - 'A'));
            }
        }
        return out;
    };
    const auto loweredText = lower(text);
    const auto loweredKey  = lower(keyword);
    size_t     pos         = loweredText.find(loweredKey);
    if (pos == std::string::npos) {
        pos = 0;
    }
    const size_t begin = (pos > 40) ? (pos - 40) : 0;
    const size_t end   = std::min(text.size(), begin + 120);
    std::string  snippet{text.substr(begin, end - begin)};
    // 片段内的换行折成空格, 避免列表里显示为多行
    for (auto& c : snippet) {
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
    }
    if (begin > 0) {
        snippet.insert(0, "...");
    }
    if (end < text.size()) {
        snippet += "...";
    }
    return snippet;
}

std::vector<SessionStore::SessionSearchHit>
    SessionStore::searchSessions(std::string_view keyword, size_t limit) {
    std::vector<SessionSearchHit> out;
    if (keyword.empty()) {
        return out;
    }
    const auto pattern = "%" + escapeLikeKeyword(keyword) + "%";
    if (limit == 0) {
        limit = kSearchDefaultLimit;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            std::error_code ec;
            fs::path        root{rootDir_};
            if (!fs::exists(root, ec)) {
                return true;
            }
            for (const auto& entry : fs::directory_iterator(root, ec)) {
                if (ec || !entry.is_directory(ec)) {
                    continue;
                }
                SessionSearchHit hit;
                hit.info.sessionId = entry.path().filename().string();
                readSessionDirMeta(entry.path(), hit.info);

                const auto dbFile = entry.path() / "session.db";
                agentxx::util::catchError<bool>(
                    [&]() -> bool {
                        agentxx::util::SqliteDb db;
                        db.open(dbFile.string());
                        // 标题命中
                        auto titleStmt = db.prepare(
                            "SELECT 1 FROM meta WHERE key = 'title' AND value LIKE ? ESCAPE '\\'"
                        );
                        titleStmt.bindText(1, pattern);
                        if (titleStmt.step()) {
                            hit.titleMatch = true;
                            out.push_back(std::move(hit));
                            return true;
                        }
                        // 正文命中: 取最后一条命中的消息文本作为片段
                        auto textStmt = db.prepare(
                            "SELECT json_extract(json, '$.text') FROM view_message "
                            "WHERE json_extract(json, '$.text') LIKE ? ESCAPE '\\' "
                            "ORDER BY seq DESC LIMIT 1"
                        );
                        textStmt.bindText(1, pattern);
                        if (textStmt.step() && !textStmt.columnIsNull(0)) {
                            hit.snippet
                                = makeSearchSnippet(textStmt.columnText(0), keyword);
                            out.push_back(std::move(hit));
                        }
                        return true;
                    },
                    [&](std::string errmsg) -> bool {
                        XX_LOGD(
                            "SessionStore: searchSessions skip {} ({})",
                            entry.path().filename().string(),
                            errmsg
                        );
                        return false;
                    }
                );
            }
            std::sort(out.begin(), out.end(), [](const SessionSearchHit& a, const SessionSearchHit& b) {
                return sessionNewerFirst(a.info, b.info);
            });
            if (out.size() > limit) {
                out.resize(limit);
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionStore: searchSessions failed: {}", errmsg);
            return false;
        }
    );
    return out;
}

// ---------------------------------------------------------------------------
// share store (session.db store 表)
// ---------------------------------------------------------------------------

size_t SessionStore::shareStoreLastId(std::string_view sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t                      out = 0;
    // 目录不存在 = 从未写入过, 直接返回 0
    if (!sessionDataDirExists(sessionId)) {
        return out;
    }
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto& db = dbs(sessionId).sessionDb;
            // 只取 id 的最大值, 不读取内容 (内容在取值时按 id 单独读取)
            auto stmt = db.prepare("SELECT COALESCE(MAX(id), 0) FROM store");
            if (stmt.step()) {
                const auto maxId = stmt.columnInt64(0);
                out              = (maxId > 0) ? static_cast<size_t>(maxId) : size_t{0};
            }
            return true;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGE("SessionStore: shareStoreLastId({}) failed: {}", sessionId, errmsg);
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
            // 自增 id: 取现有最大 id + 1, 重启后延续 (与内存中记录的最大 id
            // 一致, 见 MiddlewareContext::SessionShareStore::lastId)
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

} // namespace agent
} // namespace agentxx
