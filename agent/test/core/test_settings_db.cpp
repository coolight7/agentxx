#include "agentxx-test/core/test_settings_db.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/util/settings_db.h"
#include "agentxx/util/sqlite.h"
#include <chrono>
#include <filesystem>
#include <fmt/format.h>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_sdb_passed = 0;
int g_sdb_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_sdb_passed
#define XX_TEST_FAILED g_sdb_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

/// 创建唯一临时目录 (测试根目录), 返回路径; 由调用方在测试结束 remove_all
std::string makeTempRoot() {
    auto dir = fs::temp_directory_path()
               / fmt::format(
                   "agentxx_sdb_test_{}",
                   std::chrono::steady_clock::now().time_since_epoch().count()
               );
    fs::create_directories(dir);
    return dir.string();
}

} // namespace

// ---------------------------------------------------------------------------
// SettingsDb 单测: KV 读写/覆盖/类型存取
// ---------------------------------------------------------------------------

static TestResult testKvRoundtrip() {
    using agentxx::util::SettingsDb;

    auto root = makeTempRoot();
    auto path = (fs::path(root) / "global.db").string();
    {
        auto db = std::make_shared<SettingsDb>(path);

        // 不存在返回 nullopt
        XX_TEST_EXPECT_FALSE(db->get("not-exist").has_value());

        // 写入 + 读回
        XX_TEST_EXPECT_TRUE(db->set("theme", "dark"));
        XX_TEST_EXPECT_TRUE(db->set("model", "gpt-4o"));
        auto v1 = db->get("theme");
        XX_TEST_EXPECT_HAS_VALUE(v1);
        if (v1) {
            XX_TEST_EXPECT_EQ(*v1, std::string{"dark"});
        }

        // 覆盖 (主键冲突)
        XX_TEST_EXPECT_TRUE(db->set("theme", "light"));
        auto v2 = db->get("theme");
        XX_TEST_EXPECT_HAS_VALUE(v2);
        if (v2) {
            XX_TEST_EXPECT_EQ(*v2, std::string{"light"});
        }

        // 空值/空键可存取 (不崩溃)
        XX_TEST_EXPECT_TRUE(db->set("", "empty-key"));
        auto v3 = db->get("");
        XX_TEST_EXPECT_HAS_VALUE(v3);
        if (v3) {
            XX_TEST_EXPECT_EQ(*v3, std::string{"empty-key"});
        }
        XX_TEST_EXPECT_TRUE(db->set("empty-val", ""));
        auto v4 = db->get("empty-val");
        XX_TEST_EXPECT_HAS_VALUE(v4);
        if (v4) {
            XX_TEST_EXPECT_EQ(*v4, std::string{});
        }

        // 文件已创建
        XX_TEST_EXPECT_TRUE(fs::exists(path));

        // 模拟重启: 新实例读同一文件
        auto db2 = std::make_shared<SettingsDb>(path);
        auto v5  = db2->get("model");
        XX_TEST_EXPECT_HAS_VALUE(v5);
        if (v5) {
            XX_TEST_EXPECT_EQ(*v5, std::string{"gpt-4o"});
        }
        XX_TEST_EXPECT_EQ(db2->get("theme").value_or("?"), std::string{"light"});
    }
    fs::remove_all(root);
    return TestResult{};
}

