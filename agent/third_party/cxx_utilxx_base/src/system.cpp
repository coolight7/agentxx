#include "utilxx_base/system.h"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/env.h"
#include "utilxx_base/exception.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include "fmt/format.h"
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>

#include <asio/detail/config.hpp>
#include <cstddef>

#if (XX_IS_LINUX_D || XX_IS_ANDROID_D) && (defined(ASIO_HAS_FILE) || defined(BOOST_ASIO_HAS_FILE)) \
    && (defined(ASIO_HAS_IO_URING) || defined(BOOST_ASIO_HAS_IO_URING))
// io_uring 运行时探测 (asio 的 Linux 文件异步 I/O 由 io_uring 提供);
// 包含路径/链接库由 agentxx_util 的 PkgConfig::uring 传递
#include <liburing.h>
#endif

#include <atomic>
#include <cstring>

namespace {

/// 校验 PowerShell 版本字符串格式: 纯数字与点的组合, 如 "7.5.4" / "5.1.26100.7462"
/// - 防御探测到同名无关程序输出无关内容的情况 (视为未找到)
bool isValidPsVersion(std::string_view version) {
    if (version.empty()) {
        return false;
    }
    bool hasDot = false;
    for (char c : version) {
        if (c == '.') {
            hasDot = true;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    return hasDot;
}

} // namespace

#if XX_IS_LINUX_D
#include <algorithm>
#include <chrono>
#include <climits>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

static std::optional<std::string>                   systemName_;
static std::optional<bool>                          isRunningInWSL_;
static std::optional<utilxx_base::PowerShellInfo> psInfo_;

/// 上述探测结果缓存的互斥
/// - 探测函数会从插件 offload 线程并发调用 (如 execute_command 探测 PowerShell),
///   同进程可能多实例/多调用并发进入; 无锁的 "读-判断-写" 是数据竞争 (UB)
/// - 各缓存独立加锁: detectPowerShell 内部会调用 isRunningInWSL (另一把锁),
///   不构成嵌套同锁的死锁
static std::mutex systemNameMutex;
static std::mutex wslMutex;
static std::mutex psInfoMutex;

std::string utilxx_base::getSystemName() {
    std::lock_guard<std::mutex> lk{systemNameMutex};
    if (systemName_.has_value()) {
        return *systemName_;
    }
    std::ifstream f("/etc/os-release");
    std::string   line, name;
    while (std::getline(f, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            // 去掉 PRETTY_NAME="..." 两侧的引号
            name = line.substr(13);
            if (!name.empty() && name.front() == '"') {
                name.erase(0, 1);
            }
            if (!name.empty() && name.back() == '"') {
                name.pop_back();
            }
            break;
        }
    }
    f.close();
    if (!name.empty()) {
        systemName_ = std::move(name);
        return *systemName_;
    }

    // 备选：uname 系统调用
    struct utsname buf;
    if (uname(&buf) == 0) {
        systemName_ = fmt::format("{} {}", buf.sysname, buf.release);
    } else {
        systemName_ = "Linux";
    }
    return *systemName_;
}

bool utilxx_base::isRunningInWSL() {
    std::lock_guard<std::mutex> lk{wslMutex};
    if (isRunningInWSL_.has_value()) {
        return *isRunningInWSL_;
    }
    // catchError: 文件系统探测失败按非 WSL 处理, 并记录日志
    return utilxx_base::catchError<bool>(
        [&]() -> bool {
            isRunningInWSL_ = std::filesystem::exists("/proc/sys/fs/binfmt_misc/WSLInterop");
            return *isRunningInWSL_;
        },
        [&](std::string errmsg) -> bool {
            isRunningInWSL_ = false;
            XX_LOGD("isRunningInWSL exception: {}", errmsg);
            return false;
        }
    );
}

/// WSL/Linux 下探测 Windows 侧 PowerShell 可执行文件并获取版本号
/// - 经 WSL interop (binfmt_misc) 直接执行 Windows exe, 仅 WSL 环境可用
/// - 直接 execvp 候选 exe (不经 sh/cmd), 版本脚本作为单个 argv 元素传入,
///   从根源避免 `$PSVersionTable` 被 shell 展开 / 引号转义问题
/// - fork+pipe+poll 看门狗: 子进程异常挂起 (interop 损坏等) 时 SIGKILL 强制回收,
///   [timeoutMs] 内无输出即放弃该候选; 子进程一律 waitpid 回收不留僵尸
static std::string runPsVersionProbe(const char* exeName, int timeoutMs) {
    // exeName 只允许字母数字与点 (防止拼接 shell 注入), 候选列表由本函数调用方控制
    for (char c : std::string_view{exeName}) {
        if (false
            == ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || c == '.')) {
            return {};
        }
    }
    // Windows PowerShell 5.1 的 PSVersionTable 不含 PSEdition; 直接取 PSVersion 最稳妥
    // -NoProfile: 跳过用户 profile 加速启动并避免 profile 脚本副作用/交互
    // -NonInteractive: 禁止交互提示
    int pipefd[2] = {-1, -1};
    if (::pipe(pipefd) != 0) {
        return {};
    }
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return {};
    }
    if (pid == 0) {
        // 子进程: stdout 接到管道, stdin 重定向到 /dev/null, stderr 丢弃
        ::dup2(pipefd[1], STDOUT_FILENO);
        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            int devnull2 = ::open("/dev/null", O_WRONLY);
            if (devnull2 >= 0) {
                ::dup2(devnull2, STDERR_FILENO);
                ::close(devnull2);
            }
            ::close(devnull);
        }
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        // 新会话: 即使子进程组被误杀也不波及 agent
        ::setsid();
        const char* argv[] = {
            exeName,
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "$PSVersionTable.PSVersion.ToString()",
            nullptr,
        };
        // exeName 由调用方候选列表控制, 已校验字符集; 直接经 PATH 查找执行
        ::execvp(exeName, const_cast<char* const*>(argv));
        ::_exit(127);
    }
    ::close(pipefd[1]);

    // 看门狗读取输出, 超时则杀子进程
    std::string out;
    char        buf[1024];
    bool        childExited = false;
    auto        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (false == childExited) {
        auto remainMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now()
        )
                            .count();
        if (remainMs <= 0) {
            ::kill(pid, SIGKILL);
            XX_LOGW("PowerShell probe timeout, killed pid={}", static_cast<long>(pid));
            break;
        }

        struct pollfd pfd {
            .fd = pipefd[0], .events = POLLIN
        };

        int pollRet = ::poll(&pfd, 1, static_cast<int>(std::min<long long>(remainMs, INT_MAX)));
        if (pollRet > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
            if (n > 0) {
                out.append(buf, static_cast<size_t>(n));
                continue;
            }
        }
        if (pollRet > 0 && (pfd.revents & (POLLHUP | POLLERR))) {
            // EOF/错误: 读空管道后回收子进程
            while (true) {
                ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
                if (n <= 0) {
                    break;
                }
                out.append(buf, static_cast<size_t>(n));
            }
        }
        // 非阻塞回收子进程; 未退出则继续等待输出 (进程退出时管道关闭会触发 POLLHUP)
        int   status = 0;
        pid_t w      = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            childExited = true;
        }
    }
    ::close(pipefd[0]);
    if (false == childExited) {
        int status = 0;
        ::waitpid(pid, &status, 0);
    }
    return out;
}

