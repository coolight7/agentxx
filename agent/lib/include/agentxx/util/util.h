#pragma once

#include <string>

#if XX_IS_CLANG_D || XX_IS_GCC_D

#define XX_NO_SANITIZE_ADDRESS __attribute__((no_sanitize("address")))

#elif XX_IS_MSVC_D

// MSVC 的 __declspec(no_sanitize_address) 仅在启用 /fsanitize=address 时定义
#ifdef __SANITIZE_ADDRESS__
#define XX_NO_SANITIZE_ADDRESS __declspec(no_sanitize_address)
#else
#define XX_NO_SANITIZE_ADDRESS
#endif

#else

#define XX_NO_SANITIZE_ADDRESS // 其他编译器不做任何事

#endif

namespace agentxx {

namespace util {

[[nodiscard]] std::string getSystemName();

[[nodiscard]] bool isRunningInWSL();

/// PowerShell 可执行文件探测结果 (供 execute_windows_command tool 选择执行器/生成提示词)
struct PowerShellInfo {
    /// 是否找到可用的 PowerShell 可执行文件
    bool available = false;
    /// 可执行文件名, 如 "pwsh.exe" / "powershell.exe" (未找到时为空)
    std::string exeName;
    /// 版本号, 如 "7.5.4" / "5.1.26100.7462" (探测失败时为空)
    std::string version;
    /// true: PowerShell 7+ (pwsh); false: Windows PowerShell 5.1 或未知
    bool isPwsh = false;
};

/// 探测本机可用的 PowerShell 并返回其版本信息 (结果按进程缓存)。
/// - 优先探测 `pwsh.exe` (PowerShell 7+, 默认 UTF-8 输出、跨平台支持更好),
///   未找到再探测 `powershell.exe` (Windows PowerShell 5.1)
/// - Windows 本机与 WSL (经 WSL interop 调用 Windows 侧 exe) 均可用;
///   其他平台直接返回 available = false
/// - 探测内部带超时看门狗 (约 12s), 目标 exe 异常挂起时强制回收,
///   不会无限阻塞调用方
/// - [forceRefresh] 默认 false 命中缓存; true 时忽略缓存重新探测
[[nodiscard]] PowerShellInfo detectPowerShell(bool forceRefresh = false);

}; // namespace util
}; // namespace agentxx