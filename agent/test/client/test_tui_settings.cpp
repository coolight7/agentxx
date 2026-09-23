#include "agentxx-test/client/test_tui_settings.h"

#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/components/overlays.h"
#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx-client/update_check.h"
#include "agentxx/agent/io/channel_io_transport.h"
#include "agentxx/plugin/client_plugin_manager.h"
#include "agentxx/util/settings_db.h"
#include "ftxui/component/event.hpp"
#include "ftxui/screen/screen.hpp"
#include "utilxx_base/env.h"
#include <chrono>
#include <cctype>
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

using namespace agentxx::client;

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
        utilxx_base::LogLevel level;
        std::string_view      name;
    } cases[] = {
        {utilxx_base::LogLevel::Trace, "Trace"},
        {utilxx_base::LogLevel::Debug, "Debug"},
        {utilxx_base::LogLevel::Info,  "Info" },
        {utilxx_base::LogLevel::Warn,  "Warn" },
        {utilxx_base::LogLevel::Error, "Error"},
        {utilxx_base::LogLevel::Out,   "Out"  },
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
    auto& env      = utilxx_base::ApplicationEnv::instance();
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
    XX_TEST_EXPECT_EQ(i18n.t("settings.title"), std::string_view("Settings"));

    // 模拟中文系统环境
    env.set("LANG", std::string_view{"zh_CN.UTF-8"});
    settings.refreshAutoLanguage();
    XX_TEST_EXPECT_TRUE(settings.effectiveLanguage() == TuiLanguage::ZhCn);
    XX_TEST_EXPECT_EQ(settings.languageCode(), std::string("zh-cn"));
    XX_TEST_EXPECT_EQ(i18n.t("settings.title"), std::string_view("设置"));

    // 模拟其它语言系统环境 (回退 EnUs)
    env.set("LANG", std::string_view{"ja_JP.UTF-8"});
    settings.refreshAutoLanguage();
    XX_TEST_EXPECT_TRUE(settings.effectiveLanguage() == TuiLanguage::EnUs);
    XX_TEST_EXPECT_EQ(settings.languageCode(), std::string("en"));
    XX_TEST_EXPECT_EQ(i18n.t("settings.title"), std::string_view("Settings"));

    // 当用户显式指定语言时, effectiveLanguage 忽略系统环境
    settings.setLanguage(TuiLanguage::ZhCn);
    XX_TEST_EXPECT_TRUE(settings.effectiveLanguage() == TuiLanguage::ZhCn);
    XX_TEST_EXPECT_EQ(settings.languageCode(), std::string("zh-cn"));
    XX_TEST_EXPECT_EQ(i18n.t("settings.title"), std::string_view("设置"));

    settings.setLanguage(TuiLanguage::EnUs);
    env.set("LANG", std::string_view{"zh_CN.UTF-8"});
    XX_TEST_EXPECT_TRUE(settings.effectiveLanguage() == TuiLanguage::EnUs);
    XX_TEST_EXPECT_EQ(settings.languageCode(), std::string("en"));
    XX_TEST_EXPECT_EQ(i18n.t("settings.title"), std::string_view("Settings"));

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
    XX_TEST_EXPECT_EQ(i18n.t("settings.title"), std::string_view("设置"));
    XX_TEST_EXPECT_EQ(i18n.t("session.new"), std::string_view("[ + 新会话 ]"));
    // 带格式参数的查询
    XX_TEST_EXPECT_EQ(i18n.t("settings.themeValue", "Dark"), std::string("主题: Dark"));
    // 消息列表角色标签 (值自带首尾空格, 拼在 1 列折叠标记后) 与 Tip 前缀/级别文本
    XX_TEST_EXPECT_EQ(i18n.t("msg.roleThink"), std::string_view(" [思考] "));
    XX_TEST_EXPECT_EQ(i18n.t("msg.roleTool"), std::string_view(" [工具] "));
    XX_TEST_EXPECT_EQ(i18n.t("msg.roleSystem"), std::string_view(" [系统] "));
    XX_TEST_EXPECT_EQ(i18n.t("msg.tipLevelInfo"), std::string_view("信息"));
    XX_TEST_EXPECT_EQ(i18n.t("msg.tipLevelWarn"), std::string_view("警告"));
    XX_TEST_EXPECT_EQ(i18n.t("msg.tipLevelError"), std::string_view("错误"));
    XX_TEST_EXPECT_EQ(i18n.t("msg.tipPrefix", "警告"), std::string(" [提示] # 警告"));
    // 未配置 key: 原样返回 key 本身
    XX_TEST_EXPECT_EQ(i18n.t("no.such.key"), std::string_view("no.such.key"));

    // 切换到 English
    settings.setLanguage(TuiLanguage::EnUs);
    XX_TEST_EXPECT_EQ(i18n.t("settings.title"), std::string_view("Settings"));
    XX_TEST_EXPECT_EQ(i18n.t("session.new"), std::string_view("[ + New Session ]"));
    XX_TEST_EXPECT_EQ(i18n.t("settings.themeValue", "Dark"), std::string("Theme: Dark"));
    XX_TEST_EXPECT_EQ(i18n.t("msg.roleThink"), std::string_view(" [Think] "));
    XX_TEST_EXPECT_EQ(i18n.t("msg.roleTool"), std::string_view(" [Tool] "));
    XX_TEST_EXPECT_EQ(i18n.t("msg.roleSystem"), std::string_view(" [System] "));
    XX_TEST_EXPECT_EQ(i18n.t("msg.tipPrefix", "Warn"), std::string(" [Tip] # Warn"));

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

    auto makeEntry = [](utilxx_base::LogLevel level, const char* msg) {
        return std::make_shared<const utilxx_base::LogEntry>(utilxx_base::LogEntry{
            level,
            0,
            0,
            msg,
        });
    };

    // 默认 Info: 仅显示 Info/Warn/Error/Out, 过滤 Trace/Debug
    settings.setLogLevel(utilxx_base::LogLevel::Info);
    sink->enqueue(makeEntry(utilxx_base::LogLevel::Trace, "t"));
    sink->enqueue(makeEntry(utilxx_base::LogLevel::Debug, "d"));
    sink->enqueue(makeEntry(utilxx_base::LogLevel::Info, "i"));
    sink->enqueue(makeEntry(utilxx_base::LogLevel::Warn, "w"));
    sink->enqueue(makeEntry(utilxx_base::LogLevel::Error, "e"));
    sink->enqueue(makeEntry(utilxx_base::LogLevel::Out, "o"));
    sink->pump();
    {
        auto lines = sink->snapshot();
        XX_TEST_EXPECT_EQ(lines.size(), size_t{4});
        if (lines.size() == 4) {
            XX_TEST_EXPECT_EQ(lines[0].level, utilxx_base::LogLevel::Info);
            XX_TEST_EXPECT_EQ(lines[1].level, utilxx_base::LogLevel::Warn);
            XX_TEST_EXPECT_EQ(lines[2].level, utilxx_base::LogLevel::Error);
            XX_TEST_EXPECT_EQ(lines[3].level, utilxx_base::LogLevel::Out);
        }
    }

    // Trace: 显示全部
    sink->clear();
    settings.setLogLevel(utilxx_base::LogLevel::Trace);
    sink->enqueue(makeEntry(utilxx_base::LogLevel::Trace, "t"));
    sink->enqueue(makeEntry(utilxx_base::LogLevel::Debug, "d"));
    sink->enqueue(makeEntry(utilxx_base::LogLevel::Info, "i"));
    sink->enqueue(makeEntry(utilxx_base::LogLevel::Error, "e"));
    sink->enqueue(makeEntry(utilxx_base::LogLevel::Out, "o"));
    sink->pump();
    XX_TEST_EXPECT_EQ(sink->snapshot().size(), size_t{5});

    // Error: 仅显示 Error/Out
    sink->clear();
    settings.setLogLevel(utilxx_base::LogLevel::Error);
    sink->enqueue(makeEntry(utilxx_base::LogLevel::Trace, "t"));
    sink->enqueue(makeEntry(utilxx_base::LogLevel::Info, "i"));
    sink->enqueue(makeEntry(utilxx_base::LogLevel::Error, "e"));
    sink->enqueue(makeEntry(utilxx_base::LogLevel::Out, "o"));
    sink->pump();
    {
        auto lines = sink->snapshot();
        XX_TEST_EXPECT_EQ(lines.size(), size_t{2});
        if (lines.size() == 2) {
            XX_TEST_EXPECT_EQ(lines[0].level, utilxx_base::LogLevel::Error);
            XX_TEST_EXPECT_EQ(lines[1].level, utilxx_base::LogLevel::Out);
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
            settings.setLogLevel(static_cast<utilxx_base::LogLevel>(i % 6));
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
    // 绑定全局设置数据库后, 验证从数据库恢复已存设置, 以及设置变更同步落库 (写入 global.db 文件)
    auto& settings = TUISettings::instance();

    auto root = fs::temp_directory_path()
                / fmt::format(
                    "agentxx_tui_settings_test_{}",
                    std::chrono::steady_clock::now().time_since_epoch().count()
                );
    auto dbPath = (root / "global.db").string();

    // 预先向数据库写入历史记录 (模拟上次会话已保存的语言和主题)
    {
        agentxx::util::SettingsDb pre(dbPath);
        pre.setInt64("tui.lang", static_cast<int64_t>(TuiLanguage::ZhCn));
        pre.setInt64("tui.theme", static_cast<int64_t>(TUISettings::kThemeLight));
        // 启动时检查更新: 上次会话已关闭 (0)
        pre.setInt64("tui.checkUpdateOnStartup", 0);
    }

    auto db = std::make_shared<agentxx::util::SettingsDb>(dbPath);
    settings.attachDb(db);

    // 校验 attachDb 成功从数据库恢复已存设置
    XX_TEST_EXPECT_TRUE(settings.language() == TuiLanguage::ZhCn);
    XX_TEST_EXPECT_TRUE(settings.themeKind() == TUISettings::kThemeLight);
    XX_TEST_EXPECT_FALSE(settings.checkUpdateOnStartup());

    // 写入设置 → 直接读库文件校验持久化 (绕过单例, 模拟重启后的新进程)
    settings.setThemeKind(TUISettings::kThemeLight);
    settings.setAnimationLevel(AnimationLevel::Low);
    settings.setLogLevel(utilxx_base::LogLevel::Warn);
    settings.setTailThinkingMode(TailThinkingMode::SingleLine);
    settings.setLanguage(TuiLanguage::EnUs);
    settings.setCheckUpdateOnStartup(true);
    {
        auto fresh = agentxx::util::SettingsDb(dbPath);
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.theme", -1), int64_t{TUISettings::kThemeLight});
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.animationLevel", -1), int64_t{1}); // Low
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.logLevel", -1), int64_t{3});       // Warn
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.tailThinking", -1), int64_t{1});   // SingleLine
        XX_TEST_EXPECT_EQ(fresh.getInt64("tui.checkUpdateOnStartup", -1), int64_t{1});
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
    settings.setLogLevel(utilxx_base::LogLevel::Debug);
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

void test_model_selector_overlay_esc_and_confirm() {
    TUISharedState sharedState;
    TUITheme       theme = TUITheme::darkTheme();
    TUICtx         ctx;
    ctx.state      = &sharedState;
    ctx.theme      = &theme;
    ctx.sessionId  = "test-session";
    ctx.postRedraw = [] {};

    // 初始状态包含可用模型，当前模型为 model-b
    sharedState.mutate([](TUIRenderState& st) {
        st.modelNames      = {"model-a", "model-b", "model-c"};
        st.cachedModelName = "model-b";
        st.modelInfoLoaded = true;
    });
    ctx.frameState = sharedState.readSnapshot();

    auto        overlay   = std::make_shared<ModelSelectorOverlay>(ctx);
    bool        confirmed = false;
    bool        closed    = false;
    std::string confirmedModel;
    overlay->onConfirm([&](std::string m) {
        confirmed      = true;
        confirmedModel = std::move(m);
    });
    overlay->onClose([&] {
        closed = true;
    });

    // 首次渲染: 验证自动对齐到 cachedModelName ("model-b", index 1)
    (void)overlay->Render();

    // 模拟按向下箭头: 选定项从 index 1 移到 index 2 ("model-c")
    overlay->OnEvent(ftxui::Event::ArrowDown);

    // 模拟按 ESC: 仅触发关闭，不应触发确认，cachedModelName 保持为 "model-b"
    overlay->OnEvent(ftxui::Event::Escape);
    XX_TEST_EXPECT_TRUE(closed);
    XX_TEST_EXPECT_FALSE(confirmed);
    XX_TEST_EXPECT_EQ(sharedState.readSnapshot()->cachedModelName, std::string("model-b"));

    // 再次测试回车确认选择: 选定项已被移到 index 2 ("model-c")
    closed    = false;
    confirmed = false;
    overlay->OnEvent(ftxui::Event::Return);
    XX_TEST_EXPECT_TRUE(closed);
    XX_TEST_EXPECT_TRUE(confirmed);
    XX_TEST_EXPECT_EQ(confirmedModel, std::string("model-c"));
    XX_TEST_EXPECT_EQ(sharedState.readSnapshot()->cachedModelName, std::string("model-c"));
}

class MockTestTransport : public agentxx::agent::AgentIOTransportBase {
public:

    std::vector<agentxx::agent::WireMessage> sentMessages;
    bool                                     isAlive = true;

    void send(agentxx::agent::WireMessage msg) override {
        sentMessages.push_back(std::move(msg));
    }

    asio::awaitable<std::optional<agentxx::agent::WireMessage>> recv() override {
        co_return std::nullopt;
    }

    void close() override {
        isAlive = false;
    }

    bool alive() const noexcept override {
        return isAlive;
    }
};

void test_tui_model_retention_on_wire_model_info_and_switch_session() {
    asio::io_context ioc;
    auto             ex = ioc.get_executor();

    auto tui = std::make_shared<TUIClientAgentIO>(ex, "session-1", TUITheme::darkTheme());

    auto transport = std::make_shared<MockTestTransport>();
    tui->setTransport(transport);

    // 1. 初始接入时 cachedModelName 为空，收到 WireModelInfo 后被初始化为服务端默认模型
    tui->onPeerMessage(agentxx::agent::WireMessage{
        agentxx::agent::WireModelInfo{
                                      .currentModel = "default-model",
                                      .models       = {"default-model", "custom-model"},
                                      }
    });
    XX_TEST_EXPECT_EQ(
        tui->sharedState().readSnapshot()->cachedModelName,
        std::string("default-model")
    );

    // 2. 用户选定新模型 custom-model
    tui->setPendingModel("custom-model");
    XX_TEST_EXPECT_EQ(
        tui->sharedState().readSnapshot()->cachedModelName,
        std::string("custom-model")
    );

    // 3. 模拟后续弹窗拉取/切换 session 时服务端又推来默认模型，不应覆盖客户端已选定的 custom-model
    tui->onPeerMessage(agentxx::agent::WireMessage{
        agentxx::agent::WireModelInfo{
                                      .currentModel = "default-model",
                                      .models       = {"default-model", "custom-model"},
                                      }
    });
    XX_TEST_EXPECT_EQ(
        tui->sharedState().readSnapshot()->cachedModelName,
        std::string("custom-model")
    );

    // 4. 切换会话到 session-2: 验证 custom-model 保持不变，并向服务端同步 WireSelectModel
    transport->sentMessages.clear();
    tui->switchToSession("session-2");
    XX_TEST_EXPECT_EQ(
        tui->sharedState().readSnapshot()->cachedModelName,
        std::string("custom-model")
    );
    XX_TEST_EXPECT_EQ(tui->sharedState().readSnapshot()->pendingModel, std::string("custom-model"));
    XX_TEST_EXPECT_EQ(tui->currentSessionId(), std::string("session-2"));

    // 检查 transport 接收到的消息: 包含 WireSwitchSession 和 WireSelectModel
    bool sawSwitch = false;
    bool sawSelect = false;
    for (const auto& msg : transport->sentMessages) {
        if (auto* sw = std::get_if<agentxx::agent::WireSwitchSession>(&msg)) {
            sawSwitch = true;
            XX_TEST_EXPECT_EQ(sw->sessionId, std::string("session-2"));
        } else if (auto* sel = std::get_if<agentxx::agent::WireSelectModel>(&msg)) {
            sawSelect = true;
            XX_TEST_EXPECT_EQ(sel->sessionId, std::string("session-2"));
            XX_TEST_EXPECT_EQ(sel->model, std::string("custom-model"));
        }
    }
    XX_TEST_EXPECT_TRUE(sawSwitch);
    XX_TEST_EXPECT_TRUE(sawSelect);
}

/// 暴露受保护回调的 TUI 端点 (测试用): onSync 由服务端回推 (会话切换/重连),
/// 生产路径经 onPeerMessage 分发, 测试直接调用以模拟"服务端回推全量 Sync"
class TestableTuiClientIO : public TUIClientAgentIO {
public:

    using TUIClientAgentIO::TUIClientAgentIO;

    void pushSync(const agentxx::agent::WireSyncPayload& payload) {
        onSync(payload);
    }
};

/// 回归: 切换会话后输入栏的 [+ 📎 附件] 按钮不消失
///
/// 服务端切换会话时先回推新会话的全量 Sync (onSync 整体重建 TUIRenderState),
/// 再回推模型信息。模型能力表来自 agent 配置 (与会话无关), 必须跨 Sync 保留:
/// 否则能力表变空 -> currentModelCapability() 无多模态能力 ->
/// inputCfg.canAttach 返回 false -> 输入栏的附件按钮消失 (点击弹窗也无法打开)。
void test_tui_multimodal_capability_survives_session_sync() {
    asio::io_context ioc;
    auto             ex = ioc.get_executor();

    auto tui       = std::make_shared<TestableTuiClientIO>(ex, "session-1", TUITheme::darkTheme());
    auto transport = std::make_shared<MockTestTransport>();
    tui->setTransport(transport);

    // 1. 客户端接入: 服务端回推模型信息 (含各模型多模态能力)
    agentxx::agent::WireModelInfo info;
    info.currentModel = "vision-model";
    info.models       = {"vision-model", "text-only-model"};
    info.capabilities.push_back(agentxx::agent::ModelCapabilityInfo{
        .name       = "vision-model",
        .imageInput = true,
    });
    info.capabilities.push_back(agentxx::agent::ModelCapabilityInfo{
        .name = "text-only-model",
    });
    tui->onPeerMessage(agentxx::agent::WireMessage{info});

    {
        auto snap = tui->sharedState().readSnapshot();
        XX_TEST_EXPECT_TRUE(snap->modelInfoLoaded);
        XX_TEST_EXPECT_EQ(snap->modelCapabilities.size(), size_t{2});
        // 附件按钮显隐依据 (InputComponent::Config::canAttach)
        XX_TEST_EXPECT_TRUE(snap->currentModelCapability().hasMultimodalInput());
    }

    // 2. 切换会话: 服务端回推新会话的全量 Sync (onSync 整体重建渲染状态)
    agentxx::agent::WireSyncPayload payload;
    payload.messages.push_back(
        agentxx::agent::ViewMessage::makeText(agentxx::agent::ViewMessage::Role::User, "hi")
    );
    tui->pushSync(payload);

    {
        auto snap = tui->sharedState().readSnapshot();
        // 历史消息被整体替换
        XX_TEST_EXPECT_EQ(snap->messages.size(), size_t{1});
        // 模型能力表跨 Sync 保留: 附件按钮不消失 (修复前此处为空 -> 按钮隐藏)
        XX_TEST_EXPECT_EQ(snap->modelCapabilities.size(), size_t{2});
        XX_TEST_EXPECT_TRUE(snap->currentModelCapability().hasMultimodalInput());
        XX_TEST_EXPECT_TRUE(snap->modelInfoLoaded);
        XX_TEST_EXPECT_EQ(snap->modelNames.size(), size_t{2});
    }

    // 3. 兼容旧服务端: 切换会话后回推的模型信息不带 capabilities 时,
    //    能力表按已加载内容合并, 不被清空
    agentxx::agent::WireModelInfo legacyInfo;
    legacyInfo.currentModel = "vision-model";
    legacyInfo.models       = {"vision-model", "text-only-model"};
    tui->onPeerMessage(agentxx::agent::WireMessage{legacyInfo});

    {
        auto snap = tui->sharedState().readSnapshot();
        XX_TEST_EXPECT_TRUE(snap->currentModelCapability().hasMultimodalInput());
    }

    // 4. 能力按模型区分: 切到不支持多模态的模型后不再认为可附件
    tui->setPendingModel("text-only-model");
    {
        auto snap = tui->sharedState().readSnapshot();
        XX_TEST_EXPECT_EQ(snap->currentModelCapability().name, std::string("text-only-model"));
        XX_TEST_EXPECT_FALSE(snap->currentModelCapability().hasMultimodalInput());
    }

    // 5. 再次 Sync (如断线重连) 后能力仍按模型名正确匹配
    tui->pushSync(payload);
    {
        auto snap = tui->sharedState().readSnapshot();
        XX_TEST_EXPECT_EQ(snap->modelCapabilities.size(), size_t{2});
        XX_TEST_EXPECT_FALSE(snap->currentModelCapability().hasMultimodalInput());
    }
}

// ---------------------------------------------------------------------------
// 权限状态 (Info 侧边栏授权按钮): wire 同步 + 点击切换
// ---------------------------------------------------------------------------

/// 服务端权限状态消息应写入帧状态 (按钮显示依据); 点击切换按钮应发送
/// WireSetFullAuth 并立即在本地反映新状态 (乐观更新)
void test_tui_permission_state_sync_and_toggle() {
    asio::io_context ioc;
    auto             ex = ioc.get_executor();

    auto tui       = std::make_shared<TUIClientAgentIO>(ex, "session-1", TUITheme::darkTheme());
    auto transport = std::make_shared<MockTestTransport>();
    tui->setTransport(transport);

    // 1. 初始未完全授权
    XX_TEST_EXPECT_FALSE(tui->sharedState().readSnapshot()->fullAuthorized);

    // 2. 收到服务端状态 (完全授权): 界面状态同步
    tui->onPeerMessage(agentxx::agent::WireMessage{agentxx::agent::WirePermissionState{true}});
    XX_TEST_EXPECT_TRUE(tui->sharedState().readSnapshot()->fullAuthorized);

    // 3. 收到状态 (恢复询问): 同步回落
    tui->onPeerMessage(agentxx::agent::WireMessage{agentxx::agent::WirePermissionState{false}});
    XX_TEST_EXPECT_FALSE(tui->sharedState().readSnapshot()->fullAuthorized);

    // 4. 点击切换: 本地状态立即切换, 且发送 WireSetFullAuth(true)
    transport->sentMessages.clear();
    tui->refreshRenderContext();
    tui->toggleFullAuth();
    XX_TEST_EXPECT_TRUE(tui->sharedState().readSnapshot()->fullAuthorized);
    bool sawSetFullAuth = false;
    for (const auto& msg : transport->sentMessages) {
        if (auto* set = std::get_if<agentxx::agent::WireSetFullAuth>(&msg)) {
            sawSetFullAuth = true;
            XX_TEST_EXPECT_TRUE(set->fullAuth);
        }
    }
    XX_TEST_EXPECT_TRUE(sawSetFullAuth);

    // 5. 再次点击: 切回询问态并发送 WireSetFullAuth(false)
    transport->sentMessages.clear();
    tui->refreshRenderContext();
    tui->toggleFullAuth();
    XX_TEST_EXPECT_FALSE(tui->sharedState().readSnapshot()->fullAuthorized);
    sawSetFullAuth = false;
    for (const auto& msg : transport->sentMessages) {
        if (auto* set = std::get_if<agentxx::agent::WireSetFullAuth>(&msg)) {
            sawSetFullAuth = true;
            XX_TEST_EXPECT_FALSE(set->fullAuth);
        }
    }
    XX_TEST_EXPECT_TRUE(sawSetFullAuth);

    // 6. 握手完成 (hello ack) 后主动查询一次权限状态
    transport->sentMessages.clear();
    agentxx::agent::WireHelloAck ack;
    ack.ok        = true;
    ack.sessionId = "session-1";
    tui->onPeerMessage(agentxx::agent::WireMessage{std::move(ack)});
    bool sawGetPermissionState = false;
    for (const auto& msg : transport->sentMessages) {
        if (std::holds_alternative<agentxx::agent::WireGetPermissionState>(msg)) {
            sawGetPermissionState = true;
        }
    }
    XX_TEST_EXPECT_TRUE(sawGetPermissionState);
}

/// Info 侧边栏底部工作目录行渲染授权按钮: 非完全授权显示 "[ 询问授权 ]",
/// 完全授权显示 "[ 完全授权 ]" (文案随语言表, 两种语言各验证一次)
void test_tui_info_footer_auth_button() {
    asio::io_context ioc;
    auto             ex = ioc.get_executor();

    const auto originalLang = TUISettings::instance().language();
    auto       renderFooterText
        = [&](const std::shared_ptr<TUIClientAgentIO>& tui) -> std::string {
        auto element = tui->renderInfoSidebarFooter();
        auto screen  = ftxui::Screen::Create(ftxui::Dimension::Fixed(60), ftxui::Dimension::Fixed(6));
        ftxui::Render(screen, element);
        return screen.ToString();
    };

    auto tui = std::make_shared<TUIClientAgentIO>(ex, "session-1", TUITheme::darkTheme());
    auto transport = std::make_shared<MockTestTransport>();
    tui->setTransport(transport);

    // 简体中文: 询问授权 -> 完全授权
    TUISettings::instance().setLanguage(TuiLanguage::ZhCn);
    tui->refreshRenderContext();
    auto zhAsk = renderFooterText(tui);
    XX_TEST_EXPECT_TRUE(zhAsk.find(std::string(TuiI18n::instance().t("info.authAsk"))) != std::string::npos);
    XX_TEST_EXPECT_TRUE(zhAsk.find(std::string(TuiI18n::instance().t("info.authFull"))) == std::string::npos);

    tui->onPeerMessage(agentxx::agent::WireMessage{agentxx::agent::WirePermissionState{true}});
    tui->refreshRenderContext();
    auto zhFull = renderFooterText(tui);
    XX_TEST_EXPECT_TRUE(
        zhFull.find(std::string(TuiI18n::instance().t("info.authFull"))) != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(
        zhFull.find(std::string(TuiI18n::instance().t("info.authAsk"))) == std::string::npos
    );

    // 英文: 同一状态使用英文文案 (翻译表生效)
    TUISettings::instance().setLanguage(TuiLanguage::EnUs);
    tui->refreshRenderContext();
    auto enFull = renderFooterText(tui);
    XX_TEST_EXPECT_TRUE(enFull.find("[ Full Permission ]") != std::string::npos);
    XX_TEST_EXPECT_TRUE(enFull.find("完全授权") == std::string::npos);

    TUISettings::instance().setLanguage(TuiLanguage::ZhCn);
    tui->refreshRenderContext();
    XX_TEST_EXPECT_TRUE(
        renderFooterText(tui).find("[ 完全授权 ]") != std::string::npos
    );

    // 语言设置复位 (本模块其他用例依赖默认语言表)
    TUISettings::instance().setLanguage(originalLang);
}

/// Info 侧边栏底部: 启动更新检查发现新版本时显示提示行 (点击复制发布链接);
/// 无更新 / 检查失败时不显示 (不影响界面)
void test_tui_info_footer_update_notice() {
    asio::io_context ioc;
    auto             ex = ioc.get_executor();

    const auto originalLang = TUISettings::instance().language();
    TUISettings::instance().setLanguage(TuiLanguage::ZhCn);

    auto tui = std::make_shared<TUIClientAgentIO>(ex, "session-1", TUITheme::darkTheme());
    auto transport = std::make_shared<MockTestTransport>();
    tui->setTransport(transport);

    auto renderFooterText = [&]() -> std::string {
        auto element = tui->renderInfoSidebarFooter();
        auto screen
            = ftxui::Screen::Create(ftxui::Dimension::Fixed(70), ftxui::Dimension::Fixed(8));
        ftxui::Render(screen, element);
        return screen.ToString();
    };

    // 1. 未检查/无更新: 不出现提示行
    tui->refreshRenderContext();
    XX_TEST_EXPECT_TRUE(renderFooterText().find("新版本") == std::string::npos);

    // 2. 检查失败: 同样不出现提示行 (只记日志)
    {
        agentxx::client::UpdateCheckResult failed;
        failed.ok    = false;
        failed.error = "network unreachable";
        tui->applyUpdateCheckResult(failed);
        tui->refreshRenderContext();
        XX_TEST_EXPECT_TRUE(renderFooterText().find("新版本") == std::string::npos);
    }

    // 3. 无更新 (最新版本不高于当前): 不出现提示行
    {
        agentxx::client::UpdateCheckResult noUpdate;
        noUpdate.ok        = true;
        noUpdate.hasUpdate = false;
        noUpdate.latestTag = "v0.0.1";
        noUpdate.url       = "https://github.com/coolight7/agentxx/releases/tag/v0.0.1";
        tui->applyUpdateCheckResult(noUpdate);
        tui->refreshRenderContext();
        XX_TEST_EXPECT_TRUE(renderFooterText().find("新版本") == std::string::npos);
    }

    // 4. 发现新版本: 提示行显示标签与点击提示 (中文文案)
    {
        agentxx::client::UpdateCheckResult hasUpdate;
        hasUpdate.ok        = true;
        hasUpdate.hasUpdate = true;
        hasUpdate.latestTag = "v9.9.9";
        hasUpdate.url       = "https://github.com/coolight7/agentxx/releases/tag/v9.9.9";
        tui->applyUpdateCheckResult(hasUpdate);
        tui->refreshRenderContext();
        const auto text = renderFooterText();
        XX_TEST_EXPECT_TRUE(text.find("[ 新版本 v9.9.9 ]") != std::string::npos);
        XX_TEST_EXPECT_TRUE(text.find("点击复制发布链接") != std::string::npos);
    }

    // 5. 英文: 同一状态使用英文文案
    TUISettings::instance().setLanguage(TuiLanguage::EnUs);
    tui->refreshRenderContext();
    const auto enText = renderFooterText();
    XX_TEST_EXPECT_TRUE(enText.find("[ New Version v9.9.9 ]") != std::string::npos);
    XX_TEST_EXPECT_TRUE(enText.find("click to copy the release link") != std::string::npos);

    TUISettings::instance().setLanguage(originalLang);
}

/// 即时更新检查结果处理: 有新版本时写入共享状态 (Info 侧边栏提示行, 与启动检查
/// 共用); 无更新/失败时不改动界面状态 (仅在 UI 线程 toast 说明)
void test_tui_manual_update_check_state() {
    asio::io_context ioc;
    auto             ex = ioc.get_executor();

    const auto originalLang = TUISettings::instance().language();
    TUISettings::instance().setLanguage(TuiLanguage::ZhCn);

    auto tui       = std::make_shared<TUIClientAgentIO>(ex, "session-1", TUITheme::darkTheme());
    auto transport = std::make_shared<MockTestTransport>();
    tui->setTransport(transport);

    auto footerText = [&]() -> std::string {
        tui->refreshRenderContext();
        auto element = tui->renderInfoSidebarFooter();
        auto screen
            = ftxui::Screen::Create(ftxui::Dimension::Fixed(70), ftxui::Dimension::Fixed(8));
        ftxui::Render(screen, element);
        return screen.ToString();
    };

    // 1. 无更新: 不写入状态 (提示行不出现)
    agentxx::client::UpdateCheckResult noUpdate;
    noUpdate.ok        = true;
    noUpdate.hasUpdate = false;
    noUpdate.latestTag = "v0.0.1";
    tui->applyManualUpdateCheckResult(noUpdate);
    XX_TEST_EXPECT_TRUE(tui->sharedState().readSnapshot()->availableUpdateTag.empty());
    XX_TEST_EXPECT_TRUE(footerText().find("新版本") == std::string::npos);

    // 2. 检查失败: 不写入状态
    agentxx::client::UpdateCheckResult failed;
    failed.ok    = false;
    failed.error = "network unreachable";
    tui->applyManualUpdateCheckResult(failed);
    XX_TEST_EXPECT_TRUE(tui->sharedState().readSnapshot()->availableUpdateTag.empty());
    XX_TEST_EXPECT_TRUE(footerText().find("新版本") == std::string::npos);

    // 3. 有新版本: 写入 tag/url, Info 侧边栏底部出现提示行 (点击复制链接)
    agentxx::client::UpdateCheckResult hasUpdate;
    hasUpdate.ok        = true;
    hasUpdate.hasUpdate = true;
    hasUpdate.latestTag = "v9.9.9";
    hasUpdate.url       = "https://github.com/coolight7/agentxx/releases/tag/v9.9.9";
    tui->applyManualUpdateCheckResult(hasUpdate);
    auto snap = tui->sharedState().readSnapshot();
    XX_TEST_EXPECT_EQ(snap->availableUpdateTag, std::string("v9.9.9"));
    XX_TEST_EXPECT_EQ(
        snap->availableUpdateUrl,
        std::string("https://github.com/coolight7/agentxx/releases/tag/v9.9.9")
    );
    XX_TEST_EXPECT_TRUE(footerText().find("[ 新版本 v9.9.9 ]") != std::string::npos);

    TUISettings::instance().setLanguage(originalLang);
}

/// 即时更新检查: 结果提示文案 —— 无更新 toast "已经是最新版本"; 失败 toast 失败原因;
/// 有更新时不走 toast (打开更新提示弹窗, 由 UpdateNoticeOverlay 用例覆盖)
void test_tui_manual_update_check_toast() {
    asio::io_context ioc;
    auto             ex = ioc.get_executor();

    const auto originalLang = TUISettings::instance().language();
    TUISettings::instance().setLanguage(TuiLanguage::ZhCn);

    auto tui = std::make_shared<TUIClientAgentIO>(ex, "session-1", TUITheme::darkTheme());

    // 1. 无更新: "已经是最新版本"
    agentxx::client::UpdateCheckResult noUpdate;
    noUpdate.ok        = true;
    noUpdate.hasUpdate = false;
    noUpdate.latestTag = "v0.0.1";
    tui->showUpdateCheckResult(noUpdate);
    XX_TEST_EXPECT_EQ(tui->toastText(), std::string(TuiI18n::instance().t("toast.updateLatest")));

    // 2. 检查失败: 提示带失败原因
    agentxx::client::UpdateCheckResult failed;
    failed.ok    = false;
    failed.error = "network unreachable";
    tui->showUpdateCheckResult(failed);
    XX_TEST_EXPECT_TRUE(tui->toastText().find("network unreachable") != std::string::npos);

    // 3. 有更新 (无模态容器: 未启动 UI 线程): 不崩且不改动 toast; 状态已写入共享状态
    const auto before = std::string{tui->toastText()};
    agentxx::client::UpdateCheckResult hasUpdate;
    hasUpdate.ok        = true;
    hasUpdate.hasUpdate = true;
    hasUpdate.latestTag = "v9.9.9";
    hasUpdate.url       = "https://github.com/coolight7/agentxx/releases/tag/v9.9.9";
    tui->showUpdateCheckResult(hasUpdate);
    XX_TEST_EXPECT_EQ(tui->toastText(), before);

    // 4. 英文文案
    TUISettings::instance().setLanguage(TuiLanguage::EnUs);
    tui->showUpdateCheckResult(noUpdate);
    XX_TEST_EXPECT_EQ(
        tui->toastText(),
        std::string(TuiI18n::instance().t("toast.updateLatest"))
    );

    TUISettings::instance().setLanguage(originalLang);
}

// ---------------------------------------------------------------------------
// 插件全局快捷键列表 (设置弹窗条目 + 只读列表弹窗)
// ---------------------------------------------------------------------------

namespace {

/// 本模块快捷键用例的共享主题 (与其它 TUI 用例一致: 不依赖物理终端)
TUITheme& keybindTheme() {
    static TUITheme theme = TUITheme::darkTheme();
    return theme;
}

/// 声明快捷键能力的测试适配器 (宿主未声明 `agentxx.client.keybind` 时
/// registerKeybind 会被能力门控拒绝)
class KeybindTestUiAdapter : public agentxx::plugin::PluginUiAdapter {
public:

    agentxx::plugin::InterfaceSet supportedInterfaces() const override {
        namespace pi = agentxx::plugin::plugin_interfaces;
        return {std::string{pi::ClientUi}, std::string{pi::ClientKeybind}};
    }
};

/// 测试用管理器: 暴露 createInstance 以构造"伪实例"
/// (快捷键归属只按实例名; 生产路径的实例由 dlopen + lifecycle 创建)
class KeybindTestManager : public agentxx::plugin::ClientPluginManager {
public:

    using ClientPluginManager::ClientPluginManager;
    using ClientPluginManager::createInstance;
};

/// 经管理器入口注册一条快捷键 (与插件走同一个 registerKeybind)
AgentxxKeybind* registerProbeKeybind(
    agentxx::plugin::ClientPluginManager& mgr,
    agentxx::plugin::ClientPluginInstance* inst,
    std::string_view                      keys,
    std::string_view                      description = std::string_view{}
) {
    AgentxxKeybindSpec spec{};
    spec.version     = 1;
    spec.keys        = agentxx::plugin::PluginStringView::from(keys.data(), keys.size());
    spec.description = agentxx::plugin::PluginStringView::from(
        description.data() != nullptr ? description.data() : "",
        description.size()
    );
    spec.on_keybind = [](void*) {};
    spec.user_data  = nullptr;
    return mgr.registerKeybind(inst, &spec);
}

/// 固定视口下的最小 TUICtx (快捷键弹窗只读主题与插件管理器)
TUICtx keybindTestCtx(const std::shared_ptr<agentxx::plugin::ClientPluginManager>& mgr, int height = 34) {
    TUICtx ctx;
    ctx.theme          = &keybindTheme();
    ctx.postRedraw     = [] {};
    ctx.viewportWidth  = 100;
    ctx.viewportHeight = height;
    ctx.pluginManager  = mgr;
    return ctx;
}

/// 剥离 ANSI CSI 转义序列, 仅保留可见字符 (与 test_tui_surface / test_tui_context_overlay 同款)
std::string stripAnsiSequences(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        if (raw[i] == '\x1B') {
            size_t j = i + 1;
            if (j < raw.size() && raw[j] == '[') {
                ++j;
                while (j < raw.size() && !std::isalpha(static_cast<unsigned char>(raw[j]))) {
                    ++j;
                }
                if (j < raw.size()) {
                    ++j;
                }
            } else if (j < raw.size()) {
                ++j;
            }
            i = j;
            continue;
        }
        out += raw[i++];
    }
    return out;
}

/// 屏幕全部文本 (剥离颜色序列; 宽字符按一个字符出现一次, 与看到的界面一致)
///
/// 说明: 不要逐格拼接 `PixelAt(x,y).character` —— 宽字符占两格, 第二格为空串,
/// 逐格拼接会把 "快捷键" 变成 "快 捷 键" (子串断言因此永远匹配不上)。
std::string screenText(const ftxui::Screen& screen) {
    return stripAnsiSequences(screen.ToString());
}

/// 在屏幕各行中查找文本 (宽字符按一个字符计), 返回该行 y 与该行首个非空白列 x
///
/// 供"模拟点击某一行"用: x 取行首可见列的屏幕坐标 (单元格下标即显示列),
/// 点击该坐标必然落在该行的命中区域左边界内。
bool findRowWithText(const ftxui::Screen& screen, std::string_view needle, int& outX, int& outY) {
    if (needle.empty()) {
        return false;
    }
    for (int y = 0; y < screen.dimy(); ++y) {
        std::string line;
        for (int x = 0; x < screen.dimx(); ++x) {
            const std::string& ch = screen.PixelAt(x, y).character;
            if (ch.empty()) {
                continue; // 宽字符的右半格 / 未绘制格
            }
            line += ch;
        }
        if (line.find(needle) == std::string::npos) {
            continue;
        }
        outX = 0;
        outY = y;
        for (int x = 0; x < screen.dimx(); ++x) {
            if (!screen.PixelAt(x, y).character.empty()) {
                outX = x;
                break;
            }
        }
        return true;
    }
    return false;
}

/// 屏幕某一行的可见文字 (去掉行首尾空白; 宽字符按一个字符计)
///
/// 弹窗左右内边距与值色带右侧的填充都是空格, 比较整行文字时先去掉。
std::string rowText(const ftxui::Screen& screen, int y) {
    if (y < 0 || y >= screen.dimy()) {
        return {};
    }
    std::string line;
    for (int x = 0; x < screen.dimx(); ++x) {
        const std::string& ch = screen.PixelAt(x, y).character;
        if (ch.empty()) {
            continue;
        }
        line += ch;
    }
    const size_t begin = line.find_first_not_of(' ');
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = line.find_last_not_of(' ');
    return line.substr(begin, end - begin + 1);
}

/// 整行文字恰为 needle 的行号 (不存在返回 -1; 供"分组标题独占一行"等断言用)
int rowOfExact(const ftxui::Screen& screen, std::string_view needle) {
    for (int y = 0; y < screen.dimy(); ++y) {
        if (rowText(screen, y) == needle) {
            return y;
        }
    }
    return -1;
}

/// 屏幕文本里是否含滚动条滑块字符 (┃ 整格 / ╹ ╻ 半格; 见 ftxui vscroll_indicator)
bool hasScrollbarThumb(const std::string& text) {
    return text.find("┃") != std::string::npos || text.find("╹") != std::string::npos
           || text.find("╻") != std::string::npos;
}

/// 渲染一帧 (固定视口, 不随物理终端尺寸漂移)
ftxui::Screen renderOnce(const ftxui::Component& comp, int width, int height) {
    auto screen = ftxui::Screen::Create(
        ftxui::Dimension::Fixed(width),
        ftxui::Dimension::Fixed(height)
    );
    ftxui::Render(screen, comp->Render());
    return screen;
}

/// 语言临时切换 (断言中文字面量; 作用域结束恢复原设置)
struct ScopedLanguage {
    TuiLanguage saved = TUISettings::instance().language();

    explicit ScopedLanguage(TuiLanguage lang) {
        TUISettings::instance().setLanguage(lang);
    }

    ~ScopedLanguage() {
        TUISettings::instance().setLanguage(saved);
    }
};

} // namespace

/// 设置弹窗: "快捷键" 条目显示插件已注册的条数, 点击条目打开快捷键列表
void test_settings_overlay_keybind_entry() {
    ScopedLanguage lang(TuiLanguage::ZhCn);

    // 未装配插件管理器: 条数按 0 显示 (不崩)
    {
        auto         ctx    = keybindTestCtx(nullptr, 40);
        auto         comp   = std::make_shared<SettingsOverlay>(ctx);
        const auto   screen = renderOnce(comp, 100, 40);
        const auto   text   = screenText(screen);
        XX_TEST_EXPECT_TRUE(text.find("插件快捷键: 0") != std::string::npos);
        XX_TEST_EXPECT_TRUE(text.find("快捷键") != std::string::npos);
    }

    // 已注册 2 条: 条数反映在条目值上
    asio::io_context io;
    auto             mgr = std::make_shared<KeybindTestManager>(io.get_executor());
    mgr->setUiAdapter(std::make_shared<KeybindTestUiAdapter>());
    auto instA = mgr->createInstance("probe_a");
    auto instB = mgr->createInstance("probe_b");
    XX_TEST_EXPECT_TRUE(registerProbeKeybind(*mgr, instA.get(), "ctrl+alt+k", "toggle") != nullptr);
    XX_TEST_EXPECT_TRUE(registerProbeKeybind(*mgr, instB.get(), "f9", "refresh") != nullptr);

    auto ctx      = keybindTestCtx(mgr, 40);
    auto comp     = std::make_shared<SettingsOverlay>(ctx);
    int  opened   = 0;
    comp->onKeybindList([&] {
        ++opened;
    });

    const auto screen = renderOnce(comp, 100, 40);
    XX_TEST_EXPECT_TRUE(screenText(screen).find("插件快捷键: 2") != std::string::npos);

    // 点击"快捷键"条目 (条目整行可点: 点击标签行即激活)
    int x = 0;
    int y = 0;
    XX_TEST_EXPECT_TRUE(findRowWithText(screen, "快捷键", x, y));
    ftxui::Mouse click;
    click.button = ftxui::Mouse::Left;
    click.motion = ftxui::Mouse::Released;
    click.x      = x;
    click.y      = y;
    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::Mouse("", click)));
    XX_TEST_EXPECT_EQ(opened, 1);
}

/// 设置弹窗: "更新"组的"启动时检查更新"条目 (默认开 -> 点击切换为关, 持久化开关同步)
void test_settings_overlay_check_update_entry() {
    ScopedLanguage lang(TuiLanguage::ZhCn);
    auto&          settings = TUISettings::instance();
    const bool     original = settings.checkUpdateOnStartup();
    settings.setCheckUpdateOnStartup(true);

    auto       ctx  = keybindTestCtx(nullptr, 40);
    auto       comp = std::make_shared<SettingsOverlay>(ctx);
    const auto text = screenText(renderOnce(comp, 100, 40));
    XX_TEST_EXPECT_TRUE(text.find("启动时检查更新: 开") != std::string::npos);

    // 点击条目 (整行可点: 点击标签行即激活) -> 切换为关
    int x = 0;
    int y = 0;
    XX_TEST_EXPECT_TRUE(findRowWithText(renderOnce(comp, 100, 40), "启动时检查更新", x, y));
    ftxui::Mouse click;
    click.button = ftxui::Mouse::Left;
    click.motion = ftxui::Mouse::Released;
    click.x      = x;
    click.y      = y;
    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::Mouse("", click)));
    XX_TEST_EXPECT_FALSE(settings.checkUpdateOnStartup());
    XX_TEST_EXPECT_TRUE(
        screenText(renderOnce(comp, 100, 40)).find("启动时检查更新: 关") != std::string::npos
    );

    // 再点一次: 回到开
    XX_TEST_EXPECT_TRUE(findRowWithText(renderOnce(comp, 100, 40), "启动时检查更新", x, y));
    click.x = x;
    click.y = y;
    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::Mouse("", click)));
    XX_TEST_EXPECT_TRUE(settings.checkUpdateOnStartup());

    // 复位 (共用单例)
    settings.setCheckUpdateOnStartup(original);
}