static TestResult testTypedAccess() {
    using agentxx::util::SettingsDb;

    auto root = makeTempRoot();
    auto path = (fs::path(root) / "global.db").string();
    {
        auto db = std::make_shared<SettingsDb>(path);

        // 整数: 默认值 / 读写
        XX_TEST_EXPECT_EQ(db->getInt64("anim", 42), int64_t{42}); // 不存在用默认
        XX_TEST_EXPECT_TRUE(db->setInt64("anim", 3));
        XX_TEST_EXPECT_EQ(db->getInt64("anim", 42), int64_t{3});
        // 负数
        XX_TEST_EXPECT_TRUE(db->setInt64("neg", -7));
        XX_TEST_EXPECT_EQ(db->getInt64("neg", 0), int64_t{-7});

        // 布尔: 默认值 / 读写
        XX_TEST_EXPECT_TRUE(db->getBool("sys", true)); // 不存在用默认
        XX_TEST_EXPECT_TRUE(db->setBool("sys", false));
        XX_TEST_EXPECT_FALSE(db->getBool("sys", true));
        XX_TEST_EXPECT_TRUE(db->setBool("sys", true));
        XX_TEST_EXPECT_TRUE(db->getBool("sys", false));

        // 非法数值文本 → 默认值 (不崩溃)
        XX_TEST_EXPECT_TRUE(db->set("bad", "not-a-number"));
        XX_TEST_EXPECT_EQ(db->getInt64("bad", 99), int64_t{99});
        XX_TEST_EXPECT_FALSE(db->getBool("bad", true)); // 已存在但非 "1" 视为 false

        // 模拟重启后类型值仍可读
        auto db2 = std::make_shared<SettingsDb>(path);
        XX_TEST_EXPECT_EQ(db2->getInt64("anim", 0), int64_t{3});
        XX_TEST_EXPECT_TRUE(db2->getBool("sys", false));
    }
    fs::remove_all(root);
    return TestResult{};
}

/// 版本与乐观写入 (计划 STO-11):
/// 无条件写入递增版本、按期望版本提交、版本不匹配返回冲突且不改内容、
/// 两个句柄竞争时不丢更新的语义、重启后版本延续、老库(无 version 列)迁移
static TestResult testVersionedWrite() {
    using agentxx::util::SettingsDb;
    using agentxx::util::SqliteDb;

    auto root = makeTempRoot();
    auto path = (fs::path(root) / "global.db").string();
    {
        auto db = std::make_shared<SettingsDb>(path);

        // 条目不存时版本视为 0
        XX_TEST_EXPECT_EQ(db->version("k"), int64_t{0});
        int64_t v = -1;
        XX_TEST_EXPECT_FALSE(db->getVersioned("k", &v).has_value());
        XX_TEST_EXPECT_EQ(v, int64_t{0});

        // 无条件写入: 版本自增
        XX_TEST_EXPECT_TRUE(db->set("k", "v1"));
        XX_TEST_EXPECT_EQ(db->version("k"), int64_t{1});
        v = 0;
        auto val1 = db->getVersioned("k", &v);
        XX_TEST_EXPECT_HAS_VALUE(val1);
        XX_TEST_EXPECT_EQ(v, int64_t{1});
        XX_TEST_EXPECT_TRUE(db->set("k", "v2"));
        XX_TEST_EXPECT_EQ(db->version("k"), int64_t{2});

        // 乐观写入: 期望版本不匹配 → 冲突, 库中内容与版本都不变
        auto conflict = db->setVersioned("k", "v3", 1);
        XX_TEST_EXPECT_TRUE(conflict.status == SettingsDb::WriteStatus::Conflict);
        XX_TEST_EXPECT_EQ(conflict.version, int64_t{2});
        auto cur = db->get("k");
        XX_TEST_EXPECT_HAS_VALUE(cur);
        if (cur) {
            XX_TEST_EXPECT_EQ(*cur, std::string{"v2"});
        }
        XX_TEST_EXPECT_EQ(db->version("k"), int64_t{2});

        // 期望版本匹配 → 写入成功, 版本加一
        auto ok1 = db->setVersioned("k", "v3", 2);
        XX_TEST_EXPECT_TRUE(ok1.status == SettingsDb::WriteStatus::Ok);
        XX_TEST_EXPECT_EQ(ok1.version, int64_t{3});

        // 新建条目: 期望版本 0
        auto okNew = db->setVersioned("brand-new", "first", 0);
        XX_TEST_EXPECT_TRUE(okNew.status == SettingsDb::WriteStatus::Ok);
        XX_TEST_EXPECT_EQ(okNew.version, int64_t{1});
        auto fresh = db->get("brand-new");
        XX_TEST_EXPECT_HAS_VALUE(fresh);
        if (fresh) {
            XX_TEST_EXPECT_EQ(*fresh, std::string{"first"});
        }

        // 两个写入者竞争: A 读到版本后 B 先提交, A 再提交得到冲突 (不静默覆盖 B)
        auto    dbB = std::make_shared<SettingsDb>(path);
        int64_t vA  = 0;
        auto    valA = db->getVersioned("k", &vA);
        XX_TEST_EXPECT_HAS_VALUE(valA);
        XX_TEST_EXPECT_EQ(vA, int64_t{3});
        XX_TEST_EXPECT_TRUE(dbB->set("k", "from B"));
        auto lostUpdate = db->setVersioned("k", "from A", vA);
        XX_TEST_EXPECT_TRUE(lostUpdate.status == SettingsDb::WriteStatus::Conflict);
        XX_TEST_EXPECT_EQ(lostUpdate.version, int64_t{4});
        auto afterB = dbB->get("k");
        XX_TEST_EXPECT_HAS_VALUE(afterB);
        if (afterB) {
            XX_TEST_EXPECT_EQ(*afterB, std::string{"from B"});
        }
    }
    // 重启后版本延续 (不是从 0 重新计数)
    {
        auto db = std::make_shared<SettingsDb>(path);
        XX_TEST_EXPECT_EQ(db->version("k"), int64_t{4});
    }
    // 老库迁移: 只有 key/value 两列的库补 version 列, 已有数据版本从 0 起算
    {
        auto legacy = (fs::path(root) / "legacy.db").string();
        {
            SqliteDb raw;
            raw.open(legacy);
            raw.exec("CREATE TABLE setting (key TEXT PRIMARY KEY, value TEXT NOT NULL)");
            raw.exec("INSERT INTO setting(key, value) VALUES('tui.theme', '1')");
            raw.close();
        }
        auto db = std::make_shared<SettingsDb>(legacy);
        XX_TEST_EXPECT_EQ(db->getInt64("tui.theme", -1), int64_t{1});
        XX_TEST_EXPECT_EQ(db->version("tui.theme"), int64_t{0});
        XX_TEST_EXPECT_TRUE(db->set("tui.theme", "0"));
        XX_TEST_EXPECT_EQ(db->version("tui.theme"), int64_t{1});
        XX_TEST_EXPECT_EQ(db->getInt64("tui.theme", -1), int64_t{0});
    }
    fs::remove_all(root);
    return TestResult{};
}

