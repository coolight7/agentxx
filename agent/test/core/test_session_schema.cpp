#include "agentxx-test/core/test_session_schema.h"

#include "agentxx-test/core/test_writer_lease.h"
#include "agentxx/agent/session_store.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/sqlite.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <string>
#include <string_view>
#include <thread>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ss_passed = 0;
int g_ss_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_ss_passed
#define XX_TEST_FAILED g_ss_failed

namespace agentxx {
namespace test {

using agentxx::agent::SessionStore;

namespace fs = std::filesystem;

namespace {

/// 创建唯一临时目录 (测试根目录)
std::string makeTempRoot() {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_ss_test_{}",
                   std::chrono::steady_clock::now().time_since_epoch().count()
               );
    fs::create_directories(dir);
    return dir.string();
}

/// 删除测试临时目录 (Windows 上文件句柄可能稍晚释放, 短暂重试)
void removeTempRoot(const std::string& root) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::error_code ec;
        fs::remove_all(utilxx_base::utf8ToPath(root), ec);
        if (!ec) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    std::error_code ec;
    fs::remove_all(utilxx_base::utf8ToPath(root), ec);
    if (ec) {
        XX_LOGW("清理测试临时目录失败: {} ({})", root, ec.message());
    }
}

/// 会话目录 (SessionStore 用清洗后的 sessionId 作目录名; 测试用安全字符)
std::string sessionDir(const std::string& root, std::string_view sessionId) {
    return (fs::path{root} / std::string{sessionId}).string();
}

/// 会话库文件路径
std::string sessionDbFile(const std::string& root, std::string_view sessionId) {
    return (fs::path{sessionDir(root, sessionId)} / "session.db").string();
}

/// 构造一条仅含基础字段的展示消息
agentxx::agent::ViewMessage makeMsg(std::string id, std::string text) {
    agentxx::agent::ViewMessage msg;
    msg.id          = std::move(id);
    msg.role        = agentxx::agent::ViewMessage::Role::User;
    msg.text        = std::move(text);
    msg.startTimeMs = 1700000000000LL;
    return msg;
}

/// 读取 meta.schema_version (-1 = 无记录/读失败)
int readSchemaVersionAt(const std::string& dbFile) {
    int out = -1;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            agentxx::util::SqliteDb db;
            db.open(dbFile);
            auto stmt = db.prepare("SELECT value FROM meta WHERE key = 'schema_version'");
            if (stmt.step()) {
                out = static_cast<int>(stmt.columnInt64(0));
            }
            return true;
        },
        [&](std::string) -> bool {
            return false;
        }
    );
    return out;
}

/// 表是否存在指定列
bool tableHasColumn(const std::string& dbFile, std::string_view table, std::string_view column) {
    bool out = false;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            agentxx::util::SqliteDb db;
            db.open(dbFile);
            auto stmt = db.prepare(fmt::format("PRAGMA table_info({})", table));
            while (stmt.step()) {
                if (stmt.columnText(1) == column) {
                    out = true;
                    break;
                }
            }
            return true;
        },
        [&](std::string) -> bool {
            return false;
        }
    );
    return out;
}

/// 表是否存在
bool tableExists(const std::string& dbFile, std::string_view table) {
    bool out = false;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            agentxx::util::SqliteDb db;
            db.open(dbFile);
            auto stmt = db.prepare("SELECT count(*) FROM sqlite_master WHERE type='table' AND name=?");
            stmt.bindText(1, table);
            if (stmt.step()) {
                out = stmt.columnInt64(0) > 0;
            }
            return true;
        },
        [&](std::string) -> bool {
            return false;
        }
    );
    return out;
}

/// 表行数 (-1 = 读失败)
int64_t countRows(const std::string& dbFile, std::string_view table) {
    int64_t out = -1;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            agentxx::util::SqliteDb db;
            db.open(dbFile);
            auto stmt = db.prepare(fmt::format("SELECT count(*) FROM {}", table));
            if (stmt.step()) {
                out = stmt.columnInt64(0);
            }
            return true;
        },
        [&](std::string) -> bool {
            return false;
        }
    );
    return out;
}