/// 设置弹窗: "更新"组内的"检查更新"条目 (点击即发起一次即时检查)
///
/// - 条目文案随语言表 (标签"检查更新" + 值"立即检查")
/// - 点击触发 onCheckUpdate 回调, 弹窗保持打开 (可连续检查)
/// - 只发起检查, 不改变"启动时检查更新"开关
void test_settings_overlay_check_update_now_entry() {
    ScopedLanguage lang(TuiLanguage::ZhCn);
    auto&          settings = TUISettings::instance();
    const bool     original = settings.checkUpdateOnStartup();
    settings.setCheckUpdateOnStartup(true);

    auto       ctx  = keybindTestCtx(nullptr, 40);
    auto       comp = std::make_shared<SettingsOverlay>(ctx);
    int        checks = 0;
    comp->onCheckUpdate([&] {
        ++checks;
    });

    const auto text = screenText(renderOnce(comp, 100, 40));
    XX_TEST_EXPECT_TRUE(text.find("检查更新") != std::string::npos);   // 条目标签
    XX_TEST_EXPECT_TRUE(text.find("立即检查") != std::string::npos);   // 条目值
    XX_TEST_EXPECT_EQ(checks, 0);

    // 点击条目 (整行可点: 点击值行即可) -> 触发一次检查, 启动开关不变
    int x = 0;
    int y = 0;
    XX_TEST_EXPECT_TRUE(findRowWithText(renderOnce(comp, 100, 40), "立即检查", x, y));
    ftxui::Mouse click;
    click.button = ftxui::Mouse::Left;
    click.motion = ftxui::Mouse::Released;
    click.x      = x;
    click.y      = y;
    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::Mouse("", click)));
    XX_TEST_EXPECT_EQ(checks, 1);
    XX_TEST_EXPECT_TRUE(settings.checkUpdateOnStartup());

    // 再点一次: 再次触发 (弹窗未关闭, 由外部按"进行中"去重)
    XX_TEST_EXPECT_TRUE(findRowWithText(renderOnce(comp, 100, 40), "立即检查", x, y));
    click.x = x;
    click.y = y;
    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::Mouse("", click)));
    XX_TEST_EXPECT_EQ(checks, 2);

    // 英文文案 (同一状态换语言后重新渲染)
    TUISettings::instance().setLanguage(TuiLanguage::EnUs);
    const auto enText = screenText(renderOnce(comp, 100, 40));
    XX_TEST_EXPECT_TRUE(enText.find("Check for Updates Now") != std::string::npos);
    XX_TEST_EXPECT_TRUE(enText.find("Check Now") != std::string::npos);

    // 复位 (共用单例)
    settings.setCheckUpdateOnStartup(original);
}

