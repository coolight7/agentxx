#include "agentxx/util/util.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

namespace {

/// 去掉首尾空白符 (\r\n\t 等)
std::string trimWhitespace(std::string_view sv) {
    auto begin = sv.find_first_not_of(" \t\r\n\v\f");
    if (begin == std::string_view::npos) {
        return {};
    }
    auto end = sv.find_last_not_of(" \t\r\n\v\f");
    return std::string{sv.substr(begin, end - begin + 1)};
}

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
static std::optional<agentxx::util::PowerShellInfo> psInfo_;

std::string agentxx::util::getSystemName() {
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
    systemName_ = name;
    if (!name.empty()) {
        return name;
    }

    // 备选：uname 系统调用
    struct utsname buf;
    if (uname(&buf) == 0) {
        systemName_ = fmt::format("{} {}", buf.sysname, buf.release);
        return *systemName_;
    }
    systemName_ = "Linux";
    return *systemName_;
}

bool agentxx::util::isRunningInWSL() {
    // catchError: 文件系统探测失败按非 WSL 处理, 并记录日志
    return agentxx::util::catchError<bool>(
        [&]() -> bool {
            if (isRunningInWSL_.has_value()) {
                return *isRunningInWSL_;
            }
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

agentxx::util::PowerShellInfo agentxx::util::detectPowerShell(bool forceRefresh) {
    if (psInfo_.has_value() && false == forceRefresh) {
        return *psInfo_;
    }
    auto info = agentxx::util::PowerShellInfo{};
    // 仅 WSL 支持经 interop 调用 Windows exe; 非 WSL Linux 无 PowerShell 可探测
    if (agentxx::util::isRunningInWSL()) {
        // catchError: 探测异常按未找到处理
        agentxx::util::catchError<bool>(
            [&]() -> bool {
                // 优先 pwsh.exe (PowerShell 7+, 输出默认 UTF-8 更友好), 再 powershell.exe
                static constexpr std::array<std::pair<const char*, bool>, 2> candidates{
                    {{"pwsh.exe", true}, {"powershell.exe", false}},
                };
                for (const auto& [exeName, isPwsh] : candidates) {
                    // 探测超时给足裕量: 首次运行 powershell.exe 可能较慢
                    auto output  = runPsVersionProbe(exeName, 12000);
                    auto version = trimWhitespace(output);
                    // 版本可能带 BOM/多余内容, 只取第一行
                    if (auto nlPos = version.find_first_of("\r\n"); nlPos != std::string::npos) {
                        version.erase(nlPos);
                    }
                    version          = trimWhitespace(version);
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

std::string agentxx::util::getSystemName() {
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

bool agentxx::util::isRunningInWSL() {
    return false;
}

static std::optional<agentxx::util::PowerShellInfo> psInfo_;

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

agentxx::util::PowerShellInfo agentxx::util::detectPowerShell(bool forceRefresh) {
    if (psInfo_.has_value() && false == forceRefresh) {
        return *psInfo_;
    }
    auto info = agentxx::util::PowerShellInfo{};
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            // 优先 pwsh.exe (PowerShell 7+), 再 powershell.exe (Windows PowerShell 5.1)
            static constexpr std::array<std::pair<const char*, bool>, 2> candidates{
                {{"pwsh.exe", true}, {"powershell.exe", false}},
            };
            for (const auto& [exeName, isPwsh] : candidates) {
                auto output  = runPsVersionProbeWin(exeName);
                auto version = trimWhitespace(output);
                if (auto nlPos = version.find_first_of("\r\n"); nlPos != std::string::npos) {
                    version.erase(nlPos);
                }
                version          = trimWhitespace(version);
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

std::string agentxx::util::getSystemName() {
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

bool agentxx::util::isRunningInWSL() {
    return false;
}

agentxx::util::PowerShellInfo agentxx::util::detectPowerShell(bool /*forceRefresh*/) {
    // 其他平台 (macOS/Android/iOS 等): 不探测 Windows PowerShell
    return agentxx::util::PowerShellInfo{};
}

#endif
