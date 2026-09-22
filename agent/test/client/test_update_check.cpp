// 启动更新检查 (agentxx-client/update_check.h)
//
// 覆盖:
// - 版本标签解析: 前缀 v/V、缺位补零、预发布后缀、非法输入
// - 版本比较: 三段数字逐段比较
// - 设置项"启动时检查更新": 默认开、切换持久化 (TUI 侧开关)
// - 请求失败路径: 不可达端点返回 ok=false 且带原因 (不抛异常)
// - 本地 HTTP 服务端到端 (302 Location / JSON / 404) 见 `agentxx_test http`
#include "agentxx-test/client/test_update_check.h"

#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx-client/update_check.h"
#include "agentxx/util/settings_db.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_update_check_passed = 0;
int g_update_check_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_update_check_passed
#define XX_TEST_FAILED g_update_check_failed

namespace agentxx {
namespace test {

using namespace agentxx::client;

namespace {

/// 同步跑一次更新检查 (测试用: 独立 io_context, 跑完即返回)
std::optional<UpdateCheckResult> runCheckSync(std::string_view url) {
    asio::io_context                 io;
    std::optional<UpdateCheckResult> out;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            out = co_await agentxx::client::checkLatestRelease(url);
            co_return;
        },
        asio::detached
    );
    io.run();
    return out;
}

void test_parse_version_tag() {
    // 常见形态: 前缀 v/V、两段/三段、预发布后缀、首尾空白
    auto v123 = parseVersionTag("v1.2.3");
    XX_TEST_EXPECT_TRUE(v123.has_value());
    if (v123) {
        XX_TEST_EXPECT_EQ((*v123)[0], 1);
        XX_TEST_EXPECT_EQ((*v123)[1], 2);
        XX_TEST_EXPECT_EQ((*v123)[2], 3);
    }

    auto plain = parseVersionTag("1.2.3");
    XX_TEST_EXPECT_TRUE(plain.has_value());
    XX_TEST_EXPECT_EQ((*plain)[0], 1);

    auto upper = parseVersionTag("V10.20.30");
    XX_TEST_EXPECT_TRUE(upper.has_value());
    if (upper) {
        XX_TEST_EXPECT_EQ((*upper)[0], 10);
        XX_TEST_EXPECT_EQ((*upper)[1], 20);
        XX_TEST_EXPECT_EQ((*upper)[2], 30);
    }

    // 缺位补零: "1.2" -> (1,2,0); "1" -> (1,0,0)
    auto twoPart = parseVersionTag("v1.2");
    XX_TEST_EXPECT_TRUE(twoPart.has_value());
    if (twoPart) {
        XX_TEST_EXPECT_EQ((*twoPart)[0], 1);
        XX_TEST_EXPECT_EQ((*twoPart)[1], 2);
        XX_TEST_EXPECT_EQ((*twoPart)[2], 0);
    }
    auto onePart = parseVersionTag("1");
    XX_TEST_EXPECT_TRUE(onePart.has_value());
    if (onePart) {
        XX_TEST_EXPECT_EQ((*onePart)[0], 1);
        XX_TEST_EXPECT_EQ((*onePart)[1], 0);
        XX_TEST_EXPECT_EQ((*onePart)[2], 0);
    }

    // 预发布/构建后缀不参与比较
    auto pre = parseVersionTag("v1.2.3-rc1");
    XX_TEST_EXPECT_TRUE(pre.has_value());
    if (pre) {
        XX_TEST_EXPECT_EQ((*pre)[2], 3);
    }

    // 首尾空白 (HTTP 头值可能带 \r\n)
    auto padded = parseVersionTag(" v1.0.0\r\n");
    XX_TEST_EXPECT_TRUE(padded.has_value());
    if (padded) {
        XX_TEST_EXPECT_EQ((*padded)[0], 1);
    }

    // 非法输入
    XX_TEST_EXPECT_FALSE(parseVersionTag("").has_value());
    XX_TEST_EXPECT_FALSE(parseVersionTag("v").has_value());
    XX_TEST_EXPECT_FALSE(parseVersionTag("latest").has_value());
    XX_TEST_EXPECT_FALSE(parseVersionTag("v1.x.2").has_value());
    XX_TEST_EXPECT_FALSE(parseVersionTag("1..2").has_value());
    // 多于三段: 视为不合法 (避免 "1.2.3.4" 被截断成 1.2.3 造成误判)
    XX_TEST_EXPECT_FALSE(parseVersionTag("1.2.3.4").has_value());
    // 负数段
    XX_TEST_EXPECT_FALSE(parseVersionTag("v-1.0.0").has_value());
}

void test_compare_version() {
    XX_TEST_EXPECT_EQ(compareVersion(VersionTriple{1, 0, 0}, VersionTriple{1, 0, 0}), 0);
    XX_TEST_EXPECT_TRUE(compareVersion(VersionTriple{1, 0, 0}, VersionTriple{0, 9, 9}) > 0);
    XX_TEST_EXPECT_TRUE(compareVersion(VersionTriple{0, 9, 9}, VersionTriple{1, 0, 0}) < 0);
    XX_TEST_EXPECT_TRUE(compareVersion(VersionTriple{1, 10, 0}, VersionTriple{1, 9, 0}) > 0);
    XX_TEST_EXPECT_TRUE(compareVersion(VersionTriple{1, 0, 10}, VersionTriple{1, 0, 9}) > 0);
    XX_TEST_EXPECT_TRUE(compareVersion(VersionTriple{0, 1, 1}, VersionTriple{0, 1, 0}) > 0);
}

void test_settings_check_update_toggle() {
    auto& settings = TUISettings::instance();

    // 默认值为开 (设置项"启动时检查更新")
    XX_TEST_EXPECT_TRUE(TUISettings::kDefaultCheckUpdateOnStartup);

    const bool original = settings.checkUpdateOnStartup();
    settings.setCheckUpdateOnStartup(false);
    XX_TEST_EXPECT_FALSE(settings.checkUpdateOnStartup());
    settings.setCheckUpdateOnStartup(true);
    XX_TEST_EXPECT_TRUE(settings.checkUpdateOnStartup());
    // 复位 (本模块与 tui_settings 模块共用单例; 落库口径见该模块 test_persist_to_db)
    settings.setCheckUpdateOnStartup(original);
}

void test_check_update_storage_key() {
    // 持久化键 `tui.checkUpdateOnStartup` 的取值口径: 1 = 开, 0 = 关,
    // 键缺失时按默认值 (开) 处理 (见 TUISettings::attachDb)
    auto root = std::filesystem::temp_directory_path()
                / ("agentxx_update_check_test_"
                   + std::to_string(
                       std::chrono::steady_clock::now().time_since_epoch().count()
                   ));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    XX_TEST_EXPECT_FALSE(ec);

    auto dbPath = (root / "global.db").string();
    {
        agentxx::util::SettingsDb db(dbPath);
        XX_TEST_EXPECT_TRUE(db.setInt64("tui.checkUpdateOnStartup", 0));
        XX_TEST_EXPECT_EQ(db.getInt64("tui.checkUpdateOnStartup", -1), int64_t{0});
        XX_TEST_EXPECT_TRUE(db.setInt64("tui.checkUpdateOnStartup", 1));
        XX_TEST_EXPECT_EQ(db.getInt64("tui.checkUpdateOnStartup", -1), int64_t{1});
        // 未写入的键返回调用方默认值 (-1 = 未设置 -> 保持默认开启)
        agentxx::util::SettingsDb fresh(dbPath);
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.notExistKey", -1), int64_t{-1});
    }
    std::filesystem::remove_all(root, ec);
}

void test_check_failure_returns_error() {
    // 不可达端点 (保留端口): 必须返回 ok=false 且带原因, 而不是抛异常
    // (更新检查是附加提示, 无网络时不得影响启动)
    auto res = runCheckSync("http://127.0.0.1:1/releases/latest");
    XX_TEST_EXPECT_TRUE(res.has_value());
    if (res.has_value()) {
        XX_TEST_EXPECT_FALSE(res->ok);
        XX_TEST_EXPECT_FALSE(res->error.empty());
    }

    // 非法 URL: 同样以错误结果返回
    auto bad = runCheckSync("not-a-url");
    XX_TEST_EXPECT_TRUE(bad.has_value());
    if (bad.has_value()) {
        XX_TEST_EXPECT_FALSE(bad->ok);
        XX_TEST_EXPECT_FALSE(bad->error.empty());
    }
}

} // namespace

TestResult testUpdateCheck() {
    g_update_check_passed = 0;
    g_update_check_failed = 0;

    test_parse_version_tag();
    test_compare_version();
    test_settings_check_update_toggle();
    test_check_update_storage_key();
    test_check_failure_returns_error();

    return TestResult{g_update_check_passed, g_update_check_failed};
}

} // namespace test
} // namespace agentxx