/// 按 msg_id 查一行 json (不存在返回空串)
std::string jsonOfMsgId(const std::string& dbFile, std::string_view msgId) {
    std::string out;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            agentxx::util::SqliteDb db;
            db.open(dbFile);
            auto stmt = db.prepare("SELECT json FROM view_message WHERE msg_id = ?");
            stmt.bindText(1, msgId);
            if (stmt.step()) {
                out = stmt.columnText(0);
            }
            return true;
        },
        [&](std::string) -> bool {
            return false;
        }
    );
    return out;
}

/// 建立一个"老版本"会话库: 只有四张老表, 无 msg_id 列 / 无 schema_version /
/// 无 usage 表 (模拟计划 STO-2 提到的历史数据结构)
void createLegacyDb(const std::string& dbFile) {
    fs::create_directories(utilxx_base::utf8ToPath(fs::path{dbFile}.parent_path().string()));
    agentxx::util::SqliteDb db;
    db.open(dbFile);
    db.exec(R"sql(
CREATE TABLE view_message (
    seq  INTEGER PRIMARY KEY AUTOINCREMENT,
    json TEXT NOT NULL
);
CREATE TABLE llm_context (
    id   INTEGER PRIMARY KEY CHECK (id = 1),
    json TEXT NOT NULL
);
CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE store (
    id    INTEGER PRIMARY KEY,
    value TEXT NOT NULL
);
)sql");
    db.exec(
        "INSERT INTO view_message(json) VALUES "
        "('{\"id\":\"legacy-1\",\"role\":\"user\",\"text\":\"legacy hello\"}')"
    );
    db.exec("INSERT INTO meta(key, value) VALUES('msgIdCounter', '1')");
    db.close();
}

/// 建立一个 v2 结构的会话库 (计划 LLM-8 的迁移起点):
/// 结构上到 v2 为止 (usage 表**没有** cache_write_prompt_tokens 列, 有 session_input),
/// 且 meta.schema_version = 2, 用于验证按相邻步骤迁移到 v3
void createV2Db(const std::string& dbFile) {
    fs::create_directories(utilxx_base::utf8ToPath(fs::path{dbFile}.parent_path().string()));
    agentxx::util::SqliteDb db;
    db.open(dbFile);
    db.exec(R"sql(
CREATE TABLE view_message (
    seq    INTEGER PRIMARY KEY AUTOINCREMENT,
    json   TEXT NOT NULL,
    msg_id TEXT
);
CREATE TABLE llm_context (
    id   INTEGER PRIMARY KEY CHECK (id = 1),
    json TEXT NOT NULL
);
CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE store (
    id    INTEGER PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE usage (
    id                   INTEGER PRIMARY KEY AUTOINCREMENT,
    time_ms              INTEGER NOT NULL,
    model                TEXT NOT NULL,
    prompt_tokens        INTEGER NOT NULL DEFAULT 0,
    completion_tokens    INTEGER NOT NULL DEFAULT 0,
    total_tokens         INTEGER NOT NULL DEFAULT 0,
    cached_prompt_tokens INTEGER NOT NULL DEFAULT 0,
    reasoning_tokens     INTEGER NOT NULL DEFAULT 0,
    ok                   INTEGER NOT NULL DEFAULT 1,
    error_kind           TEXT NOT NULL DEFAULT ''
);
CREATE TABLE session_input (
    id           TEXT PRIMARY KEY,
    payload      TEXT NOT NULL,
    delivery     TEXT NOT NULL DEFAULT '',
    status       TEXT NOT NULL DEFAULT 'admitted',
    admitted_seq INTEGER NOT NULL DEFAULT 0,
    promoted_seq INTEGER NOT NULL DEFAULT 0,
    created_ms   INTEGER NOT NULL DEFAULT 0
);
)sql");
    db.exec("INSERT INTO meta(key, value) VALUES('schema_version', '2')");
    // 历史用量记录: v2 形态 (没有"缓存写入量"这一列)
    db.exec(
        "INSERT INTO usage(time_ms, model, prompt_tokens, completion_tokens, total_tokens, "
        "cached_prompt_tokens, reasoning_tokens, ok, error_kind) "
        "VALUES(1700000000000, 'legacy-model', 10, 5, 15, 3, 1, 1, '')"
    );
    db.close();
}

} // namespace