/// 更新提示弹窗: 版式 (版本行/链接行/下载按钮) 与交互 (点击/Enter 下载, Esc 关闭)
void test_update_notice_overlay_render_and_download() {
    ScopedLanguage lang(TuiLanguage::ZhCn);

    const std::string url = "https://github.com/coolight7/agentxx/releases/tag/v9.9.9";
    auto              ctx = keybindTestCtx(nullptr, 20);
    auto              comp
        = std::make_shared<UpdateNoticeOverlay>(ctx, "0.1.0", "v9.9.9", url);
    int  downloads = 0;
    bool closed    = false;
    comp->onDownload([&] {
        ++downloads;
    });
    comp->onClose([&] {
        closed = true;
    });

    // 未渲染: 按钮没有命中区域, 任意位置的点击都不触发下载
    XX_TEST_EXPECT_TRUE(comp->downloadButtonBox().IsEmpty());
    ftxui::Mouse mouse;
    mouse.button = ftxui::Mouse::Left;
    mouse.motion = ftxui::Mouse::Released;
    mouse.x      = 0;
    mouse.y      = 0;
    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::Mouse("", mouse)));
    XX_TEST_EXPECT_EQ(downloads, 0);

    // 内容版式: 标题 + "新版本 0.1.0 -> v9.9.9" + "· 链接" + "[ 前往下载 ]"
    const auto screen = renderOnce(comp, 100, 20);
    const auto text   = screenText(screen);
    XX_TEST_EXPECT_TRUE(text.find("发现新版本") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("新版本 0.1.0 -> v9.9.9") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("· " + url) != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("[ 前往下载 ]") != std::string::npos);

    // 点击版本行 (按钮之外): 不触发下载
    int x = 0;
    int y = 0;
    XX_TEST_EXPECT_TRUE(findRowWithText(screen, "-> v9.9.9", x, y));
    mouse.x = x;
    mouse.y = y;
    comp->OnEvent(ftxui::Event::Mouse("", mouse));
    XX_TEST_EXPECT_EQ(downloads, 0);
    XX_TEST_EXPECT_FALSE(closed);

    // 点击按钮: 触发一次下载 (是否关闭弹窗由外部回调决定)
    const auto btn = comp->downloadButtonBox();
    XX_TEST_EXPECT_FALSE(btn.IsEmpty());
    mouse.x = btn.x_min + 1;
    mouse.y = btn.y_min;
    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::Mouse("", mouse)));
    XX_TEST_EXPECT_EQ(downloads, 1);

    // Enter 等价点击按钮
    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::Return));
    XX_TEST_EXPECT_EQ(downloads, 2);

    // Esc 关闭
    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::Escape));
    XX_TEST_EXPECT_TRUE(closed);

    // 英文文案
    TUISettings::instance().setLanguage(TuiLanguage::EnUs);
    const auto enText = screenText(renderOnce(comp, 100, 20));
    XX_TEST_EXPECT_TRUE(enText.find("Update Available") != std::string::npos);
    XX_TEST_EXPECT_TRUE(enText.find("New version 0.1.0 -> v9.9.9") != std::string::npos);
    XX_TEST_EXPECT_TRUE(enText.find("[ Download ]") != std::string::npos);
}

