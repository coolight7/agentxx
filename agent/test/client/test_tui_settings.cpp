#include "test_tui_settings.h"

#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx/util/settings_db.h"
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace agentxx {
namespace test {

int g_tui_settings_passed = 0;
int g_tui_settings_failed = 0;

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// TUISettings 全局设置存储
// ---------------------------------------------------------------------------

void test_singleton() {
    // 全局单例: 多次获取同一实例
    XX_TEST_EXPECT_TRUE(&TUISettings::instance() == &TUISettings::instance());
}

void test_animation_level_set_get() {
    auto& settings = TUISettings::instance();

    // 逐一设置各等级并读回
    const struct {
        AnimationLevel   level;
        std::string_view name;
    } cases[] = {
        {AnimationLevel::Disabled, "Disabled"},
        {AnimationLevel::Low,      "Low"     },
        {AnimationLevel::Medium,   "Medium"  },
        {AnimationLevel::High,     "High"    },
        {AnimationLevel::Ultra,    "Ultra"   },
    };

    for (const auto& c : cases) {
        settings.setAnimationLevel(c.level);
        XX_TEST_EXPECT_TRUE(settings.animationLevel() == c.level);
        XX_TEST_EXPECT_EQ(settings.animationLevelName(), c.name);
    }
}

void test_is_animation_enabled() {
    auto& settings = TUISettings::instance();

    // 等级数值从低到高排列, 便于循环遍历比较
    const AnimationLevel levels[] = {
        AnimationLevel::Disabled,
        AnimationLevel::Low,
        AnimationLevel::Medium,
        AnimationLevel::High,
        AnimationLevel::Ultra,
    };

    // 非 Disabled: 设置项等级 >= required 时返回 true (相等或更高启用)
    for (const auto setting : levels) {
        settings.setAnimationLevel(setting);
        for (const auto required : levels) {
            const bool expected = setting == AnimationLevel::Disabled
                                      ? false
                                      : static_cast<int>(setting) >= static_cast<int>(required);
            XX_TEST_EXPECT_TRUE(settings.isAnimationEnabled(required) == expected);
        }
    }

    // 典型场景: Disabled 禁用全部动画
    settings.setAnimationLevel(AnimationLevel::Disabled);
    XX_TEST_EXPECT_FALSE(settings.isAnimationEnabled(AnimationLevel::Low));
    XX_TEST_EXPECT_FALSE(settings.isAnimationEnabled(AnimationLevel::Medium));
    XX_TEST_EXPECT_FALSE(settings.isAnimationEnabled(AnimationLevel::High));
    XX_TEST_EXPECT_FALSE(settings.isAnimationEnabled(AnimationLevel::Ultra));
    XX_TEST_EXPECT_FALSE(settings.isAnimationEnabled(AnimationLevel::Disabled));

    settings.setAnimationLevel(AnimationLevel::Medium);
    XX_TEST_EXPECT_TRUE(settings.isAnimationEnabled(AnimationLevel::Disabled));
    XX_TEST_EXPECT_TRUE(settings.isAnimationEnabled(AnimationLevel::Low));
    XX_TEST_EXPECT_TRUE(settings.isAnimationEnabled(AnimationLevel::Medium)); // 相等
    XX_TEST_EXPECT_FALSE(settings.isAnimationEnabled(AnimationLevel::High));  // 更高
    XX_TEST_EXPECT_FALSE(settings.isAnimationEnabled(AnimationLevel::Ultra)); // 更高

    settings.setAnimationLevel(AnimationLevel::Ultra);
    XX_TEST_EXPECT_TRUE(settings.isAnimationEnabled(AnimationLevel::Ultra));
}

void test_level_names_table() {
    // 名称表覆盖全部等级
    XX_TEST_EXPECT_EQ(TUISettings::kAnimationLevelNames.size(), (size_t)5);
    XX_TEST_EXPECT_EQ(TUISettings::kAnimationLevelNames[0], std::string_view("Disabled"));
    XX_TEST_EXPECT_EQ(TUISettings::kAnimationLevelNames[1], std::string_view("Low"));
    XX_TEST_EXPECT_EQ(TUISettings::kAnimationLevelNames[2], std::string_view("Medium"));
    XX_TEST_EXPECT_EQ(TUISettings::kAnimationLevelNames[3], std::string_view("High"));
    XX_TEST_EXPECT_EQ(TUISettings::kAnimationLevelNames[4], std::string_view("Ultra"));
}

void test_show_system_info() {
    auto& settings = TUISettings::instance();

    // 默认开启
    settings.setShowSystemInfo(true);
    XX_TEST_EXPECT_TRUE(settings.showSystemInfo());

    // 切换关闭/开启
    settings.setShowSystemInfo(false);
    XX_TEST_EXPECT_FALSE(settings.showSystemInfo());
    settings.setShowSystemInfo(true);
    XX_TEST_EXPECT_TRUE(settings.showSystemInfo());

    // 内部引用与 getter 一致 (供 TUICtx 指针访问)
    settings.setShowSystemInfo(false);
    XX_TEST_EXPECT_FALSE(settings.showSystemInfoRef().load());
    settings.setShowSystemInfo(true);
    XX_TEST_EXPECT_TRUE(settings.showSystemInfoRef().load());
}

void test_concurrent_access() {
    // 多线程并发读写不应崩溃, 且读到的等级值始终合法
    auto&         settings    = TUISettings::instance();
    constexpr int kIterations = 2000;

    std::atomic<bool> stop{false};
    std::thread       writer([&] {
        for (int i = 0; i < kIterations && !stop.load(); ++i) {
            settings.setAnimationLevel(static_cast<AnimationLevel>(i % 5));
            settings.setShowSystemInfo(i % 2 == 0);
        }
    });

    std::vector<std::thread> readers;
    std::atomic<int>         invalidCount{0};
    for (int t = 0; t < 2; ++t) {
        readers.emplace_back([&] {
            for (int i = 0; i < kIterations && !stop.load(); ++i) {
                const int lv = static_cast<int>(settings.animationLevel());
                if (lv < 0 || lv > 4) {
                    invalidCount.fetch_add(1);
                }
                (void)settings.isAnimationEnabled(AnimationLevel::Medium);
                (void)settings.showSystemInfo();
            }
        });
    }

    writer.join();
    for (auto& r : readers) {
        r.join();
    }
    XX_TEST_EXPECT_EQ(invalidCount.load(), 0);

    // 恢复默认, 避免影响其他用例
    settings.setAnimationLevel(TUISettings::kDefaultAnimationLevel);
    settings.setShowSystemInfo(true);
}

void test_persist_to_db() {
    // 绑定全局设置数据库后, 设置变更同步落库 (验证写入 global.db 文件)
    auto& settings = TUISettings::instance();

    auto root = fs::temp_directory_path()
                / fmt::format(
                    "agentxx_tui_settings_test_{}",
                    std::chrono::steady_clock::now().time_since_epoch().count()
                );
    auto dbPath = (root / "global.db").string();

    auto db = std::make_shared<agentxx::util::SettingsDb>(dbPath);
    settings.attachDb(db);

    // 写入设置 → 直接读库文件校验持久化 (绕过单例, 模拟重启后的新进程)
    settings.setThemeKind(TUISettings::kThemeLight);
    settings.setAnimationLevel(AnimationLevel::Low);
    settings.setShowSystemInfo(false);
    {
        auto fresh = agentxx::util::SettingsDb(dbPath);
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.theme", -1), int64_t{TUISettings::kThemeLight});
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.animationLevel", -1), int64_t{1}); // Low
        XX_TEST_EXPECT_FALSE(fresh.getBool("tui.showSystemInfo", true));
    }

    // 再次变更 → 库文件同步更新
    settings.setThemeKind(TUISettings::kThemeDark);
    settings.setAnimationLevel(AnimationLevel::Ultra);
    settings.setShowSystemInfo(true);
    {
        auto fresh = agentxx::util::SettingsDb(dbPath);
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.theme", -1), int64_t{TUISettings::kThemeDark});
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.animationLevel", -1), int64_t{4}); // Ultra
        XX_TEST_EXPECT_TRUE(fresh.getBool("tui.showSystemInfo", false));
    }

    // 恢复默认, 避免影响其他用例
    settings.setAnimationLevel(TUISettings::kDefaultAnimationLevel);
    settings.setShowSystemInfo(true);

    // 注意: TUISettings 单例持有 db 连接 (进程生命周期), Windows 上无法删除
    // 被占用文件, 故清理失败时忽略 (仅临时目录残留, 不影响测试结果)
    std::error_code ec;
    fs::remove_all(root, ec);
}

TestResult testTuiSettings() {
    g_tui_settings_passed = 0;
    g_tui_settings_failed = 0;

    test_singleton();
    test_animation_level_set_get();
    test_is_animation_enabled();
    test_level_names_table();
    test_show_system_info();
    test_concurrent_access();
    test_persist_to_db();

    return TestResult{g_tui_settings_passed, g_tui_settings_failed};
}

} // namespace test
} // namespace agentxx