utilxx_base::PowerShellInfo utilxx_base::detectPowerShell(bool forceRefresh) {
    // 缓存 + 串行化: 探测结果可能被插件 offload 线程并发请求 (无锁读改写
    // optional 是数据竞争); 探测期间持锁使并发调用方等待首次结果后复用缓存
    std::lock_guard<std::mutex> lk{psInfoMutex};
    if (psInfo_.has_value() && false == forceRefresh) {
        return *psInfo_;
    }
    auto info = utilxx_base::PowerShellInfo{};
    // 仅 WSL 支持经 interop 调用 Windows exe; 非 WSL Linux 无 PowerShell 可探测
    if (utilxx_base::isRunningInWSL()) {
        // catchError: 探测异常按未找到处理
        utilxx_base::catchError<bool>(
            [&]() -> bool {
                // 优先 pwsh.exe (PowerShell 7+, 输出默认 UTF-8 更友好), 再 powershell.exe
                static constexpr std::array<std::pair<const char*, bool>, 2> candidates{
                    {{"pwsh.exe", true}, {"powershell.exe", false}},
                };
                for (const auto& [exeName, isPwsh] : candidates) {
                    // 探测超时给足裕量: 首次运行 powershell.exe 可能较慢
                    auto output  = runPsVersionProbe(exeName, 12000);
                    auto version = utilxx_base::removeBetweenSpace(output);
                    // 版本可能带 BOM/多余内容, 只取第一行
                    if (auto nlPos = version.find_first_of("\r\n"); nlPos != std::string::npos) {
                        version.erase(nlPos);
                    }
                    version          = utilxx_base::removeBetweenSpace(version);
                    bool validOutput = isValidPsVersion(version);
                    XX_LOGD(
                        "detectPowerShell probe {}: output='{}' version='{}' valid={}",
                        exeName,
                        output,
                        version,
                        validOutput
                    );
                    if (validOutput) {
                        info.available = true;
                        info.exeName   = exeName;
                        info.version   = version;
                        info.isPwsh    = isPwsh;
                        return true;
                    }
                }
                return false;
            },
            [&](std::string errmsg) -> bool {
                XX_LOGW("detectPowerShell exception: {}", errmsg);
                return false;
            }
        );
    }
    psInfo_ = info;
    return info;
}