/// 更新提示弹窗: 窄终端下长链接按可用宽度换行 (内容不越界, 按钮仍可命中)
void test_update_notice_overlay_narrow_terminal() {
    ScopedLanguage lang(TuiLanguage::ZhCn);

    const std::string url
        = "https://github.com/coolight7/agentxx/releases/tag/v9.9.9-with-a-very-long-tag-name";
    auto ctx           = keybindTestCtx(nullptr, 16);
    ctx.viewportWidth  = 40;
    ctx.viewportHeight = 16;
    auto comp = std::make_shared<UpdateNoticeOverlay>(ctx, "0.1.0", "v9.9.9", url);
    int  downloads = 0;
    comp->onDownload([&] {
        ++downloads;
    });

    const auto screen = renderOnce(comp, 40, 16);
    const auto text   = screenText(screen);
    // 版本行与按钮仍在 (链接行换行不会挤掉其它内容)
    XX_TEST_EXPECT_TRUE(text.find("新版本 0.1.0 -> v9.9.9") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("[ 前往下载 ]") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("github.com") != std::string::npos); // 链接分段换行后仍有内容

    // 按钮命中区域落在屏幕内 (长链接换行后仍可点击下载)
    const auto btn = comp->downloadButtonBox();
    XX_TEST_EXPECT_FALSE(btn.IsEmpty());
    XX_TEST_EXPECT_TRUE(btn.x_min >= 0);
    XX_TEST_EXPECT_TRUE(btn.x_max < 40);
    XX_TEST_EXPECT_TRUE(btn.y_min >= 0);
    XX_TEST_EXPECT_TRUE(btn.y_max < 16);

    ftxui::Mouse mouse;
    mouse.button = ftxui::Mouse::Left;
    mouse.motion = ftxui::Mouse::Released;
    mouse.x      = btn.x_min + 1;
    mouse.y      = btn.y_min;
    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::Mouse("", mouse)));
    XX_TEST_EXPECT_EQ(downloads, 1);
}

