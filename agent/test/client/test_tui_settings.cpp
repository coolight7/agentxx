#include "test_tui_settings.h"

#include "agentxx-client/io/tui/framework/tui_settings.h"
#include <string_view>
#include <thread>
#include <vector>

namespace agentxx {
namespace test {

int g_tui_settings_passed = 0;
int g_tui_settings_failed = 0;

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

TestResult testTuiSettings() {
    g_tui_settings_passed = 0;
    g_tui_settings_failed = 0;

    test_singleton();
    test_animation_level_set_get();
    test_is_animation_enabled();
    test_level_names_table();
    test_show_system_info();
    test_concurrent_access();

    return TestResult{g_tui_settings_passed, g_tui_settings_failed};
}

} // namespace test
} // namespace agentxx
