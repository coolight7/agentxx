#include "agentxx-client/util/console_state.h"

#if XX_IS_WIN_D

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace agentxx::client {

namespace {

/// 取 std 句柄的控制台模式
/// - 句柄不是控制台时 (`GetConsoleMode` 失败, 例如被重定向到管道/文件) 返回 false
bool readConsoleMode(DWORD stdHandle, DWORD& mode) {
    const HANDLE handle = ::GetStdHandle(stdHandle);
    return handle != nullptr && handle != INVALID_HANDLE_VALUE
           && ::GetConsoleMode(handle, &mode) != 0;
}

/// 写回 std 句柄的控制台模式; 失败 (终端不支持该位) 返回 false
bool writeConsoleMode(DWORD stdHandle, DWORD mode) {
    const HANDLE handle = ::GetStdHandle(stdHandle);
    return handle != nullptr && handle != INVALID_HANDLE_VALUE
           && ::SetConsoleMode(handle, mode) != 0;
}

} // namespace

ConsoleStateFix ConsoleStateGuard::check() {
    ConsoleStateFix fix;

    // 未附加控制台 (服务/管道运行): GetConsoleOutputCP 失败返回 0, 无事可做
    const UINT outCp = ::GetConsoleOutputCP();
    if (outCp == 0) {
        return fix;
    }

    // 代码页: TUI 输出的是 UTF-8 字节, 被外部改过就改回
    if (outCp != CP_UTF8) {
        fix.repaired               = true;
        fix.previousOutputCodePage = outCp;
        fix.needResync             = ::SetConsoleOutputCP(CP_UTF8) != 0;
    }
    if (::GetConsoleCP() != CP_UTF8 && ::SetConsoleCP(CP_UTF8) != 0) {
        fix.repaired   = true;
        fix.needResync = true;
    }

    // 输出模式: 缺 VT 处理位时补回 (只加这一个位, 其它位是终端自己的状态, 不去改)
    DWORD outMode = 0;
    if (!outGiveUp_ && readConsoleMode(STD_OUTPUT_HANDLE, outMode)
        && (outMode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) {
        fix.repaired = true;
        if (writeConsoleMode(STD_OUTPUT_HANDLE, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            fix.modeRepaired = true;
            fix.needResync   = true;
        } else {
            // 写不回去就不再重试: 否则每帧都判定为缺失, 调用方每帧清屏
            outGiveUp_ = true;
        }
    }

    // 输入模式: 缺 VT 输入位/窗口事件位时补回 (同样只加必需位)
    constexpr DWORD inRequired = ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT;
    DWORD           inMode     = 0;
    if (!inGiveUp_ && readConsoleMode(STD_INPUT_HANDLE, inMode)
        && (inMode & inRequired) != inRequired) {
        fix.repaired = true;
        if (writeConsoleMode(STD_INPUT_HANDLE, inMode | inRequired)) {
            fix.modeRepaired = true;
            fix.needResync   = true;
        } else {
            inGiveUp_ = true;
        }
    }

    return fix;
}

const char* consoleRealignSequence() {
    // 复位字符属性 + 清屏 + 光标回原点
    return "\x1b[0m\x1b[2J\x1b[H";
}

} // namespace agentxx::client

#else // 非 Windows

namespace agentxx::client {

ConsoleStateFix ConsoleStateGuard::check() {
    // 终端状态由 termios 管理, 没有共享代码页问题
    return ConsoleStateFix{};
}

const char* consoleRealignSequence() {
    return "";
}

} // namespace agentxx::client

#endif
