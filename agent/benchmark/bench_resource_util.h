#pragma once

// 资源基准测试公共工具:
// - 进程内存/CPU 采样 (见 bench_mem_probe.h) 与各容器字节估算
// - 子进程启动/管理 (支持伪终端 PTY, 用于真实运行 TUI 子进程)
// - 固定消息模板与 token 校准 (保证各模式负载一致, 结果可横向对比)
// - 真实进程 (agentxx_cli server/tui) 的 yaml 配置生成 (新列表段结构)

#include "bench_mem_probe.h"

#include "agentxx/agent/config.h"

#include "agentxx/agent/conversation_types.h"
#include "agentxx/middlewares/summarization.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/ip/tcp.hpp"
#include "fmt/format.h"
#include "neograph/types.h"
#include "utilxx/http_server.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if XX_IS_WIN_D
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <psapi.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace agentxx {
namespace bench {

// ---------------------------------------------------------------------------
// 1. 内存 / CPU 采样: 由 bench_mem_probe.h 提供
// ---------------------------------------------------------------------------
//
// 兼容旧调用点的别名 (采样结果与函数名)
using ProcMemSample = ProcMemDetail;

inline ProcMemSample sampleProcessMemory(uint32_t pid = 0) {
    return sampleProcMemDetail(pid);
}

inline ProcMemSample sampleMemoryMedian(uint32_t pid = 0, size_t times = 3, int intervalMs = 50) {
    return sampleProcMemDetailMedian(pid, times, intervalMs);
}

using CpuWindow = CpuWindowDetail;

inline CpuWindow cpuBegin(uint32_t pid = 0) {
    return cpuBeginDetail(pid);
}

inline double cpuEnd(const CpuWindow& win) {
    return cpuEndDetailByWindow(win).busyPct;
}

// ---------------------------------------------------------------------------
// 2. 子进程启动与管理 (支持伪终端, 用于真实 TUI 子进程)
// ---------------------------------------------------------------------------

struct ProcessHandle {
#if XX_IS_WIN_D
    HANDLE hProcess    = nullptr;
    HANDLE hThread     = nullptr;
    HANDLE hStdinWrite = nullptr;
    DWORD  pid         = 0;
#else
    pid_t pid          = 0;
    int   stdinWriteFd = -1;
    /// 伪终端主端 (usePty 时产物): 既接收子进程输出, 又向其发送按键输入
    int ptyMasterFd = -1;
#endif
    bool        running  = false;
    bool        isPty    = false;
    size_t      ptyBytes = 0; ///< 累计从伪终端读走的字节数 (渲染输出量)
    std::string outputFile;
};

struct SpawnOptions {
    std::string workingDir;
    /// 追加/覆盖的环境变量 (KEY, VALUE; 子进程继承父进程环境后再应用这些)
    std::vector<std::pair<std::string, std::string>> env;
    /// POSIX: 为子进程分配伪终端 (真实 TUI 需要终端才能进入 raw 模式/全屏)
    /// - Windows 不支持: 自动退化为管道重定向 (报告会标注)
    bool usePty = false;
    /// 非 PTY 时把子进程 stdout/stderr 重定向到文件 (空 = /dev/null)
    std::string outputRedirect;
    /// true: 创建 stdin 管道 (经 writeToChildStdin 写入)
    bool createStdinPipe = true;
    /// PTY 窗口尺寸 (列 x 行), 仅 usePty 时生效; 0 = 使用默认 200x50
    int ptyCols = 200;
    int ptyRows = 50;
};

#if !XX_IS_WIN_D

/// 启动前应用环境变量 (仅子进程分支调用)
inline void applyChildEnv(const std::vector<std::pair<std::string, std::string>>& env) {
    for (const auto& kv : env) {
        ::setenv(kv.first.c_str(), kv.second.c_str(), 1);
    }
}

inline ProcessHandle spawnChildProcess(
    const std::string&              exePath,
    const std::vector<std::string>& args,
    const SpawnOptions&             opts = {}
) {
    ProcessHandle ph;
    ph.outputFile = opts.outputRedirect;

    int ptyMaster = -1;
    int ptySlave  = -1;
    int pfd[2]    = {-1, -1};

    if (opts.usePty) {
        ptyMaster = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (ptyMaster >= 0 && ::grantpt(ptyMaster) == 0 && ::unlockpt(ptyMaster) == 0) {
            char* slaveName = ::ptsname(ptyMaster);
            if (slaveName != nullptr) {
                ptySlave = ::open(slaveName, O_RDWR | O_NOCTTY);
            }

            struct winsize ws {};

            ws.ws_col = static_cast<unsigned short>(opts.ptyCols > 0 ? opts.ptyCols : 200);
            ws.ws_row = static_cast<unsigned short>(opts.ptyRows > 0 ? opts.ptyRows : 50);
            ::ioctl(ptyMaster, TIOCSWINSZ, &ws);
        }
        if (ptySlave < 0) {
            // PTY 不可用: 关闭主端并退化为管道模式
            if (ptyMaster >= 0) {
                ::close(ptyMaster);
                ptyMaster = -1;
            }
        }
    }
    if (ptyMaster < 0 && opts.createStdinPipe) {
        if (::pipe(pfd) != 0) {
            pfd[0] = pfd[1] = -1;
        }
    }

    pid_t p = ::fork();
    if (p == 0) {
        // ---- 子进程分支: 仅调用简单系统调用 ----
        applyChildEnv(opts.env);
        if (!opts.workingDir.empty()) {
            if (::chdir(opts.workingDir.c_str()) != 0) {
                // 忽略: 目录不可用时沿用父进程 cwd
            }
        }
        if (ptySlave >= 0) {
            // 建立新会话并把 PTY 从端设为控制终端 (TUI 需要 ioctl/raw 模式)
            ::setsid();
            ::ioctl(ptySlave, TIOCSCTTY, 0);
            ::dup2(ptySlave, STDIN_FILENO);
            ::dup2(ptySlave, STDOUT_FILENO);
            ::dup2(ptySlave, STDERR_FILENO);
            if (ptySlave > STDERR_FILENO) {
                ::close(ptySlave);
            }
            if (ptyMaster >= 0) {
                ::close(ptyMaster);
            }
        } else {
            if (pfd[0] >= 0) {
                ::dup2(pfd[0], STDIN_FILENO);
                ::close(pfd[0]);
                if (pfd[1] >= 0) {
                    ::close(pfd[1]);
                }
            } else {
                int devNull = ::open("/dev/null", O_RDONLY);
                if (devNull >= 0) {
                    ::dup2(devNull, STDIN_FILENO);
                    ::close(devNull);
                }
            }
            int outFd = -1;
            if (!opts.outputRedirect.empty()) {
                outFd = ::open(opts.outputRedirect.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            }
            if (outFd < 0) {
                outFd = ::open("/dev/null", O_WRONLY);
            }
            if (outFd >= 0) {
                ::dup2(outFd, STDOUT_FILENO);
                ::dup2(outFd, STDERR_FILENO);
                if (outFd > STDERR_FILENO) {
                    ::close(outFd);
                }
            }
        }

        std::vector<char*> cargs;
        cargs.reserve(args.size() + 2);
        cargs.push_back(const_cast<char*>(exePath.c_str()));
        for (const auto& a : args) {
            cargs.push_back(const_cast<char*>(a.c_str()));
        }
        cargs.push_back(nullptr);
        ::execvp(exePath.c_str(), cargs.data());
        ::_exit(127);
    }

    // ---- 父进程分支 ----
    if (p > 0) {
        ph.pid     = p;
        ph.running = true;
        if (ptySlave >= 0) {
            ::close(ptySlave);
        }
        if (ptyMaster >= 0 && ptySlave >= 0) {
            ph.ptyMasterFd = ptyMaster;
            ph.isPty       = true;
            // 非阻塞: 便于按需排空输出, 不阻塞基准测试线程
            int flags = ::fcntl(ptyMaster, F_GETFL, 0);
            if (flags >= 0) {
                ::fcntl(ptyMaster, F_SETFL, flags | O_NONBLOCK);
            }
        } else if (ptyMaster >= 0) {
            ::close(ptyMaster);
        }
        if (pfd[0] >= 0) {
            ::close(pfd[0]);
        }
        if (ptyMaster < 0 && pfd[1] >= 0) {
            ph.stdinWriteFd = pfd[1];
        }
    } else {
        // fork 失败: 清理已创建的描述符
        if (ptyMaster >= 0) {
            ::close(ptyMaster);
        }
        if (ptySlave >= 0) {
            ::close(ptySlave);
        }
        if (pfd[0] >= 0) {
            ::close(pfd[0]);
        }
        if (pfd[1] >= 0) {
            ::close(pfd[1]);
        }
    }
    return ph;
}

#else // ------------------------------- Windows -------------------------------

inline ProcessHandle spawnChildProcess(
    const std::string&              exePath,
    const std::vector<std::string>& args,
    const SpawnOptions&             opts = {}
) {
    ProcessHandle ph;
    ph.outputFile = opts.outputRedirect;

    HANDLE hStdinRead  = nullptr;
    HANDLE hStdinWrite = nullptr;
    if (opts.createStdinPipe) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;
        if (::CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0)) {
            ::SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);
            ph.hStdinWrite = hStdinWrite;
        }
    }

    std::string cmd = "\"" + exePath + "\"";
    for (const auto& a : args) {
        cmd += " \"" + a + "\"";
    }

    // 环境块: 继承父进程环境后追加覆盖项
    std::string envBlock;
    auto        appendEnv = [&envBlock](std::string_view kv) {
        envBlock.append(kv);
        envBlock.push_back('\0');
    };
    {
        LPCH parentEnv = ::GetEnvironmentStringsA();
        if (parentEnv != nullptr) {
            for (LPSTR cur = parentEnv; *cur != '\0'; cur += std::strlen(cur) + 1) {
                std::string entry(cur);
                bool        overridden = false;
                for (const auto& kv : opts.env) {
                    auto prefix = kv.first + "=";
                    if (entry.rfind(prefix, 0) == 0) {
                        overridden = true;
                        break;
                    }
                }
                if (!overridden) {
                    appendEnv(entry);
                }
            }
            ::FreeEnvironmentStringsA(parentEnv);
        }
    }
    for (const auto& kv : opts.env) {
        appendEnv(kv.first + "=" + kv.second);
    }
    appendEnv(std::string{});

    STARTUPINFOA si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = (hStdinRead != nullptr) ? hStdinRead : ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = INVALID_HANDLE_VALUE;
    si.hStdError  = INVALID_HANDLE_VALUE;

    HANDLE hOut = nullptr;
    if (!opts.outputRedirect.empty()) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;
        hOut              = ::CreateFileA(
            opts.outputRedirect.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &sa,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (hOut != INVALID_HANDLE_VALUE) {
            si.hStdOutput = hOut;
            si.hStdError  = hOut;
        }
    }
    if (si.hStdOutput == INVALID_HANDLE_VALUE) {
        HANDLE hNull = ::CreateFileA(
            "NUL",
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        si.hStdOutput = hNull;
        si.hStdError  = hNull;
    }

    PROCESS_INFORMATION pi{};
    BOOL                ok = ::CreateProcessA(
        nullptr,
        cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        envBlock.empty() ? nullptr : envBlock.data(),
        opts.workingDir.empty() ? nullptr : opts.workingDir.c_str(),
        &si,
        &pi
    );
    if (hStdinRead != nullptr) {
        ::CloseHandle(hStdinRead);
    }
    if (hOut != nullptr && hOut != INVALID_HANDLE_VALUE) {
        ::CloseHandle(hOut);
    }
    if (si.hStdOutput != INVALID_HANDLE_VALUE && si.hStdOutput != hOut) {
        ::CloseHandle(si.hStdOutput);
    }
    if (ok) {
        ph.hProcess = pi.hProcess;
        ph.hThread  = pi.hThread;
        ph.pid      = pi.dwProcessId;
        ph.running  = true;
    }
    return ph;
}

#endif

/// 子进程当前时间点是否存活 (顺带回收已退出的子进程)
inline bool childProcessAlive(ProcessHandle& ph) {
#if XX_IS_WIN_D
    if (!ph.running || ph.hProcess == nullptr) {
        return false;
    }
    DWORD code = 0;
    if (!::GetExitCodeProcess(ph.hProcess, &code)) {
        return false;
    }
    if (code == STILL_ACTIVE) {
        return true;
    }
    ph.running = false;
    return false;
#else
    if (!ph.running || ph.pid <= 0) {
        return false;
    }
    int   status = 0;
    pid_t res    = ::waitpid(ph.pid, &status, WNOHANG);
    if (res == 0) {
        return true;
    }
    ph.running = false;
    return false;
#endif
}

/// 阻塞等待子进程退出 (超时返回 false)
inline bool waitChildProcess(ProcessHandle& ph, int timeoutMs = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
#if XX_IS_WIN_D
    if (!ph.running || ph.hProcess == nullptr) {
        return true;
    }
    DWORD waitMs = static_cast<DWORD>(timeoutMs < 0 ? 0 : timeoutMs);
    DWORD res    = ::WaitForSingleObject(ph.hProcess, waitMs);
    if (res == WAIT_OBJECT_0) {
        ph.running = false;
        return true;
    }
    return false;
#else
    while (std::chrono::steady_clock::now() < deadline) {
        int   status = 0;
        pid_t res    = ::waitpid(ph.pid, &status, WNOHANG);
        if (res != 0) {
            ph.running = false;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
#endif
}

/// 终止子进程 (先 SIGTERM, 宽限后 SIGKILL)
inline void stopChildProcess(ProcessHandle& ph) {
    if (!ph.running) {
#if !XX_IS_WIN_D
        if (ph.stdinWriteFd >= 0) {
            ::close(ph.stdinWriteFd);
            ph.stdinWriteFd = -1;
        }
        if (ph.ptyMasterFd >= 0) {
            ::close(ph.ptyMasterFd);
            ph.ptyMasterFd = -1;
        }
#endif
        return;
    }
#if XX_IS_WIN_D
    if (ph.hStdinWrite) {
        ::CloseHandle(ph.hStdinWrite);
        ph.hStdinWrite = nullptr;
    }
    if (ph.hProcess) {
        ::TerminateProcess(ph.hProcess, 0);
        ::WaitForSingleObject(ph.hProcess, 3000);
        ::CloseHandle(ph.hProcess);
        ::CloseHandle(ph.hThread);
        ph.hProcess = nullptr;
        ph.hThread  = nullptr;
    }
#else
    if (ph.stdinWriteFd >= 0) {
        ::close(ph.stdinWriteFd);
        ph.stdinWriteFd = -1;
    }
    if (ph.pid > 0) {
        ::kill(ph.pid, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            int   status = 0;
            pid_t res    = ::waitpid(ph.pid, &status, WNOHANG);
            if (res != 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ::kill(ph.pid, SIGKILL);
        int status = 0;
        ::waitpid(ph.pid, &status, WNOHANG);
    }
    if (ph.ptyMasterFd >= 0) {
        ::close(ph.ptyMasterFd);
        ph.ptyMasterFd = -1;
    }
#endif
    ph.running = false;
}

/// 向子进程写入输入 (PTY 为模拟键盘输入: 普通文本直接写, 回车用 '\r')
inline bool writeToChildStdin(ProcessHandle& ph, std::string_view text) {
#if XX_IS_WIN_D
    if (!ph.hStdinWrite || text.empty()) {
        return false;
    }
    DWORD written = 0;
    return ::WriteFile(
               ph.hStdinWrite,
               text.data(),
               static_cast<DWORD>(text.size()),
               &written,
               nullptr
           )
           && written == text.size();
#else
    int fd = (ph.ptyMasterFd >= 0) ? ph.ptyMasterFd : ph.stdinWriteFd;
    if (fd < 0 || text.empty()) {
        return false;
    }
    size_t off = 0;
    while (off < text.size()) {
        ssize_t w = ::write(fd, text.data() + off, text.size() - off);
        if (w <= 0) {
            return false;
        }
        off += static_cast<size_t>(w);
    }
    return true;
#endif
}

/// 排空子进程伪终端输出 (非阻塞; 返回本次读走的字节数)
/// - TUI 每帧渲染输出量大, 不排空会让子进程阻塞在写操作上
/// - sink 非空时把输出内容追加进去 (便于统计渲染字节/排查问题)
inline size_t
    drainChildPtyOutput(ProcessHandle& ph, std::string* sink = nullptr, size_t maxKeep = 1 << 20) {
#if XX_IS_WIN_D
    (void)ph;
    (void)sink;
    (void)maxKeep;
    return 0;
#else
    if (ph.ptyMasterFd < 0) {
        return 0;
    }
    char   buf[8192];
    size_t total = 0;
    for (;;) {
        ssize_t n = ::read(ph.ptyMasterFd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        total += static_cast<size_t>(n);
        if (sink != nullptr && sink->size() < maxKeep) {
            sink->append(buf, static_cast<size_t>(n));
        }
    }
    ph.ptyBytes += total;
    return total;
#endif
}

/// 轮询等待 TCP 端口可连接 (返回是否在超时前连上)
inline bool waitForTcpPort(std::string_view host, uint16_t port, int timeoutMs = 15000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        asio::io_context           testCtx;
        asio::ip::tcp::socket      sock(testCtx);
        utilxx_base::AsioErrorCode ec;
        sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address(std::string{host}), port), ec);
        if (!ec) {
            sock.close();
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

/// 申请一个空闲本地 TCP 端口 (bind 0 后立刻释放)
inline uint16_t findFreeTcpPort() {
    asio::io_context           ctx;
    asio::ip::tcp::acceptor    acceptor(ctx);
    asio::ip::tcp::endpoint    ep(asio::ip::make_address("127.0.0.1"), 0);
    utilxx_base::AsioErrorCode ec;
    acceptor.open(ep.protocol(), ec);
    if (ec) {
        return 0;
    }
    acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
    acceptor.bind(ep, ec);
    if (ec) {
        return 0;
    }
    uint16_t port = acceptor.local_endpoint(ec).port();
    acceptor.close(ec);
    return port;
}

// ---------------------------------------------------------------------------
// 3. 插件路径探测与 5 常用插件列表
// ---------------------------------------------------------------------------

inline const std::vector<std::string>& getBench5PluginNames() {
    static const std::vector<std::string> kNames = {
        "agentxx_filesystem",
        "agentxx_execute_command",
        "agentxx_system",
        "agentxx_websearch",
        "agentxx_planning",
    };
    return kNames;
}

/// 可执行文件所在目录 (取不到时返回当前目录)
inline std::filesystem::path executableDir() {
    namespace fs = std::filesystem;
    std::error_code ec;
#if XX_IS_WIN_D
    wchar_t buf[MAX_PATH];
    if (::GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0) {
        return fs::path(buf).parent_path();
    }
#else
    if (auto p = fs::read_symlink("/proc/self/exe", ec); !ec) {
        return p.parent_path();
    }
#endif
    return fs::current_path(ec);
}

/// 定位插件目录 (优先可执行同目录的 plugins/<name>, 其次 cwd/plugins/<name>)
inline std::string resolveBenchPluginDir(const std::string& pluginName) {
    namespace fs = std::filesystem;
    std::error_code       ec;
    std::vector<fs::path> candidates;

    candidates.push_back(executableDir() / "plugins" / pluginName);
    candidates.push_back(fs::current_path(ec) / "plugins" / pluginName);
    candidates.push_back(fs::current_path(ec) / "exec" / "plugins" / pluginName);
    candidates.push_back(
        fs::current_path(ec) / "agent" / "build" / "linux-release" / "exec" / "plugins" / pluginName
    );
    candidates.push_back(
        fs::current_path(ec) / "agent" / "build" / "linux-debug" / "exec" / "plugins" / pluginName
    );

    auto hasLibFile = [](const fs::path& dir) {
        std::error_code                     ec2;
        std::filesystem::directory_iterator it(dir, ec2);
        std::filesystem::directory_iterator end;
        for (; it != end; it.increment(ec2)) {
            auto ext = it->path().extension().string();
            if (ext == ".so" || ext == ".dll" || ext == ".dylib") {
                return true;
            }
        }
        return false;
    };

    for (const auto& c : candidates) {
        if (fs::is_directory(c, ec) && hasLibFile(c)) {
            return c.string();
        }
    }
    return "builtin://" + pluginName;
}

/// 查找 libagentxx 共享库绝对路径 (供 FFI dlopen 使用)
inline std::string findSharedLibPath() {
    namespace fs = std::filesystem;
    std::error_code       ec;
    std::vector<fs::path> candidates;

#if XX_IS_WIN_D
    const std::vector<std::string> libNames = {"libagentxx.dll", "agentxx.dll"};
#else
    const std::vector<std::string> libNames = {"libagentxx.so", "libagentxxd.so"};
#endif
    auto                  exeDir = executableDir();
    auto                  cwd    = fs::current_path(ec);
    std::vector<fs::path> dirs
        = {exeDir,
           cwd,
           cwd / "exec",
           cwd / "agent" / "build" / "linux-release" / "exec",
           cwd / "agent" / "build" / "linux-debug" / "exec"};
    for (const auto& dir : dirs) {
        for (const auto& name : libNames) {
            candidates.push_back(dir / name);
        }
    }

    for (const auto& c : candidates) {
        if (fs::is_regular_file(c, ec)) {
            return c.string();
        }
    }
    return "";
}

/// 定位 agentxx_cli 可执行文件 (拆分模式需要真实进程)
inline std::string findAgentxxCliPath() {
    namespace fs = std::filesystem;
    std::error_code ec;
#if XX_IS_WIN_D
    const std::string exeName = "agentxx_cli.exe";
#else
    const std::string exeName = "agentxx_cli";
#endif
    auto                  exeDir     = executableDir();
    auto                  cwd        = fs::current_path(ec);
    std::vector<fs::path> candidates = {
        exeDir / exeName,
        cwd / exeName,
        cwd / "exec" / exeName,
        cwd / "agent" / "build" / "linux-release" / "exec" / exeName,
        cwd / "agent" / "build" / "linux-debug" / "exec" / exeName,
    };

    for (const auto& c : candidates) {
        if (fs::is_regular_file(c, ec)) {
            return c.string();
        }
    }
    return "";
}

// ---------------------------------------------------------------------------
// 4. 真实进程 (agentxx_cli) 的 yaml 配置生成
// ---------------------------------------------------------------------------

struct RealRunConfigOptions {
    std::string dataDir;    ///< 数据目录 (sqlite 会话库/全局设置)
    std::string workDir;    ///< 会话工作目录
    std::string llmBaseUrl; ///< mock LLM 地址 (http://127.0.0.1:<port>/v1)
    std::string modelName       = "bench-sim";
    size_t      contextMaxToken = 8 << 20; ///< 极大上限, 避免触发上下文压缩
    bool        enableSubagent  = false;
    bool        enableWorktree  = false;
    std::vector<std::string> pluginDirs; ///< 空 = 5 常用插件 (目录名)
};

/// 插件条目的 yaml 路径 (支持内置名/绝对目录/任意路径)
inline std::string
    resolvePluginPathForYaml(const std::string& entry, std::string_view pluginsRoot) {
    if (entry.rfind("builtin://", 0) == 0 || entry.rfind("/", 0) == 0
        || entry.find(':') != std::string::npos) {
        return entry; // 已是内置名或绝对路径
    }
    if (!pluginsRoot.empty()) {
        return std::string{pluginsRoot} + "/" + entry;
    }
    return resolveBenchPluginDir(entry);
}

/// 生成 agent (server 模式) 配置文件内容 (列表段使用新结构 `list:` / `use:`)
inline std::string
    buildAgentYamlConfig(const RealRunConfigOptions& opt, std::string_view pluginsRootDir = {}) {
    std::ostringstream ofs;
    ofs << "# 由 agentxx_benchmark 生成的资源基准测试配置 (server)\n";
    ofs << "data_dir: " << opt.dataDir << "\n";
    ofs << "work_dir: " << opt.workDir << "\n";
    ofs << "permission:\n  mode: pass\n";
    ofs << "subagent:\n  enable: " << (opt.enableSubagent ? "true" : "false") << "\n";
    ofs << "worktree:\n  enable: " << (opt.enableWorktree ? "true" : "false") << "\n";
    ofs << "model:\n  list:\n";
    ofs << "    - name: " << opt.modelName << "\n";
    ofs << "      type: openai\n";
    ofs << "      base_url: " << opt.llmBaseUrl << "\n";
    ofs << "      api_key: EMPTY\n";
    ofs << "      model_name: " << opt.modelName << "\n";
    ofs << "      model_context_max_token: " << opt.contextMaxToken << "\n";
    ofs << "  use:\n";
    ofs << "    default: " << opt.modelName << "\n";
    ofs << "plugin:\n  list:\n";
    const auto& names = opt.pluginDirs.empty() ? getBench5PluginNames() : opt.pluginDirs;
    for (const auto& name : names) {
        ofs << "    - path: " << resolvePluginPathForYaml(name, pluginsRootDir)
            << "\n      enabled: true\n";
    }
    return ofs.str();
}

/// 生成 client (tui/cli 模式) 配置文件内容:
/// - 远程模式只需数据目录/插件 (模型由 server 提供); 仍附带模型段以便本地运行
inline std::string
    buildClientYamlConfig(const RealRunConfigOptions& opt, std::string_view pluginsRootDir = {}) {
    std::ostringstream ofs;
    ofs << "# 由 agentxx_benchmark 生成的资源基准测试配置 (client)\n";
    ofs << "data_dir: " << opt.dataDir << "\n";
    ofs << "work_dir: " << opt.workDir << "\n";
    ofs << "permission:\n  mode: pass\n";
    if (!opt.llmBaseUrl.empty()) {
        ofs << "model:\n  list:\n";
        ofs << "    - name: " << opt.modelName << "\n";
        ofs << "      type: openai\n";
        ofs << "      base_url: " << opt.llmBaseUrl << "\n";
        ofs << "      api_key: EMPTY\n";
        ofs << "      model_name: " << opt.modelName << "\n";
        ofs << "      model_context_max_token: " << opt.contextMaxToken << "\n";
        ofs << "  use:\n";
        ofs << "    default: " << opt.modelName << "\n";
    }
    ofs << "plugin:\n  list:\n";
    const auto& names = opt.pluginDirs.empty() ? getBench5PluginNames() : opt.pluginDirs;
    for (const auto& name : names) {
        ofs << "    - path: " << resolvePluginPathForYaml(name, pluginsRootDir)
            << "\n      enabled: true\n";
    }
    return ofs.str();
}

// ---------------------------------------------------------------------------
// 5. 固定消息模板 (user / assistant / tool 交替, 跨模式内容固定)
// ---------------------------------------------------------------------------

inline std::string getFixedToolResultPayload() {
    // 构造固定 3000B 包含中文与 ASCII 的载荷 (使单组约 2000 tokens, 50 组≈100K, 100 组≈200K)
    static const std::string kUnit
        = "RES-BENCH tool result fixed payload line: The quick brown fox jumps over the lazy dog. "
          "资源占用固定载荷，覆盖 unicode 折算分支与 ascii 折算分支。 "
          "Standard test vectors for memory and cpu benchmark verification. "
          "Fixed payload padding to reach deterministic byte length for reproducible measurements. "
          "1234567890!@#$%^&*()_+-=[]{}|;:,.<>?/`~ ";
    std::string out;
    out.reserve(3000);
    while (out.size() + kUnit.size() <= 3000) {
        out += kUnit;
    }
    if (out.size() < 3000) {
        out.append(3000 - out.size(), '=');
    }
    return out;
}

struct FixedGroup {
    neograph::ChatMessage userMsg;
    neograph::ChatMessage assistMsg;
    neograph::ChatMessage toolMsg;

    agent::ViewMessage viewUser;
    agent::ViewMessage viewTool;
    agent::ViewMessage viewAssist;
};

inline FixedGroup makeFixedGroup(size_t index) {
    FixedGroup  g;
    std::string idxStr      = fmt::format("{:06d}", index);
    std::string userContent = fmt::format(
        "RES-BENCH user turn {} | The quick brown fox jumps over the lazy dog. 请列出当前目录并读取 README 前 40 行。 #FIXED-9f3a",
        idxStr
    );
    std::string callId   = fmt::format("call-{}", idxStr);
    std::string toolArgs = "{\"path\":\"README.md\",\"line_offset\":0,\"line_limit\":40}";
    std::string toolContent
        = fmt::format("RES-BENCH tool result {} | {}", idxStr, getFixedToolResultPayload());
    std::string assistSummary = fmt::format(
        "RES-BENCH assist summary {} | 已成功读取 README.md 前 40 行内容，并完成分析任务。",
        idxStr
    );

    // 1. LLM 消息
    g.userMsg.role    = "user";
    g.userMsg.content = userContent;

    g.assistMsg.role    = "assistant";
    g.assistMsg.content = "";
    neograph::ToolCall tc;
    tc.id        = callId;
    tc.name      = "agentxx_filesystem_read";
    tc.arguments = toolArgs;
    g.assistMsg.tool_calls.push_back(std::move(tc));

    g.toolMsg.role         = "tool";
    g.toolMsg.tool_call_id = callId;
    g.toolMsg.tool_name    = "agentxx_filesystem_read";
    g.toolMsg.content      = toolContent;

    // 2. View 消息
    g.viewUser = agent::ViewMessage::makeText(agent::ViewMessage::Role::User, userContent);

    g.viewTool.role = agent::ViewMessage::Role::Tool;
    g.viewTool.text = toolArgs;
    agent::ViewMessage::ToolData td;
    td.toolName          = "agentxx_filesystem_read";
    td.toolCallId        = callId;
    td.toolResult        = toolContent;
    td.toolFinished      = true;
    g.viewTool.tool      = td;
    g.viewTool.collapsed = true;

    g.viewAssist = agent::ViewMessage::makeText(agent::ViewMessage::Role::Assistant, assistSummary);

    return g;
}

// ---------------------------------------------------------------------------
// 6. Token 计算与 100K/200K 组数校准
// ---------------------------------------------------------------------------

inline size_t countLlmTokens(const std::vector<neograph::ChatMessage>& msgs) {
    agentxx::middleware::SummarizationMiddlewareHandle handle(
        std::weak_ptr<agentxx::agent::AgentContext>{}
    );
    return handle.countTokens({}, msgs, false);
}

struct CalibratedCounts {
    size_t n100            = 0;
    size_t n200            = 0;
    size_t actualTokens100 = 0;
    size_t actualTokens200 = 0;
    size_t groupTokens     = 0;
};

inline const CalibratedCounts& getCalibratedCounts() {
    static CalibratedCounts counts = []() {
        CalibratedCounts                   c;
        auto                               g1       = makeFixedGroup(1);
        std::vector<neograph::ChatMessage> oneGroup = {g1.userMsg, g1.assistMsg, g1.toolMsg};
        c.groupTokens                               = countLlmTokens(oneGroup);
        if (c.groupTokens == 0) {
            c.groupTokens = 450; // 防除零
        }

        // 粗算组数
        size_t n1 = 100000 / c.groupTokens;
        size_t n2 = 200000 / c.groupTokens;

        // 微调 n100
        std::vector<neograph::ChatMessage> msgs100;
        msgs100.reserve(n1 * 3);
        for (size_t i = 0; i < n1; ++i) {
            auto g = makeFixedGroup(i + 1);
            msgs100.push_back(std::move(g.userMsg));
            msgs100.push_back(std::move(g.assistMsg));
            msgs100.push_back(std::move(g.toolMsg));
        }
        size_t tok1 = countLlmTokens(msgs100);
        while (tok1 < 98000) {
            ++n1;
            auto g = makeFixedGroup(n1);
            msgs100.push_back(std::move(g.userMsg));
            msgs100.push_back(std::move(g.assistMsg));
            msgs100.push_back(std::move(g.toolMsg));
            tok1 = countLlmTokens(msgs100);
        }
        c.n100            = n1;
        c.actualTokens100 = tok1;

        // 微调 n200
        std::vector<neograph::ChatMessage> msgs200 = msgs100;
        msgs200.reserve(n2 * 3);
        for (size_t i = n1; i < n2; ++i) {
            auto g = makeFixedGroup(i + 1);
            msgs200.push_back(std::move(g.userMsg));
            msgs200.push_back(std::move(g.assistMsg));
            msgs200.push_back(std::move(g.toolMsg));
        }
        size_t tok2 = countLlmTokens(msgs200);
        while (tok2 < 198000) {
            ++n2;
            auto g = makeFixedGroup(n2);
            msgs200.push_back(std::move(g.userMsg));
            msgs200.push_back(std::move(g.assistMsg));
            msgs200.push_back(std::move(g.toolMsg));
            tok2 = countLlmTokens(msgs200);
        }
        c.n200            = n2;
        c.actualTokens200 = tok2;

        return c;
    }();
    return counts;
}

// ---------------------------------------------------------------------------
// 7. 容器字节估算
// ---------------------------------------------------------------------------

/// 采样并填充 ResourceResult 的内存细项 (rssMB/privateMB 兼容字段同步更新)/// - includeModules:
/// 解析 smaps 生成模块级分解 (开销数毫秒, 建议只在采样点开启)
/// - includeTrim:    调用 malloc_trim(0) 测可回收量 (仅自身进程有效, 会改变内存状态)
inline void fillResourceMemDetail(
    ResourceResult& r,
    uint32_t        pid,
    bool            includeModules = true,
    bool            includeTrim    = false
) {
    r.mem             = sampleProcMemDetailMedian(pid, 3, 20);
    r.rssMB           = r.mem.rssMB;
    r.privateMB       = r.mem.privateMB;
    r.heapFragmentPct = heapFragmentPercent(r.mem);
    if (includeModules) {
        auto bd   = sampleModuleBreakdown(pid, 18);
        r.modules = std::move(bd.rows);
    }
    if (includeTrim) {
        r.trimReclaimableMB = trimReclaimableMB();
    }
}

inline size_t estimateLlmMessagesBytes(const neograph::json& llmMsgs) {
    if (!llmMsgs.is_array()) {
        return 0;
    }
    return llmMsgs.dump().size();
}

inline size_t estimateLlmMessagesBytes(const utilxx_base::Json& llmMsgs) {
    if (!llmMsgs.is_array()) {
        return 0;
    }
    return llmMsgs.dump().size();
}

// ---------------------------------------------------------------------------
// 8. 插件配置 / 主机信息 / 负载缩放
// ---------------------------------------------------------------------------

/// 5 常用插件的 agent 侧配置 (path 为探测到的插件目录)
inline std::vector<agent::PluginConfig> bench5PluginConfigs() {
    std::vector<agent::PluginConfig> out;
    for (const auto& name : getBench5PluginNames()) {
        agent::PluginConfig pc;
        pc.path    = resolveBenchPluginDir(name);
        pc.enabled = true;
        pc.sides   = agent::PluginSide::Auto;
        out.push_back(std::move(pc));
    }
    return out;
}

/// 采集运行环境信息 (写入报告头部, 便于跨机器/跨版本对比)
inline HostInfo collectHostInfo() {
    HostInfo info;
    info.system   = utilxx_base::getSystemName();
    info.exePath  = currentExecutablePath();
    info.cpuCores = std::thread::hardware_concurrency();
#ifdef AGENTXX_VERSION_STRING
    info.version = AGENTXX_VERSION_STRING;
#endif
#if XX_IS_RELEASE_D
    info.buildConfig = "Release";
#else
    info.buildConfig = "Debug";
#endif
#if XX_IS_WIN_D
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (::GlobalMemoryStatusEx(&ms)) {
        info.memTotalMB = static_cast<double>(ms.ullTotalPhys) / (1024.0 * 1024.0);
    }
#else
    {
        auto               meminfo = readWholeFile("/proc/meminfo");
        std::istringstream iss(meminfo);
        std::string        line;
        while (std::getline(iss, line)) {
            if (line.rfind("MemTotal:", 0) == 0) {
                std::istringstream vs(line.substr(9));
                double             kb = 0.0;
                vs >> kb;
                info.memTotalMB = kb / 1024.0;
                break;
            }
        }
    }
#endif
    return info;
}

/// 负载缩放系数 (环境变量 AGENTXX_BENCH_SCALE):
/// - 真实进程场景 (WS 轮次/伪终端打字) 每轮开销远大于进程内注入, 完整 100K/200K
///   需要数千轮, 故允许按比例缩小负载; 报告 note 会标注实际使用的缩放系数
/// - <1 时仅缩小"注入的固定组数", token 数同比例缩小 (便于横向对比相对增长)
inline double benchLoadScale() {
    if (auto v = utilxx_base::ApplicationEnv::instance().get("AGENTXX_BENCH_SCALE")) {
        double parsed = std::atof(v->c_str());
        if (parsed > 0.01 && parsed <= 1.0) {
            return parsed;
        }
    }
    return 1.0;
}

// ---------------------------------------------------------------------------
// 8. 会话 id / 临时目录
// ---------------------------------------------------------------------------

inline std::string generateBenchSessionId() {
    static std::atomic<uint64_t> seq{0};
    auto                         now = std::chrono::steady_clock::now().time_since_epoch().count();
    return fmt::format("bench_sess_{}_{}", now, seq.fetch_add(1));
}

inline std::filesystem::path createBenchTempDir(const std::string& prefix) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto            base = fs::temp_directory_path(ec);
    if (ec) {
        base = fs::current_path(ec);
    }
    static std::atomic<uint32_t> seq{0};
    auto                         name = fmt::format(
        "{}_{}_{}",
        prefix,
        static_cast<long>(
#if XX_IS_WIN_D
            ::GetCurrentProcessId()
#else
            getpid()
#endif
        ),
        seq.fetch_add(1)
    );
    fs::path dir = base / name;
    fs::create_directories(dir, ec);

    // 在临时目录下写入 README.md, 内容即固定 512B tool 结果载荷,
    // 保证后续 agentxx_filesystem_read 工具真实执行时结果与固定模板一致
    std::ofstream ofs(dir / "README.md", std::ios::binary | std::ios::trunc);
    if (ofs.is_open()) {
        auto payload = getFixedToolResultPayload();
        ofs.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        ofs.close();
    }
    return dir;
}

struct ResourceLlmSimServer {
    std::unique_ptr<utilxx::HttpServer>  svr;
    std::thread                          thr;
    uint16_t                             port        = 0;
    std::shared_ptr<std::atomic<size_t>> turnCounter = std::make_shared<std::atomic<size_t>>(0);

    ResourceLlmSimServer() = default;

    ResourceLlmSimServer(ResourceLlmSimServer&& o) noexcept :
        svr(std::move(o.svr)),
        thr(std::move(o.thr)),
        port(o.port),
        turnCounter(std::move(o.turnCounter)) {
        o.port = 0;
    }

    ResourceLlmSimServer& operator=(ResourceLlmSimServer&& o) noexcept {
        if (this != &o) {
            stop();
            svr         = std::move(o.svr);
            thr         = std::move(o.thr);
            port        = o.port;
            turnCounter = std::move(o.turnCounter);
            o.port      = 0;
        }
        return *this;
    }

    ResourceLlmSimServer(const ResourceLlmSimServer&)            = delete;
    ResourceLlmSimServer& operator=(const ResourceLlmSimServer&) = delete;

    ~ResourceLlmSimServer() {
        stop();
    }

    void stop() {
        if (svr) {
            svr->stop();
        }
        if (thr.joinable()) {
            thr.join();
        }
        svr.reset();
        port = 0;
    }
};

inline ResourceLlmSimServer startResourceLlmSimServer() {
    ResourceLlmSimServer sim;

    utilxx::HttpServer::Config cfg;
    cfg.address          = "127.0.0.1";
    cfg.port             = 0;
    cfg.ioThreads        = 1;
    cfg.accessLogEnabled = false;
    cfg.maxConnections   = 128;
    cfg.maxRequestBody   = 10 * 1024 * 1024;

    sim.svr           = std::make_unique<utilxx::HttpServer>(cfg);
    auto* rawSvr      = sim.svr.get();
    auto  turnCounter = sim.turnCounter;

    // GET /health
    rawSvr->router().add(
        "/health",
        1,
        std::make_shared<utilxx::HttpServer::Handler>(
            [](utilxx::HttpServer::Request&, utilxx::HttpServer::Response& resp, std::string_view
            ) -> asio::awaitable<void> {
                namespace http = boost::beast::http;
                resp.result(http::status::ok);
                resp.set(http::field::content_type, "application/json");
                resp.body() = "{\"status\":\"ok\"}";
                resp.prepare_payload();
                co_return;
            }
        )
    );

    auto chatHandler = std::make_shared<utilxx::HttpServer::Handler>(
        [turnCounter](
            utilxx::HttpServer::Request&  req,
            utilxx::HttpServer::Response& resp,
            std::string_view
        ) -> asio::awaitable<void> {
            namespace http = boost::beast::http;

            std::string_view body = req.body();
            bool             stream
                = (body.find("\"stream\":true") != std::string_view::npos
                   || body.find("\"stream\": true") != std::string_view::npos);

            // 判断是否为预热轮次
            bool isWarmup = (body.find("RES-BENCH") == std::string_view::npos);

            bool lastIsTool  = false;
            auto lastRolePos = body.rfind("\"role\"");
            if (lastRolePos != std::string_view::npos) {
                auto roleSub
                    = body.substr(lastRolePos, std::min<size_t>(body.size() - lastRolePos, 40));
                if (roleSub.find("\"tool\"") != std::string_view::npos) {
                    lastIsTool = true;
                }
            }

            neograph::json toolCalls = neograph::json::array();
            std::string    replyContent;

            if (isWarmup) {
                replyContent = "Hello! Ready for benchmarking.";
                turnCounter->fetch_add(1);
            } else if (lastIsTool) {
                // tool 结果回来, assistant 返回摘要 (本轮正式结束)
                replyContent
                    = "RES-BENCH assist summary | 已成功读取 README.md 前 40 行内容，并完成分析任务。";
                turnCounter->fetch_add(1);
            } else {
                // user 请求, assistant 返回 tool_call: agentxx_filesystem_read
                neograph::json tc;
                tc["id"]       = "call-000001";
                tc["type"]     = "function";
                tc["function"] = {
                    {"name",      "agentxx_filesystem_read"                                     },
                    {"arguments", "{\"path\":\"README.md\",\"line_offset\":0,\"line_limit\":40}"}
                };
                toolCalls.push_back(tc);
            }

            bool hasToolCalls = !toolCalls.empty();

            if (stream) {
                std::string sseBody;
                auto append = [&](const neograph::json& delta, const std::string& finishReason) {
                    neograph::json ev;
                    ev["id"]      = "chatcmpl-bench-sim";
                    ev["object"]  = "chat.completion.chunk";
                    ev["created"] = 1234567890;
                    ev["model"]   = "bench-sim";

                    neograph::json choice;
                    choice["index"] = 0;
                    choice["delta"] = delta;
                    if (finishReason.empty()) {
                        choice["finish_reason"] = nullptr;
                    } else {
                        choice["finish_reason"] = finishReason;
                    }
                    ev["choices"]  = neograph::json::array({choice});
                    sseBody       += "data: " + ev.dump() + "\n\n";
                };

                neograph::json d;
                d["role"] = "assistant";
                if (hasToolCalls) {
                    d["content"] = nullptr;
                    append(d, "");
                    neograph::json dTc;
                    dTc["tool_calls"] = toolCalls;
                    append(dTc, "");
                    append(neograph::json::object(), "tool_calls");
                } else {
                    d["content"] = replyContent;
                    append(d, "");
                    append(neograph::json::object(), "stop");
                }

                sseBody += "data: [DONE]\n\n";
                resp.result(http::status::ok);
                resp.set(http::field::content_type, "text/event-stream");
                resp.set(http::field::cache_control, "no-cache");
                resp.body() = std::move(sseBody);
                resp.prepare_payload();
            } else {
                neograph::json msg;
                msg["role"] = "assistant";
                if (hasToolCalls) {
                    msg["content"]    = nullptr;
                    msg["tool_calls"] = toolCalls;
                } else {
                    msg["content"] = replyContent;
                }

                neograph::json choice;
                choice["index"]         = 0;
                choice["message"]       = msg;
                choice["finish_reason"] = hasToolCalls ? "tool_calls" : "stop";

                neograph::json respJson;
                respJson["id"]      = "chatcmpl-bench-sim";
                respJson["object"]  = "chat.completion";
                respJson["created"] = 1234567890;
                respJson["model"]   = "bench-sim";
                respJson["choices"] = neograph::json::array({choice});
                respJson["usage"]   = {
                    {"prompt_tokens",     100},
                    {"completion_tokens", 50 },
                    {"total_tokens",      150}
                };

                resp.result(http::status::ok);
                resp.set(http::field::content_type, "application/json");
                resp.body() = respJson.dump();
                resp.prepare_payload();
            }
            co_return;
        }
    );

    // 兼顾带 /v1 与不带 /v1 的请求路径
    rawSvr->router().add("/v1/chat/completions", 2, chatHandler);
    rawSvr->router().add("/chat/completions", 2, chatHandler);

    sim.thr = std::thread([rawSvr]() {
        rawSvr->start();
    });

    for (int i = 0; i < 100; ++i) {
        sim.port = rawSvr->port();
        if (sim.port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return sim;
}

} // namespace bench
} // namespace agentxx
