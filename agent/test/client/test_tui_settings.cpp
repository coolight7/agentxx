#include "test_tui_settings.h"

#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx/util/env.h"
#include "agentxx/util/settings_db.h"
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <memory>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tui_settings_passed = 0;
int g_tui_settings_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tui_settings_passed
#define XX_TEST_FAILED g_tui_settings_failed

namespace agentxx {
namespace test {

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

void test_log_level_set_get() {
    auto& settings = TUISettings::instance();

    // 逐一设置各等级并读回 (名称表与 LogLevel 枚举值一一对应)
    const struct {
        agentxx::util::LogLevel level;
        std::string_view        name;
    } cases[] = {
        {agentxx::util::LogLevel::Trace, "Trace"},
        {agentxx::util::LogLevel::Debug, "Debug"},
        {agentxx::util::LogLevel::Info,  "Info" },
        {agentxx::util::LogLevel::Warn,  "Warn" },
        {agentxx::util::LogLevel::Error, "Error"},
        {agentxx::util::LogLevel::Out,   "Out"  },
    };

    for (const auto& c : cases) {
        settings.setLogLevel(c.level);
        XX_TEST_EXPECT_TRUE(settings.logLevel() == c.level);
        XX_TEST_EXPECT_EQ(settings.logLevelName(), c.name);
    }

    // 恢复默认, 避免影响其他用例
    settings.setLogLevel(TUISettings::kDefaultLogLevel);
}

void test_log_level_names_table() {
    // 名称表覆盖全部等级 (与 LogLevel 枚举值顺序一致)
    XX_TEST_EXPECT_EQ(TUISettings::kLogLevelNames.size(), (size_t)6);
    XX_TEST_EXPECT_EQ(TUISettings::kLogLevelNames[0], std::string_view("Trace"));
    XX_TEST_EXPECT_EQ(TUISettings::kLogLevelNames[1], std::string_view("Debug"));
    XX_TEST_EXPECT_EQ(TUISettings::kLogLevelNames[2], std::string_view("Info"));
    XX_TEST_EXPECT_EQ(TUISettings::kLogLevelNames[3], std::string_view("Warn"));
    XX_TEST_EXPECT_EQ(TUISettings::kLogLevelNames[4], std::string_view("Error"));
    XX_TEST_EXPECT_EQ(TUISettings::kLogLevelNames[5], std::string_view("Out"));
}

void test_language_names_table() {
    XX_TEST_EXPECT_EQ(TUISettings::kLanguageNames.size(), (size_t)3);
    XX_TEST_EXPECT_EQ(TUISettings::kLanguageNames[0], std::string_view("自动 (Auto)"));
    XX_TEST_EXPECT_EQ(TUISettings::kLanguageNames[1], std::string_view("简体中文 (zh-cn)"));
    XX_TEST_EXPECT_EQ(TUISettings::kLanguageNames[2], std::string_view("English (en-us)"));
}

void test_match_supported_language() {
    // 中文环境格式
    const char* zhCases[] = {
        "zh-CN",
        "zh_CN",
        "zh_CN.UTF-8",
        "zh_CN.GBK",
        "zh-Hans-CN",
        "zh-Hans",
        "zh-Hant-TW",
        "zh_TW",
        "zh_HK.UTF-8",
        "zh-SG",
        "zh",
        "chinese",
        "Chinese (Simplified)_China.936",
        "  zh-cn  ",
        "zh_CN.UTF-8@pinyin",
    };
    for (const char* loc : zhCases) {
        XX_TEST_EXPECT_TRUE(matchSupportedLanguage(loc) == TuiLanguage::ZhCn);
    }

    // 英文环境格式
    const char* enCases[] = {
        "en-US",
        "en_US",
        "en_US.UTF-8",
        "en-GB",
        "en_GB.UTF-8",
        "en-CA",
        "en-AU",
        "en",
        "english",
        "English_United States.1252",
        "  en-us  ",
    };
    for (const char* loc : enCases) {
        XX_TEST_EXPECT_TRUE(matchSupportedLanguage(loc) == TuiLanguage::EnUs);
    }

    // 其它非中文系统环境 (在已支持列表 [ZhCn, EnUs] 中回退为国际通用语言 EnUs)
    const char* otherCases[] = {
        "ja_JP.UTF-8",
        "ja-JP",
        "ko_KR.UTF-8",
        "ko-KR",
        "fr_FR.UTF-8",
        "fr-FR",
        "de_DE.UTF-8",
        "de-DE",
        "es_ES.UTF-8",
        "ru_RU.UTF-8",
        "it_IT.UTF-8",
    };
    for (const char* loc : otherCases) {
        XX_TEST_EXPECT_TRUE(matchSupportedLanguage(loc) == TuiLanguage::EnUs);
    }

    // 无法识别/空环境/C/POSIX -> 回退默认中文 ZhCn
    const char* fallbackCases[] = {
        "",
        "   ",
        "C",
        "POSIX",
        "c",
        "posix",
        "C.UTF-8",
        "unknown-custom-12345",
    };
    for (const char* loc : fallbackCases) {
        XX_TEST_EXPECT_TRUE(matchSupportedLanguage(loc) == TuiLanguage::ZhCn);
    }

    // 多语言优先级列表 (LANGUAGE 语法)
    XX_TEST_EXPECT_TRUE(matchSupportedLanguage("zh_CN:en_US") == TuiLanguage::ZhCn);
    XX_TEST_EXPECT_TRUE(matchSupportedLanguage("en_US:zh_CN") == TuiLanguage::EnUs);
    XX_TEST_EXPECT_TRUE(matchSupportedLanguage("fr_FR:zh_CN") == TuiLanguage::EnUs);
    XX_TEST_EXPECT_TRUE(matchSupportedLanguage("C:zh_CN") == TuiLanguage::ZhCn);
}

void test_auto_language_detection() {
    auto& settings = TUISettings::instance();
    auto& env      = agentxx::util::ApplicationEnv::instance();
    auto& i18n     = TuiI18n::instance();

    // 设为自动模式
    settings.setLanguage(TuiLanguage::Auto);
    XX_TEST_EXPECT_TRUE(settings.language() == TuiLanguage::Auto);
    XX_TEST_EXPECT_EQ(settings.languageName(), std::string_view("自动 (Auto)"));

    // 模拟英文系统环境
    env.set("LANG", std::string_view{"en_US.UTF-8"});
    settings.refreshAutoLanguage();
    XX_TEST_EXPECT_TRUE(settings.effectiveLanguage() == TuiLanguage::EnUs);
    XX_TEST_EXPECT_EQ(settings.languageCode(), std::string("en"));
    XX_TEST_EXPECT_EQ(i18n.t("settings.title"), std::string_view(" Settings "));

    // 模拟中文系统环境
    env.set("LANG", std::string_view{"zh_CN.UTF-8"});
    settings.refreshAutoLanguage();
    XX_TEST_EXPECT_TRUE(settings.effectiveLanguage() == TuiLanguage::ZhCn);
    XX_TEST_EXPECT_EQ(settings.languageCode(), std::string("zh-cn"));
    XX_TEST_EXPECT_EQ(i18n.t("settings.title"), std::string_view(" 设置 "));

    // 模拟其它语言系统环境 (回退 EnUs)
    env.set("LANG", std::string_view{"ja_JP.UTF-8"});
    settings.refreshAutoLanguage();
    XX_TEST_EXPECT_TRUE(settings.effectiveLanguage() == TuiLanguage::EnUs);
    XX_TEST_EXPECT_EQ(settings.languageCode(), std::string("en"));
    XX_TEST_EXPECT_EQ(i18n.t("settings.title"), std::string_view(" Settings "));

    // 当用户显式指定语言时, effectiveLanguage 忽略系统环境
    settings.setLanguage(TuiLanguage::ZhCn);
    XX_TEST_EXPECT_TRUE(settings.effectiveLanguage() == TuiLanguage::ZhCn);
    XX_TEST_EXPECT_EQ(settings.languageCode(), std::string("zh-cn"));
    XX_TEST_EXPECT_EQ(i18n.t("settings.title"), std::string_view(" 设置 "));

    settings.setLanguage(TuiLanguage::EnUs);
    env.set("LANG", std::string_view{"zh_CN.UTF-8"});
    XX_TEST_EXPECT_TRUE(settings.effectiveLanguage() == TuiLanguage::EnUs);
    XX_TEST_EXPECT_EQ(settings.languageCode(), std::string("en"));
    XX_TEST_EXPECT_EQ(i18n.t("settings.title"), std::string_view(" Settings "));

    // 按语言代码切换: 传入 auto 时设置为 Auto, 但 languageCode 始终返回具体语言代码
    settings.setLanguageByCode("auto");
    XX_TEST_EXPECT_TRUE(settings.language() == TuiLanguage::Auto);
    XX_TEST_EXPECT_TRUE(settings.languageCode() == "zh-cn" || settings.languageCode() == "en");

    // 清理模拟环境变量并恢复默认设置
    env.remove("LANG");
    settings.setLanguage(TUISettings::kDefaultLanguage);
}

void test_language_set_get() {
    auto& settings = TUISettings::instance();

    // 默认自动 (Auto)
    XX_TEST_EXPECT_TRUE(settings.language() == TuiLanguage::Auto);
    XX_TEST_EXPECT_EQ(settings.languageName(), std::string_view("自动 (Auto)"));

    // 切换简体中文 → 读回
    settings.setLanguage(TuiLanguage::ZhCn);
    XX_TEST_EXPECT_TRUE(settings.language() == TuiLanguage::ZhCn);
    XX_TEST_EXPECT_EQ(settings.languageName(), std::string_view("简体中文 (zh-cn)"));

    // 切换英文 → 读回
    settings.setLanguage(TuiLanguage::EnUs);
    XX_TEST_EXPECT_TRUE(settings.language() == TuiLanguage::EnUs);
    XX_TEST_EXPECT_EQ(settings.languageName(), std::string_view("English (en-us)"));

    // 切回自动
    settings.setLanguage(TuiLanguage::Auto);
    XX_TEST_EXPECT_TRUE(settings.language() == TuiLanguage::Auto);
    XX_TEST_EXPECT_EQ(settings.languageName(), std::string_view("自动 (Auto)"));

    // 恢复默认
    settings.setLanguage(TUISettings::kDefaultLanguage);
}

void test_i18n_lookup_switches_with_language() {
    auto& settings = TUISettings::instance();
    auto& i18n     = TuiI18n::instance();

    // 默认简体中文
    settings.setLanguage(TuiLanguage::ZhCn);
    XX_TEST_EXPECT_EQ(i18n.t("settings.title"), std::string_view(" 设置 "));
    XX_TEST_EXPECT_EQ(i18n.t("session.new"), std::string_view("+ 新会话"));
    // 带格式参数的查询
    XX_TEST_EXPECT_EQ(i18n.t("settings.themeValue", "Dark"), std::string(" 主题: Dark "));
    // 未配置 key: 原样返回 key 本身
    XX_TEST_EXPECT_EQ(i18n.t("no.such.key"), std::string_view("no.such.key"));

    // 切换到 English
    settings.setLanguage(TuiLanguage::EnUs);
    XX_TEST_EXPECT_EQ(i18n.t("settings.title"), std::string_view(" Settings "));
    XX_TEST_EXPECT_EQ(i18n.t("session.new"), std::string_view("+ New Session"));
    XX_TEST_EXPECT_EQ(i18n.t("settings.themeValue", "Dark"), std::string(" Theme: Dark "));

    // 恢复默认 (简体中文)
    settings.setLanguage(TUISettings::kDefaultLanguage);
}

void test_tail_thinking_mode_set_get() {
    auto& settings = TUISettings::instance();

    const struct {
        TailThinkingMode mode;
        std::string_view name;
    } cases[] = {
        {TailThinkingMode::AutoExpand, "Auto Expand"},
        {TailThinkingMode::SingleLine, "Single Line"},
    };

    for (const auto& c : cases) {
        settings.setTailThinkingMode(c.mode);
        XX_TEST_EXPECT_TRUE(settings.tailThinkingMode() == c.mode);
        XX_TEST_EXPECT_EQ(settings.tailThinkingModeName(), c.name);
    }
}

void test_tail_thinking_names_table() {
    XX_TEST_EXPECT_EQ(TUISettings::kTailThinkingModeNames.size(), (size_t)2);
    XX_TEST_EXPECT_EQ(TUISettings::kTailThinkingModeNames[0], std::string_view("Auto Expand"));
    XX_TEST_EXPECT_EQ(TUISettings::kTailThinkingModeNames[1], std::string_view("Single Line"));
}

void test_tail_line_preview() {
    // 空串或零长度
    XX_TEST_EXPECT_EQ(tailLinePreview("", 60), "");
    XX_TEST_EXPECT_EQ(tailLinePreview("   \n\t  ", 60), "");
    XX_TEST_EXPECT_EQ(tailLinePreview("hello", 0), "");

    // 长度未超 max
    XX_TEST_EXPECT_EQ(tailLinePreview("hello world", 60), "hello world");
    XX_TEST_EXPECT_EQ(tailLinePreview("line 1\nline 2\nline 3", 60), "line 1 line 2 line 3");

    // max 为最大显示列数 (含 "..." 占 3 列): 超出时截取末尾 budget = max-3 列
    std::string text = "1234567890abcdefghij";
    XX_TEST_EXPECT_EQ(tailLinePreview(text, 10), "...defghij");

    // UTF-8 多字节: 宽字符按 2 列计, 截断只发生在码点边界
    std::string zh = "第一步分析问题第二步编写代码第三步进行测试";
    // zh 共 21 个汉字 (42 列), max=7 -> 内容预算 4 列 = 末尾 2 个汉字
    XX_TEST_EXPECT_EQ(tailLinePreview(zh, 7), "...测试");

    // 换行与空白压缩; 极小预算 (max=4 -> 预算 1 列) 至少保留最后一个码点,
    // 宽字符宁可溢出预算也不返回空内容 (渲染层 xflex_shrink 兜底裁剪)
    std::string multiline = "思考过程第一行\n\n思考过程第二行  \n  思考完成";
    XX_TEST_EXPECT_EQ(tailLinePreview(multiline, 4), "...成");

    // 恰好等于内容预算: 不加省略号 (4 汉字 8 列 <= max11 - 3)
    XX_TEST_EXPECT_EQ(tailLinePreview("一二三四", 11), "一二三四");

    // 宽字符跨预算边界: 整体舍弃放不下的码点 (6 汉字 12 列, 预算 8 列 ->
    // 反向累计 六(2)+五(2)+四(2)+三(2)=8 列, 二 放不下整体舍弃)
    XX_TEST_EXPECT_EQ(tailLinePreview("一二三四五六", 11), "...三四五六");

    // ASCII 尾部 + CJK 混合截断不切断多字节序列
    const std::string mixed = "abc中文def";
    // 总列宽 3+4+3=10 > 预算 (6-3=3): 反向累计 f(1)+e(1)+d(1)=3 列, 中文 放不下整体舍弃
    XX_TEST_EXPECT_EQ(tailLinePreview(mixed, 6), "...def");
}

// TUILogSink 按 TUISettings.logLevel 过滤 (Out 恒显示)
void test_log_sink_level_filter() {
    auto& settings = TUISettings::instance();
    auto  sink     = std::make_shared<TUILogSink>();

    auto makeEntry = [](agentxx::util::LogLevel level, const char* msg) {
        return std::make_shared<const agentxx::util::LogEntry>(agentxx::util::LogEntry{
            level,
            0,
            0,
            msg,
        });
    };

    // 默认 Info: 仅显示 Info/Warn/Error/Out, 过滤 Trace/Debug
    settings.setLogLevel(agentxx::util::LogLevel::Info);
    sink->enqueue(makeEntry(agentxx::util::LogLevel::Trace, "t"));
    sink->enqueue(makeEntry(agentxx::util::LogLevel::Debug, "d"));
    sink->enqueue(makeEntry(agentxx::util::LogLevel::Info, "i"));
    sink->enqueue(makeEntry(agentxx::util::LogLevel::Warn, "w"));
    sink->enqueue(makeEntry(agentxx::util::LogLevel::Error, "e"));
    sink->enqueue(makeEntry(agentxx::util::LogLevel::Out, "o"));
    sink->pump();
    {
        auto lines = sink->snapshot();
        XX_TEST_EXPECT_EQ(lines.size(), size_t{4});
        if (lines.size() == 4) {
            XX_TEST_EXPECT_EQ(lines[0].level, agentxx::util::LogLevel::Info);
            XX_TEST_EXPECT_EQ(lines[1].level, agentxx::util::LogLevel::Warn);
            XX_TEST_EXPECT_EQ(lines[2].level, agentxx::util::LogLevel::Error);
            XX_TEST_EXPECT_EQ(lines[3].level, agentxx::util::LogLevel::Out);
        }
    }

    // Trace: 显示全部
    sink->clear();
    settings.setLogLevel(agentxx::util::LogLevel::Trace);
    sink->enqueue(makeEntry(agentxx::util::LogLevel::Trace, "t"));
    sink->enqueue(makeEntry(agentxx::util::LogLevel::Debug, "d"));
    sink->enqueue(makeEntry(agentxx::util::LogLevel::Info, "i"));
    sink->enqueue(makeEntry(agentxx::util::LogLevel::Error, "e"));
    sink->enqueue(makeEntry(agentxx::util::LogLevel::Out, "o"));
    sink->pump();
    XX_TEST_EXPECT_EQ(sink->snapshot().size(), size_t{5});

    // Error: 仅显示 Error/Out
    sink->clear();
    settings.setLogLevel(agentxx::util::LogLevel::Error);
    sink->enqueue(makeEntry(agentxx::util::LogLevel::Trace, "t"));
    sink->enqueue(makeEntry(agentxx::util::LogLevel::Info, "i"));
    sink->enqueue(makeEntry(agentxx::util::LogLevel::Error, "e"));
    sink->enqueue(makeEntry(agentxx::util::LogLevel::Out, "o"));
    sink->pump();
    {
        auto lines = sink->snapshot();
        XX_TEST_EXPECT_EQ(lines.size(), size_t{2});
        if (lines.size() == 2) {
            XX_TEST_EXPECT_EQ(lines[0].level, agentxx::util::LogLevel::Error);
            XX_TEST_EXPECT_EQ(lines[1].level, agentxx::util::LogLevel::Out);
        }
    }

    // 恢复默认, 避免影响其他用例
    settings.setLogLevel(TUISettings::kDefaultLogLevel);
}

void test_concurrent_access() {
    // 多线程并发读写不应崩溃, 且读到的等级值始终合法
    auto&         settings    = TUISettings::instance();
    constexpr int kIterations = 2000;

    std::atomic<bool> stop{false};
    std::thread       writer([&] {
        for (int i = 0; i < kIterations && !stop.load(); ++i) {
            settings.setAnimationLevel(static_cast<AnimationLevel>(i % 5));
            settings.setLogLevel(static_cast<agentxx::util::LogLevel>(i % 6));
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
                const int ll = static_cast<int>(settings.logLevel());
                if (ll < 0 || ll > 5) {
                    invalidCount.fetch_add(1);
                }
                (void)settings.isAnimationEnabled(AnimationLevel::Medium);
                (void)settings.logLevelName();
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
    settings.setLogLevel(TUISettings::kDefaultLogLevel);
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
    settings.setLogLevel(agentxx::util::LogLevel::Warn);
    settings.setTailThinkingMode(TailThinkingMode::SingleLine);
    settings.setLanguage(TuiLanguage::EnUs);
    {
        auto fresh = agentxx::util::SettingsDb(dbPath);
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.theme", -1), int64_t{TUISettings::kThemeLight});
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.animationLevel", -1), int64_t{1}); // Low
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.logLevel", -1), int64_t{3});       // Warn
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.tailThinking", -1), int64_t{1});   // SingleLine
        XX_TEST_EXPECT_EQ(
            fresh.getInt64("tui.lang", -1),
            int64_t{static_cast<int>(TuiLanguage::EnUs)}
        ); // English
        // 注: tui.tailThinkingPreviewLen 设置项已移除 (预览长度改为按终端宽度自适应),
        // 不再有对应持久化键
    }

    // 再次变更 → 库文件同步更新
    settings.setThemeKind(TUISettings::kThemeDark);
    settings.setAnimationLevel(AnimationLevel::Ultra);
    settings.setLogLevel(agentxx::util::LogLevel::Debug);
    settings.setTailThinkingMode(TailThinkingMode::AutoExpand);
    settings.setLanguage(TuiLanguage::ZhCn);
    {
        auto fresh = agentxx::util::SettingsDb(dbPath);
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.theme", -1), int64_t{TUISettings::kThemeDark});
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.animationLevel", -1), int64_t{4}); // Ultra
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.logLevel", -1), int64_t{1});       // Debug
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.tailThinking", -1), int64_t{0});   // AutoExpand
        XX_TEST_EXPECT_EQ(
            fresh.getInt64("tui.lang", -1),
            int64_t{static_cast<int>(TuiLanguage::ZhCn)}
        );
    }

    // 变更为 English (EnUs)
    settings.setLanguage(TuiLanguage::EnUs);
    {
        auto fresh = agentxx::util::SettingsDb(dbPath);
        XX_TEST_EXPECT_EQ(
            fresh.getInt64("tui.lang", -1),
            int64_t{static_cast<int>(TuiLanguage::EnUs)}
        );
    }

    // 变更为自动 (Auto)
    settings.setLanguage(TuiLanguage::Auto);
    {
        auto fresh = agentxx::util::SettingsDb(dbPath);
        XX_TEST_EXPECT_EQ(
            fresh.getInt64("tui.lang", -1),
            int64_t{static_cast<int>(TuiLanguage::Auto)}
        );
    }

    // 恢复默认, 避免影响其他用例
    settings.setAnimationLevel(TUISettings::kDefaultAnimationLevel);
    settings.setLogLevel(TUISettings::kDefaultLogLevel);
    settings.setTailThinkingMode(TUISettings::kDefaultTailThinkingMode);
    settings.setLanguage(TUISettings::kDefaultLanguage);

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
    test_log_level_set_get();
    test_log_level_names_table();
    test_language_names_table();
    test_match_supported_language();
    test_auto_language_detection();
    test_language_set_get();
    test_i18n_lookup_switches_with_language();
    test_tail_thinking_mode_set_get();
    test_tail_thinking_names_table();
    test_tail_line_preview();
    test_log_sink_level_filter();
    test_concurrent_access();
    test_persist_to_db();

    return TestResult{g_tui_settings_passed, g_tui_settings_failed};
}

} // namespace test
} // namespace agentxx