#elif XX_IS_WIN_D

#include <windows.h>
#undef max
#undef min

static std::optional<std::string> systemName_;
/// 系统名缓存互斥 (探测可能被插件 offload 线程并发调用, 无锁读改写是数据竞争)
static std::mutex systemNameMutex;

std::string utilxx_base::getSystemName() {
    std::lock_guard<std::mutex> lk{systemNameMutex};
    if (systemName_.has_value()) {
        return *systemName_;
    }

    OSVERSIONINFOEXW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    HMODULE hNtDll           = GetModuleHandleW(L"ntdll.dll");
    if (hNtDll) {
        typedef LONG(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        auto RtlGetVersion = (RtlGetVersionPtr)GetProcAddress(hNtDll, "RtlGetVersion");
        if (RtlGetVersion && RtlGetVersion((PRTL_OSVERSIONINFOW)&info) == 0) {
            systemName_ = fmt::format(
                "Windows {}.{} (build {})",
                info.dwMajorVersion,
                info.dwMinorVersion,
                info.dwBuildNumber
            );
            return *systemName_;
        }
    }
    systemName_ = "Windows";
    return *systemName_;
}

bool utilxx_base::isRunningInWSL() {
    return false;
}

static std::optional<utilxx_base::PowerShellInfo> psInfo_;
/// PowerShell 探测缓存互斥 (见 Linux 分支同名注释)
static std::mutex psInfoMutex;

/// Windows 下探测 PowerShell: 直接运行 `exeName -NoProfile -NonInteractive -Command
/// '$PSVersionTable.PSVersion.ToString()'`, 读取 stdout 获取版本
/// - 用 _popen/_pclose 实现 (该函数仅在启动时调用一次, 阻塞可接受), 输出为空或非法即视为不可用
static std::string runPsVersionProbeWin(const char* exeName) {
    std::string cmd = fmt::format(
        "{} -NoProfile -NonInteractive -Command \"$PSVersionTable.PSVersion.ToString()\"",
        exeName
    );
    // 丢弃 stderr, 只取 stdout 版本输出
    cmd      += " 2>NUL";
    FILE* fp  = _popen(cmd.c_str(), "r");
    if (nullptr == fp) {
        return {};
    }
    std::string out;
    char        buf[512];
    while (fgets(buf, sizeof(buf), fp) != nullptr) {
        out.append(buf);
    }
    _pclose(fp);
    return out;
}

utilxx_base::PowerShellInfo utilxx_base::detectPowerShell(bool forceRefresh) {
    // 缓存 + 串行化 (见 Linux 分支同名注释)
    std::lock_guard<std::mutex> lk{psInfoMutex};
    if (psInfo_.has_value() && false == forceRefresh) {
        return *psInfo_;
    }
    auto info = utilxx_base::PowerShellInfo{};
    utilxx_base::catchError<bool>(
        [&]() -> bool {
            // 优先 pwsh.exe (PowerShell 7+), 再 powershell.exe (Windows PowerShell 5.1)
            static constexpr std::array<std::pair<const char*, bool>, 2> candidates{
                {{"pwsh.exe", true}, {"powershell.exe", false}},
            };
            for (const auto& [exeName, isPwsh] : candidates) {
                auto output  = runPsVersionProbeWin(exeName);
                auto version = utilxx_base::removeBetweenSpace(output);
                if (auto nlPos = version.find_first_of("\r\n"); nlPos != std::string::npos) {
                    version.erase(nlPos);
                }
                version          = utilxx_base::removeBetweenSpace(version);
                bool validOutput = isValidPsVersion(version);
                XX_LOGD(
                    "detectPowerShell probe {}: output='{}' version='{}' valid={}",
                    exeName,
                    output,
                    version,
                    validOutput
                );
                if (validOutput) {
                    info.available = true;
                    info.exeName   = exeName;
                    info.version   = version;
                    info.isPwsh    = isPwsh;
                    return true;
                }
            }
            return false;
        },
        [&](std::string errmsg) -> bool {
            XX_LOGW("detectPowerShell exception: {}", errmsg);
            return false;
        }
    );
    psInfo_ = info;
    return info;
}

#else

std::string utilxx_base::getSystemName() {
#if XX_IS_WIN_D
    return "Windows";
#elif XX_IS_LINUX_D
    return "Linux";
#elif XX_IS_MACOS_D
    return "macOS";
#elif XX_IS_ANDROID_D
    return "Android";
#elif XX_IS_IOS_D
    return "iOS";
#else
    return "Unknown";
#endif
}

bool utilxx_base::isRunningInWSL() {
    return false;
}

utilxx_base::PowerShellInfo utilxx_base::detectPowerShell(bool /*forceRefresh*/) {
    // 其他平台 (macOS/Android/iOS 等): 不探测 Windows PowerShell
    return utilxx_base::PowerShellInfo{};
}

#endif

// =====================================================================
// 文件异步 I/O 可用性 (asio::stream_file 的文件异步读写是否有效)
// =====================================================================

namespace {

/// 默认数据根目录名 (置于用户主目录下; 取不到主目录时置于系统临时目录下)
constexpr std::string_view kDefaultDataDirName = ".agentxx";

/// 自动探测结果缓存: 0 未探测 / 1 可用 / 2 不可用
/// - 按进程只探测一次, 之后直接返回缓存值; 原子量用于多线程/多协程并发读取
std::atomic<int> asyncFileIoProbeState{0};

/// 强制设置值: -1 未设置 / 0 强制不可用 / 1 强制可用 (优先于探测结果)
std::atomic<int> asyncFileIoOverrideState{-1};

#if (XX_IS_LINUX_D || XX_IS_ANDROID_D) && (defined(ASIO_HAS_FILE) || defined(BOOST_ASIO_HAS_FILE)) \
    && (defined(ASIO_HAS_IO_URING) || defined(BOOST_ASIO_HAS_IO_URING))
/// 读取 /proc/self/status 的 `Seccomp` 字段 (仅探测失败时用于日志定位, 失败返回 -1)
/// - 0: 未启用 seccomp; 1: SECCOMP_MODE_STRICT; 2: SECCOMP_MODE_FILTER
int readSeccompMode() {
    std::ifstream status{"/proc/self/status"};
    if (false == status.is_open()) {
        return -1;
    }
    for (std::string line; std::getline(status, line);) {
        if (line.rfind("Seccomp:", 0) != 0) {
            continue;
        }
        int mode = -1;
        if (std::sscanf(line.c_str(), "Seccomp: %d", &mode) != 1) {
            return -1;
        }
        return mode;
    }
    return -1;
}
#endif

/// 探测文件异步 I/O 是否可用 (由 [isAsyncFileIoSupported] 首次调用时触发, 只跑一次)
bool probeAsyncFileIoSupported() {
#if !(defined(ASIO_HAS_FILE) || defined(BOOST_ASIO_HAS_FILE))
    // 编译期未启用 asio 文件 I/O (该平台无 io_uring/IOCP 文件句柄支持), 直接用同步实现
    XX_LOGD("Async file io unsupported: `ASIO_HAS_FILE`/`BOOST_ASIO_HAS_FILE` not defined");
    return false;
#elif XX_IS_LINUX_D || XX_IS_ANDROID_D
#if defined(ASIO_HAS_IO_URING) || defined(BOOST_ASIO_HAS_IO_URING)
    /// Linux/Android: asio 的文件异步 I/O 由 io_uring 提供, 编译期可用不代表运行
    /// 环境可用 —— 容器/虚拟化的 seccomp 过滤 (Seccomp: 2) 会拦截 `io_uring_setup`,
    /// 内核过旧时该调用返回 ENOSYS, 故实际创建一个 io_uring 环来确认
    io_uring ring{};
    int      initRet     = io_uring_queue_init(2, &ring, 0);
    int      seccompMode = readSeccompMode();
    if (initRet < 0) {
        XX_LOGW(
            "Async file io unsupported: io_uring_queue_init failed, errno={} ({}) seccomp={}",
            -initRet,
            std::strerror(-initRet),
            seccompMode
        );
        return false;
    }
    io_uring_queue_exit(&ring);
    XX_LOGD("Async file io supported: io_uring ring created, seccomp={}", seccompMode);
    return true;
#else
    // Linux 上 asio 的文件 I/O 只能由 io_uring 提供: 该宏缺失即编译期未启用
    XX_LOGW("Async file io unsupported: built without io_uring (`ASIO_HAS_IO_URING` undefined)");
    return false;
#endif
#else
    // 其他平台 (Windows: IOCP + 随机访问句柄): 编译期宏已确认文件异步 I/O 可用
    XX_LOGD("Async file io supported: provided by platform native async file handle");
    return true;
#endif
}

} // namespace

bool utilxx_base::isAsyncFileIoSupported() {
    // 强制设置优先 (供测试关闭异步路径, 覆盖同步兜底实现)
    int overrideState = asyncFileIoOverrideState.load(std::memory_order_acquire);
    if (overrideState >= 0) {
        return overrideState == 1;
    }

    int probeState = asyncFileIoProbeState.load(std::memory_order_acquire);
    if (probeState != 0) {
        return probeState == 1;
    }

    // 首次探测: 并发调用时可能各探测一次, 结果一致且代价仅一次系统调用, 故不加锁
    bool supported = probeAsyncFileIoSupported();
    asyncFileIoProbeState.store(supported ? 1 : 2, std::memory_order_release);
    return supported;
}

void utilxx_base::setAsyncFileIoSupported(bool supported) {
    asyncFileIoOverrideState.store(supported ? 1 : 0, std::memory_order_release);
}

void utilxx_base::resetAsyncFileIoSupported() {
    asyncFileIoOverrideState.store(-1, std::memory_order_release);
}

// =====================================================================
// 平台惯例数据目录 (宿主数据存放位置的默认约定)
// =====================================================================

std::string utilxx_base::userHomeDir() {
#if XX_IS_WIN_D
    auto home = ApplicationEnv::instance().get("USERPROFILE");
#else
    auto home = ApplicationEnv::instance().get("HOME");
#endif
    return (home && !home->empty()) ? *home : std::string{};
}

std::string utilxx_base::defaultDataDir() {
    auto home = userHomeDir();
    if (!home.empty()) {
        return (std::filesystem::path(home) / kDefaultDataDirName).string();
    }
    // 取不到用户主目录 (极简容器等): 回退系统临时目录下的同名目录
    std::error_code ec;
    auto            tmp = std::filesystem::temp_directory_path(ec);
    return (tmp / kDefaultDataDirName).string();
}

std::string utilxx_base::getGlobalSettingsDbPath(std::string_view dataDir) {
    std::string dir;
    if (dataDir.empty()) {
        dir = defaultDataDir();
    } else {
        dir = std::string{dataDir};
        // 相对路径按当前工作目录绝对化并词法规范化 (避免拼接出不稳定的路径)
        if (!std::filesystem::path(dir).is_absolute()) {
            std::error_code ec;
            auto            abs = std::filesystem::absolute(dir, ec);
            if (!ec) {
                dir = abs.lexically_normal().generic_string();
            }
        } else {
            dir = std::filesystem::path(dir).lexically_normal().generic_string();
        }
    }
    return (std::filesystem::path(dir) / "sqlite" / "global.db").string();
}
