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
    // E) 用量账本: 追加、聚合、最近若干条
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
    // 账本落库: 重新打开仍能聚合 (账本是持久事实, 不依赖内存)
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

    removeTempRoot(root);
    return TestResult{g_ss_passed, g_ss_failed};
}

} // namespace test
} // namespace agentxx