/// 设置弹窗: 条目按分组显示 (分组标题行不可选中/点击; "检查更新"在"更新"组内)
void test_settings_overlay_groups() {
    ScopedLanguage lang(TuiLanguage::ZhCn);
    auto&          settings = TUISettings::instance();
    const bool     original = settings.checkUpdateOnStartup();
    settings.setCheckUpdateOnStartup(true);

    // 视口足够高: 全部内容一次显示 (不滚动)
    auto       ctx    = keybindTestCtx(nullptr, 40);
    auto       comp   = std::make_shared<SettingsOverlay>(ctx);
    const auto screen = renderOnce(comp, 100, 40);

    // 四个分组标题各占一整行, 按 界面 -> 显示 -> 更新 -> 其他 排列
    const int yInterface = rowOfExact(screen, "界面");
    const int yDisplay   = rowOfExact(screen, "显示");
    const int yUpdate    = rowOfExact(screen, "更新");
    const int yOther     = rowOfExact(screen, "其他");
    XX_TEST_EXPECT_TRUE(yInterface >= 0);
    XX_TEST_EXPECT_TRUE(yDisplay > yInterface);
    XX_TEST_EXPECT_TRUE(yUpdate > yDisplay);
    XX_TEST_EXPECT_TRUE(yOther > yUpdate);

    // "更新"组内两条: 启动时检查更新 (标签 + 值) / 检查更新 (标签 + 值)
    if (yUpdate >= 0) {
        XX_TEST_EXPECT_EQ(rowOfExact(screen, "启动时检查更新"), yUpdate + 1);
        XX_TEST_EXPECT_EQ(rowOfExact(screen, "启动时检查更新: 开"), yUpdate + 2);
        XX_TEST_EXPECT_EQ(rowText(screen, yUpdate + 3), std::string("")); // 条目之间空行
        XX_TEST_EXPECT_EQ(rowOfExact(screen, "检查更新"), yUpdate + 4);
        XX_TEST_EXPECT_EQ(rowOfExact(screen, "立即检查"), yUpdate + 5);

        // 点击分组标题行: 只有标题, 不命中任何条目 (选中项不变, 开关不被切换)
        int x = 0;
        int y = 0;
        XX_TEST_EXPECT_TRUE(findRowWithText(screen, "更新", x, y));
        XX_TEST_EXPECT_EQ(y, yUpdate);
        ftxui::Mouse click;
        click.button = ftxui::Mouse::Left;
        click.motion = ftxui::Mouse::Released;
        click.x      = x;
        click.y      = y;
        XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::Mouse("", click)));
        XX_TEST_EXPECT_EQ(comp->selectedIndex(), 0);
        XX_TEST_EXPECT_TRUE(settings.checkUpdateOnStartup());
    }

    // 英文分组标题 (同一状态换语言后重新渲染)
    TUISettings::instance().setLanguage(TuiLanguage::EnUs);
    const auto enScreen = renderOnce(comp, 100, 40);
    XX_TEST_EXPECT_TRUE(rowOfExact(enScreen, "Interface") >= 0);
    XX_TEST_EXPECT_TRUE(rowOfExact(enScreen, "Display") >= 0);
    XX_TEST_EXPECT_TRUE(rowOfExact(enScreen, "Update") >= 0);
    XX_TEST_EXPECT_TRUE(rowOfExact(enScreen, "Other") >= 0);

    // 复位 (共用单例)
    settings.setCheckUpdateOnStartup(original);
}