TestResult testSessionSchema() {
    auto root = makeTempRoot();

    // -----------------------------------------------------------------------
    // A) 新库: 首次写入即建立完整结构并记录 schema 版本
    // -----------------------------------------------------------------------
    const auto dbNew = sessionDbFile(root, "s-new");
    {
        SessionStore store{root};
        store.appendViewMessage("s-new", makeMsg("m1", "hello"), 1);
        XX_TEST_EXPECT_TRUE(store.lastWriteError().empty());
        XX_TEST_EXPECT_EQ(readSchemaVersionAt(dbNew), SessionStore::kSchemaVersion);
        XX_TEST_EXPECT_TRUE(tableHasColumn(dbNew, "view_message", "msg_id"));
        XX_TEST_EXPECT_TRUE(tableExists(dbNew, "usage"));
        XX_TEST_EXPECT_TRUE(tableHasColumn(dbNew, "usage", "error_kind"));
        XX_TEST_EXPECT_FALSE(fs::exists(utilxx_base::utf8ToPath(dbNew + ".bak.v0")));
    }

    // -----------------------------------------------------------------------
    // B) 老库: 缺少 msg_id 列 / schema_version / usage 表时自动补齐, 数据保留,
    //    迁移前留一份备份
    // -----------------------------------------------------------------------
    const auto dbLegacy = sessionDbFile(root, "s-legacy");
    createLegacyDb(dbLegacy);
    {
        SessionStore store{root};
        // 读路径不迁移: 老库的历史按老结构照常读到
        auto loaded = store.loadSession("s-legacy");
        XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), size_t{1});
        XX_TEST_EXPECT_EQ(loaded.viewMessages[0].id, std::string{"legacy-1"});
        XX_TEST_EXPECT_EQ(loaded.msgIdCounter, uint64_t{1});
        XX_TEST_EXPECT_EQ(readSchemaVersionAt(dbLegacy), -1); // 还没写过, 版本仍未记录

        // 写路径触发迁移
        store.appendViewMessage("s-legacy", makeMsg("m2", "after migration"), 2);
        XX_TEST_EXPECT_TRUE(store.lastWriteError().empty());
    }
    XX_TEST_EXPECT_EQ(readSchemaVersionAt(dbLegacy), SessionStore::kSchemaVersion);
    XX_TEST_EXPECT_TRUE(tableHasColumn(dbLegacy, "view_message", "msg_id"));
    XX_TEST_EXPECT_TRUE(tableExists(dbLegacy, "usage"));
    XX_TEST_EXPECT_EQ(countRows(dbLegacy, "view_message"), int64_t{2});
    // 老行的 msg_id 由 json 回填
    XX_TEST_EXPECT_TRUE(!jsonOfMsgId(dbLegacy, "legacy-1").empty());
    XX_TEST_EXPECT_TRUE(fs::exists(utilxx_base::utf8ToPath(dbLegacy + ".bak.v0")));

    // -----------------------------------------------------------------------
    // C) 幂等: 再次打开并写入不重复迁移 (版本不变, 备份文件不被重写)
    // -----------------------------------------------------------------------
    std::error_code     ec;
    const auto          backupFile = dbLegacy + ".bak.v0";
    const auto          backupTime = fs::last_write_time(utilxx_base::utf8ToPath(backupFile), ec);
    const int64_t       backupSize = static_cast<int64_t>(
        fs::file_size(utilxx_base::utf8ToPath(backupFile), ec)
    );
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    {
        SessionStore store{root};
        store.appendViewMessage("s-legacy", makeMsg("m3", "third"), 3);
        XX_TEST_EXPECT_TRUE(store.lastWriteError().empty());
    }
    XX_TEST_EXPECT_EQ(readSchemaVersionAt(dbLegacy), SessionStore::kSchemaVersion);
    XX_TEST_EXPECT_EQ(countRows(dbLegacy, "view_message"), int64_t{3});
    XX_TEST_EXPECT_EQ(
        fs::last_write_time(utilxx_base::utf8ToPath(backupFile), ec).time_since_epoch().count(),
        backupTime.time_since_epoch().count()
    );
    (void)backupSize;

    // -----------------------------------------------------------------------
    // D) 高版本库: 读写都拒绝, 不破坏原有数据
    // -----------------------------------------------------------------------
    const auto dbFuture = sessionDbFile(root, "s-future");
    createLegacyDb(dbFuture);
    {
        agentxx::util::SqliteDb db;
        db.open(dbFuture);
        db.exec("INSERT OR REPLACE INTO meta(key, value) VALUES('schema_version', '99')");
        db.close();
    }
    {
        SessionStore store{root};
        auto         loaded = store.loadSession("s-future");
        XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), size_t{0});
        store.appendViewMessage("s-future", makeMsg("m9", "should not be written"), 2);
        XX_TEST_EXPECT_FALSE(store.lastWriteError().empty());
        store.saveLlmMessages("s-future", utilxx_base::Json::array());
        XX_TEST_EXPECT_FALSE(store.lastWriteError().empty());
        XX_TEST_EXPECT_EQ(countRows(dbFuture, "llm_context"), int64_t{0});
    }
    XX_TEST_EXPECT_EQ(readSchemaVersionAt(dbFuture), 99);
    XX_TEST_EXPECT_EQ(countRows(dbFuture, "view_message"), int64_t{1});

    // -----------------------------------------------------------------------
    // D2) 迁移中断 (计划 TST-3): 迁移步骤失败时不推进版本、不改坏老数据;
    //     排除故障后重新打开可从旧版本续做 (每步独立事务)
    // -----------------------------------------------------------------------
    const auto dbBroken = sessionDbFile(root, "s-broken");
    createLegacyDb(dbBroken);
    {
        // 制造"迁移做不下去"的库状态: 建一个与 schema 里 session_input 同名的视图,
        // 迁移建索引时必然失败 (SQLite: views may not be indexed)
        agentxx::util::SqliteDb db;
        db.open(dbBroken);
        db.exec("CREATE VIEW session_input AS SELECT 1 AS id");
        db.close();
    }
    {
        SessionStore store{root};
        // 读路径不受影响: 老库的历史按老结构照常读到
        auto loaded = store.loadSession("s-broken");
        XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), size_t{1});
        // 写路径: 迁移失败明确失败, 不落数据
        store.appendViewMessage("s-broken", makeMsg("m-broken", "should not be written"), 2);
        XX_TEST_EXPECT_FALSE(store.lastWriteError().empty());
    }
    // 版本未推进 (仍是老库), 老数据不丢, 失败步骤没有留下半成品结构
    XX_TEST_EXPECT_EQ(readSchemaVersionAt(dbBroken), -1);
    XX_TEST_EXPECT_EQ(countRows(dbBroken, "view_message"), int64_t{1});
    XX_TEST_EXPECT_FALSE(tableExists(dbBroken, "usage"));
    XX_TEST_EXPECT_FALSE(tableHasColumn(dbBroken, "view_message", "msg_id"));
    // 迁移失败前的老行仍在, 只是还没有回填 msg_id
    XX_TEST_EXPECT_TRUE(jsonOfMsgId(dbBroken, "legacy-1").empty());

    {
        // 排除故障后重新打开: 从旧版本续做, 迁移完成且历史保留
        agentxx::util::SqliteDb db;
        db.open(dbBroken);
        db.exec("DROP VIEW session_input");
        db.close();
    }
    {
        SessionStore store{root};
        store.appendViewMessage("s-broken", makeMsg("m-after", "after fix"), 2);
        XX_TEST_EXPECT_TRUE(store.lastWriteError().empty());
    }
    XX_TEST_EXPECT_EQ(readSchemaVersionAt(dbBroken), SessionStore::kSchemaVersion);
    XX_TEST_EXPECT_EQ(countRows(dbBroken, "view_message"), int64_t{2});
    // 迁移失败期间被拒的消息确实没写进去, 修好后写入的是新消息
    XX_TEST_EXPECT_TRUE(!jsonOfMsgId(dbBroken, "legacy-1").empty());
    XX_TEST_EXPECT_TRUE(!jsonOfMsgId(dbBroken, "m-after").empty());
    XX_TEST_EXPECT_TRUE(jsonOfMsgId(dbBroken, "m-broken").empty());

    // -----------------------------------------------------------------------
    // D2) v2 -> v3: 用量记录补"缓存写入量"列 (计划 LLM-8), 老记录按 0 保留
    // -----------------------------------------------------------------------
    const auto dbV2 = sessionDbFile(root, "s-v2");
    createV2Db(dbV2);
    XX_TEST_EXPECT_TRUE(tableExists(dbV2, "usage"));
    XX_TEST_EXPECT_FALSE(tableHasColumn(dbV2, "usage", "cache_write_prompt_tokens"));
    XX_TEST_EXPECT_EQ(readSchemaVersionAt(dbV2), 2);
    {
        SessionStore store{root};
        // 写路径触发迁移; 迁移后写入一条带缓存写入量的记录
        SessionStore::UsageRecord rec;
        rec.timeMs                 = 2000;
        rec.model                  = "claude-x";
        rec.promptTokens           = 1512;
        rec.completionTokens       = 20;
        rec.totalTokens            = 1532;
        rec.cachedPromptTokens     = 900;
        rec.cacheWritePromptTokens = 512;
        store.addUsage("s-v2", rec);
        XX_TEST_EXPECT_TRUE(store.lastWriteError().empty());
    }
    XX_TEST_EXPECT_EQ(readSchemaVersionAt(dbV2), SessionStore::kSchemaVersion);
    XX_TEST_EXPECT_TRUE(tableHasColumn(dbV2, "usage", "cache_write_prompt_tokens"));
    XX_TEST_EXPECT_TRUE(fs::exists(utilxx_base::utf8ToPath(dbV2 + ".bak.v2")));
    {
        SessionStore store{root};
        auto         recent = store.recentUsage("s-v2", 5);
        XX_TEST_EXPECT_EQ(recent.size(), size_t{2});
        if (recent.size() == 2) {
            // 新记录: 缓存读/写量都在
            XX_TEST_EXPECT_EQ(recent[0].cachedPromptTokens, int64_t{900});
            XX_TEST_EXPECT_EQ(recent[0].cacheWritePromptTokens, int64_t{512});
            // 老记录: 无缓存写入量 → 0, 其余字段原样保留
            XX_TEST_EXPECT_EQ(recent[1].model, std::string{"legacy-model"});
            XX_TEST_EXPECT_EQ(recent[1].promptTokens, int64_t{10});
            XX_TEST_EXPECT_EQ(recent[1].cachedPromptTokens, int64_t{3});
            XX_TEST_EXPECT_EQ(recent[1].cacheWritePromptTokens, int64_t{0});
        }
        auto sum = store.usageSummary("s-v2");
        XX_TEST_EXPECT_EQ(sum.calls, int64_t{2});
        XX_TEST_EXPECT_EQ(sum.cachedPromptTokens, int64_t{903});
        XX_TEST_EXPECT_EQ(sum.cacheWritePromptTokens, int64_t{512});
    }

    // -----------------------------------------------------------------------
    // E) 用量记录: 追加、聚合、最近若干条
    // -----------------------------------------------------------------------
    {
        SessionStore store{root};
        auto         sum0 = store.usageSummary("s-usage");
        XX_TEST_EXPECT_EQ(sum0.calls, int64_t{0});
        XX_TEST_EXPECT_FALSE(fs::exists(utilxx_base::utf8ToPath(sessionDir(root, "s-usage"))));

        SessionStore::UsageRecord ok1;
        ok1.timeMs           = 1000;
        ok1.model            = "test-sim";
        ok1.promptTokens     = 10;
        ok1.completionTokens = 5;
        ok1.totalTokens      = 15;
        ok1.ok               = true;
        store.addUsage("s-usage", ok1);

        SessionStore::UsageRecord ok2;
        ok2.timeMs             = 2000;
        ok2.model              = "test-sim";
        ok2.promptTokens       = 100;
        ok2.completionTokens   = 50;
        ok2.totalTokens        = 150;
        ok2.cachedPromptTokens = 80;
        ok2.reasoningTokens    = 20;
        ok2.ok                 = true;
        store.addUsage("s-usage", ok2);

        SessionStore::UsageRecord bad;
        bad.timeMs    = 3000;
        bad.model     = "test-sim";
        bad.ok        = false;
        bad.errorKind = "HTTP 500";
        store.addUsage("s-usage", bad);

        auto sum = store.usageSummary("s-usage");
        XX_TEST_EXPECT_EQ(sum.calls, int64_t{3});
        XX_TEST_EXPECT_EQ(sum.failedCalls, int64_t{1});
        XX_TEST_EXPECT_EQ(sum.promptTokens, int64_t{110});
        XX_TEST_EXPECT_EQ(sum.completionTokens, int64_t{55});
        XX_TEST_EXPECT_EQ(sum.totalTokens, int64_t{165});
        XX_TEST_EXPECT_EQ(sum.cachedPromptTokens, int64_t{80});
        XX_TEST_EXPECT_EQ(sum.reasoningTokens, int64_t{20});

        auto recent = store.recentUsage("s-usage", 2);
        XX_TEST_EXPECT_EQ(recent.size(), size_t{2});
        if (recent.size() == 2) {
            XX_TEST_EXPECT_EQ(recent[0].timeMs, int64_t{3000}); // 最新在前
            XX_TEST_EXPECT_FALSE(recent[0].ok);
            XX_TEST_EXPECT_EQ(recent[0].errorKind, std::string{"HTTP 500"});
            XX_TEST_EXPECT_EQ(recent[1].timeMs, int64_t{2000});
            XX_TEST_EXPECT_EQ(recent[1].cachedPromptTokens, int64_t{80});
            XX_TEST_EXPECT_EQ(recent[1].model, std::string{"test-sim"});
        }
        XX_TEST_EXPECT_EQ(store.recentUsage("s-usage", 0).size(), size_t{0});
    }
    // 用量记录落库: 重新打开仍能聚合 (用量记录是持久事实, 不依赖内存)
    {
        SessionStore store{root};
        auto         sum = store.usageSummary("s-usage");
        XX_TEST_EXPECT_EQ(sum.calls, int64_t{3});
        XX_TEST_EXPECT_EQ(sum.totalTokens, int64_t{165});
    }

    // -----------------------------------------------------------------------
    // F) 写租约集成: 目录被外部占用时写操作明确失败 (lastWriteError 非空),
    //    且不产出任何数据; 外部锁释放后恢复可写
    // -----------------------------------------------------------------------
    const auto dbBusy   = sessionDbFile(root, "s-busy");
    const auto dirBusy  = sessionDir(root, "s-busy");
    {
        ForeignWriterLock foreign{dirBusy};
        XX_TEST_EXPECT_TRUE(foreign.held());
        SessionStore store{root};
        store.appendViewMessage("s-busy", makeMsg("b1", "blocked"), 1);
        XX_TEST_EXPECT_FALSE(store.lastWriteError().empty());
        store.saveLlmMessages("s-busy", utilxx_base::Json::array());
        XX_TEST_EXPECT_FALSE(store.lastWriteError().empty());
    }
    XX_TEST_EXPECT_FALSE(fs::exists(utilxx_base::utf8ToPath(dbBusy)));
    {
        SessionStore store{root};
        store.appendViewMessage("s-busy", makeMsg("b1", "now ok"), 1);
        XX_TEST_EXPECT_TRUE(store.lastWriteError().empty());
    }
    XX_TEST_EXPECT_EQ(countRows(dbBusy, "view_message"), int64_t{1});
    // 会话可正常读回
    {
        SessionStore store{root};
        auto         loaded = store.loadSession("s-busy");
        XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), size_t{1});
    }

    // -----------------------------------------------------------------------
    // G) 会话标题与检索 (STO-12): 自动标题与来源、用户改名、关键词检索
    // -----------------------------------------------------------------------
    {
        SessionStore store{root};
        // 首条用户消息生成自动标题 (来源 auto)
        store.appendViewMessage("s-title", makeMsg("t1", "帮我查一下构建脚本 build script"), 1);
        XX_TEST_EXPECT_EQ(
            store.sessionTitle("s-title"),
            std::string{"帮我查一下构建脚本 build script"}
        );
        XX_TEST_EXPECT_EQ(store.sessionTitleSource("s-title"), std::string{"auto"});

        // 用户改名: 覆盖标题并记录来源
        XX_TEST_EXPECT_TRUE(store.setSessionTitle("s-title", "构建脚本排查"));
        XX_TEST_EXPECT_EQ(store.sessionTitle("s-title"), std::string{"构建脚本排查"});
        XX_TEST_EXPECT_EQ(store.sessionTitleSource("s-title"), std::string{"user"});

        // 后续自动标题不覆盖用户改名 (只写标题为空的情况)
        store.appendViewMessage("s-title", makeMsg("t2", "another user message"), 2);
        XX_TEST_EXPECT_EQ(store.sessionTitle("s-title"), std::string{"构建脚本排查"});

        // 空标题被忽略 (不把标题清空)
        XX_TEST_EXPECT_FALSE(store.setSessionTitle("s-title", ""));
        XX_TEST_EXPECT_EQ(store.sessionTitle("s-title"), std::string{"构建脚本排查"});

        // 未持久化的会话: 标题为空, 且不会因读取而创建目录
        XX_TEST_EXPECT_TRUE(store.sessionTitle("s-none").empty());
        XX_TEST_EXPECT_FALSE(fs::exists(utilxx_base::utf8ToPath(sessionDir(root, "s-none"))));
    }
    {
        SessionStore store{root};
        // 第一条消息不含关键词 (避免同时命中自动标题), 第二条才含关键词
        store.appendViewMessage("s-body", makeMsg("b1", "第一条消息 只是开场"), 1);
        store.appendViewMessage("s-body", makeMsg("b2", "正文里出现了 needle-keyword 关键词"), 2);

        // 标题命中
        auto hits           = store.searchSessions("构建脚本排查");
        bool titleHitFound = false;
        for (const auto& h : hits) {
            if (h.info.sessionId == "s-title") {
                titleHitFound = h.titleMatch;
            }
        }
        XX_TEST_EXPECT_TRUE(titleHitFound);

        // 正文命中: 返回命中片段
        auto bodyHits = store.searchSessions("needle-keyword");
        XX_TEST_EXPECT_EQ(bodyHits.size(), size_t{1});
        if (bodyHits.size() == 1) {
            XX_TEST_EXPECT_EQ(bodyHits[0].info.sessionId, std::string{"s-body"});
            XX_TEST_EXPECT_FALSE(bodyHits[0].titleMatch);
            XX_TEST_EXPECT_TRUE(bodyHits[0].snippet.find("needle-keyword") != std::string::npos);
        }

        // 通配符按字面匹配 (LIKE 通配符已转义), 空关键词返回空
        XX_TEST_EXPECT_EQ(store.searchSessions("%").size(), size_t{0});
        XX_TEST_EXPECT_EQ(store.searchSessions("_").size(), size_t{0});
        XX_TEST_EXPECT_EQ(store.searchSessions("").size(), size_t{0});

        // limit 截断: 两个会话命中同一关键词
        store.appendViewMessage("s-body2", makeMsg("c1", "needle-keyword 又出现一次"), 1);
        XX_TEST_EXPECT_EQ(store.searchSessions("needle-keyword").size(), size_t{2});
        XX_TEST_EXPECT_EQ(store.searchSessions("needle-keyword", 1).size(), size_t{1});
    }

    // ---- 展示历史序号与增量补拉 (计划 STO-4) ----
    {
        SessionStore store{root};
        const std::string seqSession = "s-seq";

        // 显式序号写入 (会话分配): 恢复时按 meta 的 viewSeqCounter 续编号
        store.appendViewMessage(seqSession, makeMsg("s1", "first"), 1, 10);
        store.appendViewMessage(seqSession, makeMsg("s2", "second"), 2, 11);
        store.appendViewMessage(seqSession, makeMsg("s3", "third"), 3, 12);

        auto loaded = store.loadSession(seqSession);
        XX_TEST_EXPECT_EQ(loaded.viewMessages.size(), size_t{3});
        XX_TEST_EXPECT_EQ(loaded.lastViewSeq, uint64_t{12});
        XX_TEST_EXPECT_EQ(loaded.msgIdCounter, uint64_t{3});

        // 增量补拉: 只回补序号更大的消息
        auto after10 = store.loadViewMessagesAfter(seqSession, 10);
        XX_TEST_EXPECT_EQ(after10.size(), size_t{2});
        if (after10.size() == 2) {
            XX_TEST_EXPECT_EQ(after10[0].seq, uint64_t{11});
            XX_TEST_EXPECT_EQ(after10[0].message.id, std::string{"s2"});
            XX_TEST_EXPECT_EQ(after10[0].message.text, std::string{"second"});
            XX_TEST_EXPECT_EQ(after10[1].seq, uint64_t{12});
        }
        XX_TEST_EXPECT_EQ(store.loadViewMessagesAfter(seqSession, 0).size(), size_t{3});
        XX_TEST_EXPECT_EQ(store.loadViewMessagesAfter(seqSession, 12).size(), size_t{0});
        // limit: 只取前 N 条 (调用方据此判断"差量过大, 回退全量同步")
        XX_TEST_EXPECT_EQ(store.loadViewMessagesAfter(seqSession, 10, 1).size(), size_t{1});
        // 不存在的会话不创建目录
        XX_TEST_EXPECT_EQ(store.loadViewMessagesAfter("s-seq-missing", 0).size(), size_t{0});
        XX_TEST_EXPECT_FALSE(fs::exists(fs::path(utilxx_base::utf8ToPath(root)) / "s-seq-missing"));

        // 更新已有消息不改动序号 (增量补拉不会重复下发已持有消息)
        auto updated = makeMsg("s3", "third (updated)");
        store.updateViewMessage(seqSession, updated);
        XX_TEST_EXPECT_EQ(store.loadViewMessagesAfter(seqSession, 11).size(), size_t{1});
        if (auto rows = store.loadViewMessagesAfter(seqSession, 11); rows.size() == 1) {
            XX_TEST_EXPECT_EQ(rows[0].seq, uint64_t{12});
            XX_TEST_EXPECT_EQ(rows[0].message.text, std::string{"third (updated)"});
        }

        // 老库 (无 viewSeqCounter 记录) 兜底: 取库内最大行序号
        {
            const auto dbFile = fs::path(utilxx_base::utf8ToPath(root))
                                / utilxx_base::utf8ToPath(
                                    SessionStore::sanitizeSessionId("s-seq-legacy")
                                )
                                / "session.db";
            fs::create_directories(dbFile.parent_path());
            agentxx::util::SqliteDb db;
            db.open(dbFile.string());
            db.exec(
                "CREATE TABLE view_message (seq INTEGER PRIMARY KEY AUTOINCREMENT, json TEXT NOT "
                "NULL, msg_id TEXT)"
            );
            db.exec("INSERT INTO view_message(json, msg_id) VALUES ('{\"id\":\"l1\",\"role\":"
                    "\"user\",\"text\":\"legacy one\"}', 'l1')");
            db.exec("INSERT INTO view_message(json, msg_id) VALUES ('{\"id\":\"l2\",\"role\":"
                    "\"user\",\"text\":\"legacy two\"}', 'l2')");
            db.close();

            auto legacy = store.loadSession("s-seq-legacy");
            XX_TEST_EXPECT_EQ(legacy.viewMessages.size(), size_t{2});
            XX_TEST_EXPECT_EQ(legacy.lastViewSeq, uint64_t{2});
        }
    }

    removeTempRoot(root);
    return TestResult{g_ss_passed, g_ss_failed};
}

} // namespace test
} // namespace agentxx
