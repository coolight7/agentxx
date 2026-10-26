// 控制台状态自检 (Windows) 测试
//
// 覆盖场景 (见 agentxx-client/util/console_state.h):
// - 输出/输入代码页被外部改成非 UTF-8 (工具子进程执行 chcp 的效果) 后,
//   自检把它改回 UTF-8 并报告"已修复" (调用方据此清屏重新对齐画面)
// - 状态正常时自检是幂等的 (不报告修复, 不产生额外副作用)
// - 输出句柄缺 VT 处理位 / 输入句柄缺 VT 输入位时要补回
//   (关掉 VT 后终端不解释 ANSI 序列, 画面整体错乱且鼠标/按键事件失效)
// - 行输入/回显/自动换行等"非必需位"的变化**不算**被外部改动:
//   conhost 在开 VT 输入后仍保持 ENABLE_LINE_INPUT/ENABLE_ECHO_INPUT 为 1,
//   若把它们当判据会每帧误判 -> 每帧清屏 -> 画面持续闪烁 (已发生过的缺陷)
//
// 测试会临时改动所在终端的代码页/控制台模式, 结束时全部还原; 进程没有控制台
// (无 console 的服务/CI 进程) 时跳过 —— 此时没有代码页可检测。
#include "agentxx-test/client/test_console_state.h"
#include "agentxx-client/util/console_state.h"

#include "utilxx_base/log.h"

#include <string_view>

#if XX_IS_WIN_D
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_console_state_passed = 0;
int g_console_state_failed = 0;

#if XX_IS_WIN_D
/// 取句柄的控制台模式; 不是控制台句柄时返回 false
bool consoleModeOf(HANDLE handle, DWORD& mode) {
    return handle != nullptr && handle != INVALID_HANDLE_VALUE
           && ::GetConsoleMode(handle, &mode) != 0;
}

/// 让 std 句柄临时指向本进程所在的控制台
/// - 进程有控制台但 std 被重定向到管道/文件时 (CI、包装脚本、工具链),
///   `GetStdHandle` 拿到的是管道句柄 —— 自检逻辑就绕开了模式检查;
///   本函数用 CONOUT$/CONIN$ 打开控制台句柄并交给 SetStdHandle, 测完必须 [unbindStdHandle]
/// - `previous` 收到的原句柄; 返回新句柄 (失败为 INVALID_HANDLE_VALUE)
HANDLE bindStdHandleToConsole(DWORD stdHandle, bool isOutput, HANDLE& previous) {
    previous             = ::GetStdHandle(stdHandle);
    const HANDLE console = ::CreateFileW(
        isOutput ? L"CONOUT$" : L"CONIN$",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );
    if (console != INVALID_HANDLE_VALUE) {
        ::SetStdHandle(stdHandle, console);
    }
    return console;
}

/// 还原 [bindStdHandleToConsole] 改过的 std 句柄
void unbindStdHandle(DWORD stdHandle, HANDLE console, HANDLE previous) {
    if (console == INVALID_HANDLE_VALUE) {
        return;
    }
    ::SetStdHandle(stdHandle, previous);
    ::CloseHandle(console);
}