/// 设置弹窗: 终端过矮时内容可滚动 (条目不再压缩间距, 内容不缺失)
void test_settings_overlay_short_terminal_scroll() {
    ScopedLanguage lang(TuiLanguage::ZhCn);

    // 80x24 (经典默认终端): 内容 30 行 (9 条目 × 2 + 4 分组标题 + 8 间距) 装不下,
    // 内容区按可用高度限高 (24 - 外框 6 = 18 行), 弹窗本身不超出终端
    auto ctx = keybindTestCtx(nullptr, 24);
    ctx.viewportWidth = 80;
    auto       comp   = std::make_shared<SettingsOverlay>(ctx);
    const auto text   = screenText(renderOnce(comp, 80, 24));
    XX_TEST_EXPECT_TRUE(text.find("界面") != std::string::npos);  // 首个分组标题
    XX_TEST_EXPECT_TRUE(text.find("主题") != std::string::npos);  // 首项
    XX_TEST_EXPECT_TRUE(text.find("[Esc]") != std::string::npos); // 底部提示 (弹窗未被裁掉)
    XX_TEST_EXPECT_TRUE(text.find("关于") == std::string::npos);  // 末尾条目在视口外

    // End: 选中项跳到末项, 内容区跟着滚动 (末项进来, 首项出去)
    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::End));
    const auto scrolled = screenText(renderOnce(comp, 80, 24));
    XX_TEST_EXPECT_TRUE(scrolled.find("关于") != std::string::npos);
    XX_TEST_EXPECT_TRUE(scrolled.find("其他") != std::string::npos); // 末个分组标题
    XX_TEST_EXPECT_TRUE(scrolled.find("主题") == std::string::npos);
    XX_TEST_EXPECT_TRUE(scrolled.find("[Esc]") != std::string::npos);

    // Home: 回到顶部
    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::Home));
    const auto home = screenText(renderOnce(comp, 80, 24));
    XX_TEST_EXPECT_TRUE(home.find("主题") != std::string::npos);
    XX_TEST_EXPECT_TRUE(home.find("关于") == std::string::npos);
}

