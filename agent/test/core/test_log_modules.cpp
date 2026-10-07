/// 按模块日志级别测试 (计划 OBS-5)
///
/// 背景: 全局日志级别是粗粒度开关 —— 为了排查某个子系统打开 Debug 会淹没所有模块
/// (TUI 日志窗口尤其明显)。OBS-5 给日志条目带上"模块名"(默认取源文件基名), 并允许按
/// 模块名或前缀单独设置最低级别; 本模块验证解析与过滤语义。
#include "agentxx-test/core/test_log_modules.h"

#include "agentxx/util/diagnostics.h"
#include "utilxx_base/log.h"
#include <string>
#include <string_view>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见)
int g_lm_passed = 0;
int g_lm_failed = 0;
} // namespace

#define XX_TEST_PASSED g_lm_passed
#define XX_TEST_FAILED g_lm_failed

namespace agentxx {
namespace test {

namespace {

using utilxx_base::LogDispatcher;
using utilxx_base::LogLevel;

/// 记录当前捕获到的日志行 (按顺序), 用于断言"谁出现了/谁被过滤了"
std::vector<std::string> capturedLines() {
    LogDispatcher::instance().flush();
    return agentxx::util::recentLogLines();
}

bool containsLine(const std::vector<std::string>& lines, std::string_view needle) {
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// 用显式模块名打一条日志 (绕过宏的 __FILE__ 推导, 便于构造用例)
void logWithModule(LogLevel level, std::string_view module, std::string_view text) {
    utilxx_base::xxLogPrint(level, module, std::string{text});
}

} // namespace

TestResult testLogModuleLevels() {
    const int passedBefore = g_lm_passed;
    const int failedBefore = g_lm_failed;

    // ---- 1. logModuleOf: 源文件路径 -> 模块名 ----
    XX_TEST_EXPECT_TRUE(utilxx_base::logModuleOf(__FILE__) == std::string_view{"test_log_modules"});
    XX_TEST_EXPECT_EQ(
        std::string{utilxx_base::logModuleOf("D:/proj/lib/src/agent/session_store.cpp")},
        std::string{"session_store"}
    );
    XX_TEST_EXPECT_EQ(
        std::string{utilxx_base::logModuleOf("D:\\proj\\lib\\src\\modelcall.cpp")},
        std::string{"modelcall"}
    );
    XX_TEST_EXPECT_EQ(
        std::string{utilxx_base::logModuleOf("plain_file.h")},
        std::string{"plain_file"}
    );
    XX_TEST_EXPECT_EQ(
        std::string{utilxx_base::logModuleOf("a.b.c.cpp")},
        std::string{"a.b.c"}
    );
    XX_TEST_EXPECT_EQ(std::string{utilxx_base::logModuleOf("noext")}, std::string{"noext"});
    XX_TEST_EXPECT_TRUE(utilxx_base::logModuleOf("").empty());

    // ---- 2. 规格解析: `前缀=级别,...` ----
    LogDispatcher::instance().clearModuleLevels();
    XX_TEST_EXPECT_EQ(LogDispatcher::instance().moduleLevelCount(), size_t{0});
    {
        std::vector<std::string> invalid;
        const auto applied = utilxx_base::applyLogModuleLevelSpec(
            "modelcall=debug, toolcall=TRACE ,plugin.=Warn",
            &invalid
        );
        XX_TEST_EXPECT_EQ(applied, size_t{3});
        XX_TEST_EXPECT_EQ(invalid.size(), size_t{0});
        XX_TEST_EXPECT_EQ(LogDispatcher::instance().moduleLevelCount(), size_t{3});
    }
    {
        // 非法段: 缺 `=` / 空前缀 / 级别名不认识; 结尾逗号与空段不算错误
        std::vector<std::string> invalid;
        const auto applied = utilxx_base::applyLogModuleLevelSpec(
            "noequals,=debug,bogus=nope,, good=info,",
            &invalid
        );
        XX_TEST_EXPECT_EQ(applied, size_t{1}); // 只有 good=info 生效
        XX_TEST_EXPECT_EQ(invalid.size(), size_t{3});
        if (invalid.size() == 3) {
            XX_TEST_EXPECT_EQ(invalid[0], std::string{"noequals"});
            XX_TEST_EXPECT_EQ(invalid[1], std::string{"=debug"});
            XX_TEST_EXPECT_EQ(invalid[2], std::string{"bogus=nope"});
        }
        XX_TEST_EXPECT_EQ(LogDispatcher::instance().moduleLevelCount(), size_t{4}); // 3 + 1
    }
    // 同前缀重复设置: 覆盖而不是叠加 (先清空, 只观察这一个前缀)
    LogDispatcher::instance().clearModuleLevels();
    {
        std::vector<std::string> invalid;
        const auto applied = utilxx_base::applyLogModuleLevelSpec("dup=debug,dup=error", &invalid);
        XX_TEST_EXPECT_EQ(applied, size_t{2});
        XX_TEST_EXPECT_EQ(invalid.size(), size_t{0});
        XX_TEST_EXPECT_EQ(LogDispatcher::instance().moduleLevelCount(), size_t{1});
    }
    LogDispatcher::instance().clearModuleLevels();
    XX_TEST_EXPECT_EQ(LogDispatcher::instance().moduleLevelCount(), size_t{0});

    // ---- 3. 过滤行为 (经捕获 sink 端到端) ----
    agentxx::util::enableLogCapture(128);
    agentxx::util::clearCapturedLogs();

    // 3.1 默认不过滤任何模块 (行为与引入本特性前一致)
    logWithModule(LogLevel::Debug, "mod_a", "lm-default-a-debug");
    logWithModule(LogLevel::Trace, "mod_b", "lm-default-b-trace");
    {
        const auto lines = capturedLines();
        XX_TEST_EXPECT_TRUE(containsLine(lines, "lm-default-a-debug"));
        XX_TEST_EXPECT_TRUE(containsLine(lines, "lm-default-b-trace"));
    }

    // 3.2 只过滤命中的模块, 其它模块不受影响
    LogDispatcher::instance().setModuleLevel("mod_a", LogLevel::Warn);
    agentxx::util::clearCapturedLogs();
    logWithModule(LogLevel::Debug, "mod_a", "lm-filtered-a-debug");
    logWithModule(LogLevel::Info, "mod_a", "lm-filtered-a-info");
    logWithModule(LogLevel::Warn, "mod_a", "lm-filtered-a-warn");
    logWithModule(LogLevel::Error, "mod_a", "lm-filtered-a-error");
    logWithModule(LogLevel::Debug, "mod_b", "lm-filtered-b-debug");
    logWithModule(LogLevel::Out, "mod_a", "lm-filtered-a-out");
    {
        const auto lines = capturedLines();
        XX_TEST_EXPECT_FALSE(containsLine(lines, "lm-filtered-a-debug"));
        XX_TEST_EXPECT_FALSE(containsLine(lines, "lm-filtered-a-info"));
        XX_TEST_EXPECT_TRUE(containsLine(lines, "lm-filtered-a-warn"));
        XX_TEST_EXPECT_TRUE(containsLine(lines, "lm-filtered-a-error"));
        // `Out` 是"必须展示"的输出, 任何模块级别都不拦
        XX_TEST_EXPECT_TRUE(containsLine(lines, "lm-filtered-a-out"));
        XX_TEST_EXPECT_TRUE(containsLine(lines, "lm-filtered-b-debug"));
    }

    // 3.3 前缀匹配: `plugin.` 命中 `plugin.foo`, 不命中 `plugins_other` (少了点)
    LogDispatcher::instance().setModuleLevel("plugin.", LogLevel::Warn);
    agentxx::util::clearCapturedLogs();
    logWithModule(LogLevel::Info, "plugin.foo", "lm-prefix-plugin-foo-info");
    logWithModule(LogLevel::Info, "plugins_other", "lm-prefix-other-info");
    {
        const auto lines = capturedLines();
        XX_TEST_EXPECT_FALSE(containsLine(lines, "lm-prefix-plugin-foo-info"));
        XX_TEST_EXPECT_TRUE(containsLine(lines, "lm-prefix-other-info"));
    }

    // 3.4 最长前缀优先: `plugin.noisy` 单独放宽到 Trace 后它通过, 其它 plugin.* 仍被拦
    LogDispatcher::instance().setModuleLevel("plugin.noisy", LogLevel::Trace);
    agentxx::util::clearCapturedLogs();
    logWithModule(LogLevel::Info, "plugin.noisy", "lm-longest-noisy-info");
    logWithModule(LogLevel::Info, "plugin.quiet", "lm-longest-quiet-info");
    {
        const auto lines = capturedLines();
        XX_TEST_EXPECT_TRUE(containsLine(lines, "lm-longest-noisy-info"));
        XX_TEST_EXPECT_FALSE(containsLine(lines, "lm-longest-quiet-info"));
    }

    // 3.5 清空后恢复全量输出
    LogDispatcher::instance().clearModuleLevels();
    agentxx::util::clearCapturedLogs();
    logWithModule(LogLevel::Trace, "plugin.quiet", "lm-cleared-quiet-trace");
    logWithModule(LogLevel::Debug, "mod_a", "lm-cleared-a-debug");
    {
        const auto lines = capturedLines();
        XX_TEST_EXPECT_TRUE(containsLine(lines, "lm-cleared-quiet-trace"));
        XX_TEST_EXPECT_TRUE(containsLine(lines, "lm-cleared-a-debug"));
    }

    // 收尾: 关掉捕获, 不留后台线程与内存
    agentxx::util::clearCapturedLogs();
    agentxx::util::disableLogCapture();
    LogDispatcher::instance().clearModuleLevels();

    return TestResult{g_lm_passed - passedBefore, g_lm_failed - failedBefore};
}

} // namespace test
} // namespace agentxx