/// 输出句柄是否带 VT 处理位
bool outHandleHasVt() {
    DWORD mode = 0;
    return consoleModeOf(::GetStdHandle(STD_OUTPUT_HANDLE), mode)
           && (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

/// 输入句柄是否带 VT 输入位 + 窗口事件位
bool inHandleHasVt() {
    constexpr DWORD required = ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT;
    DWORD           mode     = 0;
    return consoleModeOf(::GetStdHandle(STD_INPUT_HANDLE), mode) && (mode & required) == required;
}
#endif
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_console_state_passed
#define XX_TEST_FAILED g_console_state_failed

namespace agentxx {
namespace test {

using agentxx::client::consoleRealignSequence;
using agentxx::client::ConsoleStateGuard;

TestResult testConsoleState() {
#if XX_IS_WIN_D
    // 没有控制台时自检无事可做 (GetConsoleOutputCP 失败返回 0)
    const UINT initialOutCp = ::GetConsoleOutputCP();
    if (initialOutCp == 0) {
        XX_LOGI("console_state: 进程未附加控制台, 跳过");
        return TestResult{g_console_state_passed, g_console_state_failed};
    }

    // 先存下真实终端状态: 测试结束必须原样还原 (用户在同一个终端里跑测试)
    const UINT savedOutCp = initialOutCp;
    const UINT savedInCp  = ::GetConsoleCP();
    // std 句柄被重定向时先指向本控制台, 否则模式相关用例测不到 (见 bindStdHandleToConsole)
    HANDLE       previousOut = INVALID_HANDLE_VALUE;
    HANDLE       previousIn  = INVALID_HANDLE_VALUE;
    const HANDLE boundOut    = bindStdHandleToConsole(STD_OUTPUT_HANDLE, true, previousOut);
    const HANDLE boundIn     = bindStdHandleToConsole(STD_INPUT_HANDLE, false, previousIn);
    const HANDLE hOut        = ::GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE hIn         = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD        savedOutMode = 0;
    DWORD        savedInMode  = 0;
    const bool   hasOutMode   = consoleModeOf(hOut, savedOutMode);
    const bool   hasInMode    = consoleModeOf(hIn, savedInMode);

    // 1) 代码页被外部改掉: 自检应改回 UTF-8, 并报告改动前的取值
    ::SetConsoleOutputCP(936);
    ::SetConsoleCP(936);
    {
        ConsoleStateGuard guard;
        const auto        fix = guard.check();
        XX_TEST_EXPECT_TRUE(fix.repaired);
        XX_TEST_EXPECT_TRUE(fix.needResync);
        XX_TEST_EXPECT_EQ(fix.previousOutputCodePage, 936u);
        XX_TEST_EXPECT_TRUE(::GetConsoleOutputCP() == CP_UTF8);
        XX_TEST_EXPECT_TRUE(::GetConsoleCP() == CP_UTF8);

        // 2) 状态正常时幂等: 不报告修复
        const auto again = guard.check();
        XX_TEST_EXPECT_FALSE(again.repaired);
        XX_TEST_EXPECT_FALSE(again.needResync);
    }

    // 3) 输出模式缺 VT 处理位: 补回
    if (hasOutMode) {
        ::SetConsoleMode(
            hOut,
            savedOutMode & ~static_cast<DWORD>(ENABLE_VIRTUAL_TERMINAL_PROCESSING)
        );
        ConsoleStateGuard guard;
        const bool        broken = !outHandleHasVt();
        const auto        fix    = guard.check();
        XX_TEST_EXPECT_TRUE(broken);
        XX_TEST_EXPECT_TRUE(fix.repaired);
        XX_TEST_EXPECT_TRUE(fix.modeRepaired);
        XX_TEST_EXPECT_TRUE(fix.needResync);
        XX_TEST_EXPECT_TRUE(outHandleHasVt());
    } else {
        XX_LOGI("console_state: stdout 不是控制台句柄, 跳过 VT 输出模式用例");
    }

    // 4) 输入模式缺 VT 输入位/窗口事件位: 补回
    if (hasInMode) {
        constexpr DWORD vtInput = ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT;
        ::SetConsoleMode(hIn, savedInMode & ~vtInput);
        ConsoleStateGuard guard;
        const bool        broken = !inHandleHasVt();
        const auto        fix    = guard.check();
        XX_TEST_EXPECT_TRUE(broken);
        XX_TEST_EXPECT_TRUE(fix.repaired);
        XX_TEST_EXPECT_TRUE(fix.modeRepaired);
        XX_TEST_EXPECT_TRUE(inHandleHasVt());
    } else {
        XX_LOGI("console_state: stdin 不是控制台句柄, 跳过 VT 输入模式用例");
    }

    // 5) 行输入/回显位的变化不算"被外部改动" (回归: 曾把它当判据, 导致每帧清屏闪烁)
    if (hasInMode) {
        // 基线取**当前**模式: 用例 4 刚把 VT 输入位补回, 若这里写回测试开始前的原始模式,
        // 等于又把那一位清掉, 自检会如实报告 "被外部改动" —— 与本用例要验证的判据无关。
        DWORD currentInMode = 0;
        XX_TEST_EXPECT_TRUE(consoleModeOf(hIn, currentInMode));
        ::SetConsoleMode(hIn, currentInMode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
        ConsoleStateGuard guard;
        const auto        fix = guard.check();
        XX_TEST_EXPECT_FALSE(fix.repaired);
        XX_TEST_EXPECT_FALSE(fix.needResync);
        // 自检也不该擅自改掉这两位 (终端自己维护的状态)
        DWORD nowMode = 0;
        XX_TEST_EXPECT_TRUE(consoleModeOf(hIn, nowMode));
        XX_TEST_EXPECT_TRUE((nowMode & ENABLE_LINE_INPUT) != 0);
        XX_TEST_EXPECT_TRUE((nowMode & ENABLE_ECHO_INPUT) != 0);
    }

    // 6) 重新对齐序列: 清屏 + 光标回原点 (含复位字符属性)
    XX_TEST_EXPECT_TRUE(std::string_view{consoleRealignSequence()} == "\x1b[0m\x1b[2J\x1b[H");

    // 还原真实终端状态
    if (hasOutMode) {
        ::SetConsoleMode(hOut, savedOutMode);
    }
    if (hasInMode) {
        ::SetConsoleMode(hIn, savedInMode);
    }
    ::SetConsoleOutputCP(savedOutCp);
    ::SetConsoleCP(savedInCp);
    unbindStdHandle(STD_OUTPUT_HANDLE, boundOut, previousOut);
    unbindStdHandle(STD_INPUT_HANDLE, boundIn, previousIn);
    XX_TEST_EXPECT_TRUE(::GetConsoleOutputCP() == savedOutCp);
#else
    // 非 Windows: 空实现, 恒不报告修复
    ConsoleStateGuard guard;
    XX_TEST_EXPECT_FALSE(guard.check().repaired);
    XX_TEST_EXPECT_FALSE(guard.gaveUp());
    XX_TEST_EXPECT_TRUE(std::string_view{consoleRealignSequence()}.empty());
#endif

    return TestResult{g_console_state_passed, g_console_state_failed};
}

} // namespace test
} // namespace agentxx