/// 设置弹窗: 内容超出可用高度时显示滚动条, 装得下时不显示
void test_settings_overlay_scroll_indicator() {
    ScopedLanguage lang(TuiLanguage::ZhCn);

    // 100x24: 内容 (30 行) 多于可见高度 (18 行) -> 内容区右侧出现滚动条
    {
        auto ctx = keybindTestCtx(nullptr, 24);
        ctx.viewportWidth = 100;
        auto       comp   = std::make_shared<SettingsOverlay>(ctx);
        const auto text   = screenText(renderOnce(comp, 100, 24));
        XX_TEST_EXPECT_TRUE(hasScrollbarThumb(text));
    }
    // 100x40: 内容全部显示 -> 没有滚动条
    {
        auto ctx = keybindTestCtx(nullptr, 40);
        ctx.viewportWidth = 100;
        auto       comp   = std::make_shared<SettingsOverlay>(ctx);
        const auto text   = screenText(renderOnce(comp, 100, 40));
        XX_TEST_EXPECT_FALSE(hasScrollbarThumb(text));
    }
}

/// 设置弹窗: 滚轮上/下 = 移动选中项 (与文件选择弹窗一致; 内容区随选中项滚动)
void test_settings_overlay_mouse_wheel() {
    ScopedLanguage lang(TuiLanguage::ZhCn);

    auto       ctx  = keybindTestCtx(nullptr, 24);
    auto       comp = std::make_shared<SettingsOverlay>(ctx);
    renderOnce(comp, 100, 24);
    XX_TEST_EXPECT_EQ(comp->selectedIndex(), 0);

    ftxui::Mouse wheel;
    // 真实滚轮事件的 motion 为 Pressed/Released (见 ftxui 输入解析): 处理只看 button
    wheel.motion = ftxui::Mouse::Pressed;
    wheel.x      = 20;
    wheel.y      = 10;

    // 已在首项: 上滚不动
    wheel.button = ftxui::Mouse::WheelUp;
    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::Mouse("", wheel)));
    XX_TEST_EXPECT_EQ(comp->selectedIndex(), 0);

    // 下滚: 选中项下移一项 (条目多时无需键盘也能看到后面的内容)
    wheel.button = ftxui::Mouse::WheelDown;
    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::Mouse("", wheel)));
    XX_TEST_EXPECT_EQ(comp->selectedIndex(), 1);
    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::Mouse("", wheel)));
    XX_TEST_EXPECT_EQ(comp->selectedIndex(), 2);

    // 滚到末尾: 内容区随之滚动, 末项可见
    for (int i = 0; i < 12; ++i) {
        comp->OnEvent(ftxui::Event::Mouse("", wheel));
    }
    XX_TEST_EXPECT_EQ(comp->selectedIndex(), 8); // 最后一项 (共 9 项)
    const auto text = screenText(renderOnce(comp, 100, 24));
    XX_TEST_EXPECT_TRUE(text.find("关于") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("[Esc]") != std::string::npos); // 弹窗仍未被裁掉

    // 上滚: 回到首项
    wheel.button = ftxui::Mouse::WheelUp;
    for (int i = 0; i < 12; ++i) {
        comp->OnEvent(ftxui::Event::Mouse("", wheel));
    }
    XX_TEST_EXPECT_EQ(comp->selectedIndex(), 0);
    XX_TEST_EXPECT_TRUE(screenText(renderOnce(comp, 100, 24)).find("主题") != std::string::npos);
}