static TestResult testDefaultPath() {
    // 默认路径: {defaultDataDir}/sqlite/global.db
    using agentxx::agent::AgentConfigStatic;
    using agentxx::util::SettingsDb;

    auto db   = std::make_shared<SettingsDb>();
    auto path = fs::path(db->dbPath()).lexically_normal().string();
    auto expect
        = fs::path(AgentConfigStatic::getGlobalSettingsDbPath("")).lexically_normal().string();
    XX_TEST_EXPECT_EQ(path, expect);

    // 显式 dataDir 时: {dataDir}/sqlite/global.db
    auto db2
        = std::make_shared<SettingsDb>(AgentConfigStatic::getGlobalSettingsDbPath("/tmp/custom-data"
        ));
    XX_TEST_EXPECT_TRUE(db2->dbPath().find("global.db") != std::string::npos);

    // data_dir: default 关键字与系统数据目录 (平台惯例)
    // - Windows: %APPDATA%/agentxx/, 其他: ~/.agentxx/
    XX_TEST_EXPECT_EQ(AgentConfigStatic::kDefaultDataDirKey, std::string_view("default"));
    auto sysDir = AgentConfigStatic::systemDataDir();
    XX_TEST_EXPECT_FALSE(sysDir.empty());
    XX_TEST_EXPECT_TRUE(sysDir.find("agentxx") != std::string::npos);
    // 系统数据目录派生路径: {sysDir}/sqlite/global.db
    auto sysDbPath = AgentConfigStatic::getGlobalSettingsDbPath(sysDir);
    XX_TEST_EXPECT_TRUE(sysDbPath.find("agentxx") != std::string::npos);
    XX_TEST_EXPECT_TRUE(sysDbPath.find("global.db") != std::string::npos);
    return TestResult{};
}

TestResult testSettingsDb() {
    g_sdb_passed = 0;
    g_sdb_failed = 0;

    testKvRoundtrip();
    testTypedAccess();
    testVersionedWrite();
    testDefaultPath();

    return TestResult{g_sdb_passed, g_sdb_failed};
}

} // namespace test
} // namespace agentxx
