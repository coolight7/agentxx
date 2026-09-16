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

/// 当前运行环境是否支持**文件异步读写** (即 asio 的 `stream_file` 文件异步 I/O 是否有效)
///
/// 判断来源:
/// - 编译期: 本构建未启用 asio 文件 I/O (`ASIO_HAS_FILE` / `BOOST_ASIO_HAS_FILE`
///   均未定义, 如 macOS/BSD 等平台) 时恒为 `false`;
/// - Linux/Android: asio 的文件异步 I/O 由 io_uring 提供, 编译期宏之外还需运行时
///   确认真的能创建 io_uring 环 —— 容器/虚拟化环境的 seccomp 过滤
///   (`/proc/self/status` 的 `Seccomp: 2`) 可能拦截 `io_uring_setup` 系统调用,
///   内核过旧时该调用返回 `ENOSYS`, 此时返回 `false`;
/// - Windows: 文件异步 I/O 由 IOCP + 随机访问句柄提供 (编译期宏已确认), 无需运行时探测。
///
/// 探测结果按进程缓存 (最多探测一次), 后续调用直接返回缓存值;
/// 调用方据此选择"异步实现 / 同步兜底实现", 见 [setAsyncFileIoSupported]。
///
/// - 相关: 强制开关 (测试用) 见 [setAsyncFileIoSupported] / [resetAsyncFileIoSupported]
[[nodiscard]] bool isAsyncFileIoSupported();

/// 强制设置 [isAsyncFileIoSupported] 的判断结果 (仅测试/调试用)
/// - 设置后不再自动探测, [isAsyncFileIoSupported] 直接返回该值,
///   便于测试关闭异步路径以覆盖同步兜底实现 (或反之);
/// - 只影响判断结果, 不改变底层 asio/io_uring 的真实能力:
///   强制开启而实际不可用时, 异步读写会按各自实现的错误处理返回失败;
/// - 相关: 恢复自动探测见 [resetAsyncFileIoSupported]
///
/// - `args`:
///     - [supported] `true` 强制按可用处理; `false` 强制按不可用处理
void setAsyncFileIoSupported(bool supported);

/// 清除 [setAsyncFileIoSupported] 的强制设置, 恢复为自动探测的缓存结果
/// - 与 [setAsyncFileIoSupported] 成对使用, 测试结束后应调用以免影响后续用例
void resetAsyncFileIoSupported();

/// 计算字符串的 MD5 散列值
/// - 返回 32 位全小写十六进制字符串
///
/// - `args`:
///     - [input] 待计算散列的原始字符串
///
/// - `return` 32 位十六进制 MD5 字符串
[[nodiscard]] std::string md5Hex(std::string_view input);

/// 获取本机设备唯一标识
/// - 基于操作系统机器特征 (Linux machine-id / Windows MachineGuid / Hostname)
///   计算所得的 32 位全小写十六进制 MD5 字符串
/// - 结果按进程缓存 (最多计算一次), 避免反复读取系统信息
///
/// - `return` 32 位设备标识 MD5 字符串
[[nodiscard]] std::string getDeviceId();

}; // namespace util
}; // namespace agentxx