/// 快捷键列表弹窗: 无注册时显示空状态; Esc 关闭
void test_keybind_list_overlay_empty() {    ScopedLanguage lang(TuiLanguage::ZhCn);

    auto         ctx    = keybindTestCtx(nullptr);
    auto         comp   = std::make_shared<KeybindListOverlay>(ctx);
    bool         closed = false;
    comp->onClose([&] {
        closed = true;
    });

    const auto text = screenText(renderOnce(comp, 100, 34));
    XX_TEST_EXPECT_TRUE(text.find("插件快捷键") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("暂无插件注册的全局快捷键") != std::string::npos);
    XX_TEST_EXPECT_EQ(comp->keybindCount(), size_t{0});

    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::Escape));
    XX_TEST_EXPECT_TRUE(closed);
}

/// 快捷键列表弹窗: 键位 + 说明 + 归属插件; 冲突段 (申请方 / 占用方); 注销后刷新
void test_keybind_list_overlay_entries() {
    ScopedLanguage lang(TuiLanguage::ZhCn);

    asio::io_context io;
    auto             mgr = std::make_shared<KeybindTestManager>(io.get_executor());
    mgr->setUiAdapter(std::make_shared<KeybindTestUiAdapter>());
    auto  owner = mgr->createInstance("probe_owner");
    auto  other = mgr->createInstance("probe_other");
    auto* bindK = registerProbeKeybind(*mgr, owner.get(), "ctrl+alt+k", "toggle panel");
    XX_TEST_EXPECT_TRUE(bindK != nullptr);
    XX_TEST_EXPECT_TRUE(registerProbeKeybind(*mgr, owner.get(), "f9") != nullptr); // 无说明
    // 跨插件抢同一键位被拒: 记录进注册表快照 (列表的冲突段)
    XX_TEST_EXPECT_TRUE(registerProbeKeybind(*mgr, other.get(), "ctrl+alt+k", "dup") == nullptr);

    auto       ctx    = keybindTestCtx(mgr);
    auto       comp   = std::make_shared<KeybindListOverlay>(ctx);
    const auto text   = screenText(renderOnce(comp, 100, 34));
    XX_TEST_EXPECT_EQ(comp->keybindCount(), size_t{2});
    XX_TEST_EXPECT_TRUE(text.find("ctrl+alt+k") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("toggle panel") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("probe_owner") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("( 无说明 )") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("键位冲突 (1)") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("申请方 probe_other") != std::string::npos);
    XX_TEST_EXPECT_TRUE(text.find("占用方 probe_owner") != std::string::npos);

    // 键位空出 (占用方注销): 冲突段消失, 列表内容每帧随快照刷新
    mgr->unregisterKeybind(owner.get(), bindK);
    const auto refreshed = screenText(renderOnce(comp, 100, 34));
    XX_TEST_EXPECT_EQ(comp->keybindCount(), size_t{1});
    XX_TEST_EXPECT_TRUE(refreshed.find("键位冲突") == std::string::npos);
    XX_TEST_EXPECT_TRUE(refreshed.find("ctrl+alt+k") == std::string::npos);
    XX_TEST_EXPECT_TRUE(refreshed.find("f9") != std::string::npos);
}

/// 快捷键列表弹窗: 条目多于弹窗高度时可滚动 (方向键)
void test_keybind_list_overlay_scroll() {
    ScopedLanguage lang(TuiLanguage::ZhCn);

    asio::io_context io;
    auto             mgr    = std::make_shared<KeybindTestManager>(io.get_executor());
    mgr->setUiAdapter(std::make_shared<KeybindTestUiAdapter>());
    auto             instA  = mgr->createInstance("scroll_a");
    auto             instB  = mgr->createInstance("scroll_b");
    constexpr int    kTotal = 24; // 16 (单实例上限) + 8
    for (int i = 0; i < kTotal; ++i) {
        const std::string keys = fmt::format("ctrl+alt+{}", static_cast<char>('a' + i));
        auto&             host = (i < 16) ? instA : instB;
        XX_TEST_EXPECT_TRUE(registerProbeKeybind(*mgr, host.get(), keys, "many") != nullptr);
    }

    // 视口压低 (弹窗高度 = 视口 4/5): 24 条远多于可见行数
    auto       ctx         = keybindTestCtx(mgr, 16);
    auto       comp        = std::make_shared<KeybindListOverlay>(ctx);
    const auto firstScreen = screenText(renderOnce(comp, 100, 16));
    XX_TEST_EXPECT_EQ(comp->keybindCount(), size_t{kTotal});
    XX_TEST_EXPECT_TRUE(firstScreen.find("ctrl+alt+a") != std::string::npos);
    XX_TEST_EXPECT_TRUE(firstScreen.find("ctrl+alt+x") == std::string::npos); // 末条在视口外

    // 方向键滚动到底: 末条可见, 首条滚出
    for (int i = 0; i < 30; ++i) {
        XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::ArrowDown));
    }
    const auto scrolled = screenText(renderOnce(comp, 100, 16));
    XX_TEST_EXPECT_TRUE(scrolled.find("ctrl+alt+x") != std::string::npos);
    XX_TEST_EXPECT_TRUE(scrolled.find("ctrl+alt+a") == std::string::npos);

    // Home / End: 跳到列表首尾
    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::Home));
    const auto home = screenText(renderOnce(comp, 100, 16));
    XX_TEST_EXPECT_TRUE(home.find("ctrl+alt+a") != std::string::npos);
    XX_TEST_EXPECT_TRUE(comp->OnEvent(ftxui::Event::End));
    const auto end = screenText(renderOnce(comp, 100, 16));
    XX_TEST_EXPECT_TRUE(end.find("ctrl+alt+x") != std::string::npos);
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
    test_model_selector_overlay_esc_and_confirm();
    test_tui_model_retention_on_wire_model_info_and_switch_session();
    test_tui_multimodal_capability_survives_session_sync();
    test_tui_permission_state_sync_and_toggle();
    test_tui_info_footer_auth_button();
    test_tui_info_footer_update_notice();
    test_settings_overlay_keybind_entry();
    test_settings_overlay_check_update_entry();
    test_settings_overlay_check_update_now_entry();
    test_settings_overlay_groups();
    test_settings_overlay_short_terminal_scroll();
    test_settings_overlay_scroll_indicator();
    test_settings_overlay_mouse_wheel();
    test_update_notice_overlay_render_and_download();
    test_update_notice_overlay_narrow_terminal();
    test_tui_manual_update_check_state();
    test_tui_manual_update_check_toast();
    test_keybind_list_overlay_empty();
    test_keybind_list_overlay_entries();
    test_keybind_list_overlay_scroll();

    return TestResult{g_tui_settings_passed, g_tui_settings_failed};
}

} // namespace test
} // namespace agentxx
