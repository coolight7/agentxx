#include "agentxx/util/log.h"

#include <ctime>
#include <iostream>

#if XX_IS_LINUX_D
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#elif XX_IS_WIN_D
#include <dbghelp.h>
#include <windows.h>
// windows.h 的 min/max 宏与 C++ 标准库冲突
#undef min
#undef max
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#endif

namespace agentxx {
namespace util {

// ---------------------------------------------------------------------------
// LogSink
// ---------------------------------------------------------------------------

void LogSink::enqueue(std::shared_ptr<const LogEntry> entry) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= maxQueue_) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        queue_.push_back(std::move(entry));
    }
    cv_.notify_one();
}

size_t LogSink::pump() {
    std::deque<std::shared_ptr<const LogEntry>> batch;
    uint64_t                                    dropped = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        batch.swap(queue_);
        dropped = dropped_.exchange(0, std::memory_order_relaxed);
    }
    for (const auto& e : batch) {
        onLog(*e);
    }
    if (dropped > 0) {
        onDropped(dropped);
    }
    return batch.size();
}

void LogSink::flush() {
    while (pump() > 0) {
        std::this_thread::yield();
    }
}

void LogSink::onDropped(uint64_t count) {
    // 默认写 stderr (不走日志系统, 避免递归)
    std::cerr << "[log] dropped " << count << " entries (queue full)\n";
}

// ---------------------------------------------------------------------------
// ThreadedLogSink
// ---------------------------------------------------------------------------

ThreadedLogSink::ThreadedLogSink() :
    thread_([this] {
        threadLoop();
    }) {}

ThreadedLogSink::~ThreadedLogSink() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    cv_.notify_one();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ThreadedLogSink::threadLoop() {
    while (true) {
        std::deque<std::shared_ptr<const LogEntry>> batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return !queue_.empty() || !running_;
            });
            if (!running_ && queue_.empty()) {
                break;
            }
            batch.swap(queue_);
            idle_ = false;
        }
        for (const auto& e : batch) {
            onLog(*e);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            idle_ = true;
        }
        cv_.notify_all();
    }
}

void ThreadedLogSink::flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] {
        return queue_.empty() && idle_;
    });
}

// ---------------------------------------------------------------------------
// LogDispatcher
// ---------------------------------------------------------------------------

LogDispatcher& LogDispatcher::instance() {
    static LogDispatcher inst;
    return inst;
}

LogDispatcher::~LogDispatcher() {
    flush();
}

void LogDispatcher::addSink(std::shared_ptr<LogSink> sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        cur  = sinks_.load(std::memory_order_acquire);
    auto                        next = std::make_shared<SinkList>();
    next->reserve(cur->size() + 1);
    // 顺带清理已释放的 sink
    for (const auto& wp : *cur) {
        if (!wp.expired()) {
            next->push_back(wp);
        }
    }
    next->push_back(std::move(sink));
    sinks_.store(std::move(next), std::memory_order_release);
}

void LogDispatcher::removeSink(const std::shared_ptr<LogSink>& sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        cur  = sinks_.load(std::memory_order_acquire);
    auto                        next = std::make_shared<SinkList>();
    next->reserve(cur->size());
    for (const auto& wp : *cur) {
        auto sp = wp.lock();
        if (sp && sp != sink) {
            next->push_back(wp);
        }
    }
    sinks_.store(std::move(next), std::memory_order_release);
}

void LogDispatcher::dispatch(LogLevel level, std::string message) {
    auto entry = std::make_shared<const LogEntry>(LogEntry{
        level,
        seq_.fetch_add(1, std::memory_order_relaxed),
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        )
            .count(),
        std::move(message),
    });
    // 无锁加载快照 (copy-on-write); 多线程并发 dispatch 互不阻塞
    auto snapshot = sinks_.load(std::memory_order_acquire);
    for (const auto& wp : *snapshot) {
        if (auto sp = wp.lock()) {
            sp->enqueue(entry);
        }
    }
}

void LogDispatcher::flush() {
    auto snapshot = sinks_.load(std::memory_order_acquire);
    for (const auto& wp : *snapshot) {
        if (auto sp = wp.lock()) {
            sp->flush();
        }
    }
}

void xxLogPrint(LogLevel level, std::string message) {
    LogDispatcher::instance().dispatch(level, std::move(message));
}

#if XX_IS_LINUX_D

static std::string _exe_path{};

void printStack() {
    // 常规上下文 (非信号处理器), 可使用 printf/backtrace_symbols/popen
    char*  buffer[64];
    char** strings = nullptr;

    auto size = backtrace((void**)buffer, 64);

    printf("======= Dump stack start =======\n");
    strings = backtrace_symbols((void**)buffer, size);
    if (strings == nullptr) {
        printf("backtrace_symbols return nullptr\n");
    }
    for (int i = 0; i < size; i++) {
        if (nullptr == strings || nullptr == strings[i]) {
            printf("[%02d] %p\n", i, buffer[i]);
        } else {
            printf("[%02d] %s\n", i, strings[i]);
        }
        if (buffer[i] != NULL) {
            // 用 snprintf 限定长度, 并对 exe 路径加引号, 避免溢出与路径含空格/特殊字符的注入
            char addr2line_cmd[512];
            int  n = snprintf(
                addr2line_cmd,
                sizeof(addr2line_cmd),
                "addr2line -f -e '%s' %p",
                _exe_path.c_str(),
                buffer[i]
            );
            if (n > 0 && n < static_cast<int>(sizeof(addr2line_cmd))) {
                FILE* addr2line_fp = popen(addr2line_cmd, "r");
                if (addr2line_fp != NULL) {
                    char line[256]{};
                    while (fgets(line, sizeof(line), addr2line_fp) != NULL) {
                        printf("%s", line);
                    }
                    pclose(addr2line_fp);
                }
            }
        } else {
            printf("(unknown)\n");
        }
    }
    printf("======= Dump stack end =======\n");
    free(strings);
}

// ---- async-signal-safe 输出辅助: 信号处理器中只能用此类函数, 不能用 malloc/printf/fmt/fopen/popen
// ----

static void sigSafeWriteBuf(int fd, const char* buf, size_t len) {
    if (fd < 0 || buf == nullptr) {
        return;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t r = write(fd, buf + off, len - off);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        off += static_cast<size_t>(r);
    }
}

static void sigSafeWriteStr(int fd, const char* s) {
    if (s == nullptr) {
        return;
    }
    size_t len = 0;
    while (s[len] != '\0') {
        ++len;
    }
    sigSafeWriteBuf(fd, s, len);
}

static void sigSafeWriteUInt(int fd, unsigned long long v) {
    char tmp[24];
    int  n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v != 0) {
            tmp[n++]  = static_cast<char>('0' + static_cast<int>(v % 10));
            v        /= 10;
        }
    }
    char buf[24];
    for (int i = 0; i < n; ++i) {
        buf[i] = tmp[n - 1 - i];
    }
    sigSafeWriteBuf(fd, buf, static_cast<size_t>(n));
}

static void sigSafeWriteHex(int fd, const void* ptr) {
    uintptr_t v = reinterpret_cast<uintptr_t>(ptr);
    char      tmp[sizeof(uintptr_t) * 2];
    int       n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v != 0) {
            int d      = static_cast<int>(v & 0xFu);
            tmp[n++]   = static_cast<char>(d < 10 ? ('0' + d) : ('a' + (d - 10)));
            v        >>= 4;
        }
    }
    char buf[2 + sizeof(uintptr_t) * 2];
    int  pos   = 0;
    buf[pos++] = '0';
    buf[pos++] = 'x';
    for (int i = n - 1; i >= 0; --i) {
        buf[pos++] = tmp[i];
    }
    sigSafeWriteBuf(fd, buf, static_cast<size_t>(pos));
}

/// 将栈帧地址写入两个 fd (异步信号安全), 并调用 backtrace_symbols_fd 输出基本符号信息
static void sigSafeDumpStack(int consoleFd, int fileFd, void** frames, int size) {
    sigSafeWriteStr(consoleFd, "======= Dump stack start =======\n");
    sigSafeWriteStr(fileFd, "======= Dump stack start =======\n");
    for (int i = 0; i < size; i++) {
        for (int fd : {consoleFd, fileFd}) {
            sigSafeWriteStr(fd, "[");
            sigSafeWriteUInt(fd, static_cast<unsigned long long>(i));
            sigSafeWriteStr(fd, "] ");
            sigSafeWriteHex(fd, frames[i]);
            sigSafeWriteStr(fd, "\n");
        }
    }
    // backtrace_symbols_fd 是 async-signal-safe 的, 输出动态符号表中的函数名
    if (consoleFd >= 0) {
        sigSafeWriteStr(consoleFd, "--- symbols ---\n");
        backtrace_symbols_fd(frames, size, consoleFd);
    }
    if (fileFd >= 0) {
        sigSafeWriteStr(fileFd, "--- symbols ---\n");
        backtrace_symbols_fd(frames, size, fileFd);
    }
    sigSafeWriteStr(consoleFd, "======= Dump stack end =======\n");
    sigSafeWriteStr(fileFd, "======= Dump stack end =======\n");
}

/// fork 子进程调用 addr2line 将地址解析为函数名+源码文件位置 (async-signal-safe:
/// fork/pipe/dup2/close/read/write/execvp/waitpid/_exit 均为信号安全函数)
static void sigSafeAddr2Line(int consoleFd, int fileFd, void** frames, int size) {
    if (_exe_path.empty() || size <= 0) {
        return;
    }

    // 预格式化地址字符串到静态缓冲区 (信号处理器中不可 malloc)
    static char addr_bufs[64][20]; // "0x" + 最多16位hex + '\0'
    const char* argv[64 + 6 + 1];  // addr2line -f -C -p -e <exe> <addrs...> NULL

    int argc     = 0;
    argv[argc++] = "addr2line";
    argv[argc++] = "-f"; // 显示函数名
    argv[argc++] = "-C"; // demangle C++ 符号
    argv[argc++] = "-p"; // 单行漂亮输出
    argv[argc++] = "-e";
    argv[argc++] = _exe_path.c_str();

    int addr_count = size < 64 ? size : 64;
    for (int i = 0; i < addr_count; i++) {
        uintptr_t v   = reinterpret_cast<uintptr_t>(frames[i]);
        char*     buf = addr_bufs[i];
        buf[0]        = '0';
        buf[1]        = 'x';
        char tmp[16];
        int  n = 0;
        if (v == 0) {
            tmp[n++] = '0';
        } else {
            while (v != 0) {
                int d      = static_cast<int>(v & 0xFu);
                tmp[n++]   = static_cast<char>(d < 10 ? ('0' + d) : ('a' + (d - 10)));
                v        >>= 4;
            }
        }
        for (int j = n - 1; j >= 0; --j) {
            buf[2 + (n - 1 - j)] = tmp[j];
        }
        buf[2 + n]   = '\0';
        argv[argc++] = buf;
    }
    argv[argc] = nullptr;

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        // 子进程: stdout 重定向到 pipe 写端, 然后 exec addr2line
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        execvp("addr2line", const_cast<char* const*>(argv));
        _exit(127); // exec 失败
    }

    // 父进程: 从 pipe 读取 addr2line 输出, 写入 console 和 file
    close(pipefd[1]);
    char    rbuf[512];
    ssize_t rn;
    while ((rn = read(pipefd[0], rbuf, sizeof(rbuf))) > 0) {
        sigSafeWriteBuf(consoleFd, rbuf, static_cast<size_t>(rn));
        if (fileFd >= 0) {
            sigSafeWriteBuf(fileFd, rbuf, static_cast<size_t>(rn));
        }
    }
    close(pipefd[0]);

    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
    }
}

/// 信号编号 -> 名称 (async-signal-safe: 仅返回静态字符串)
static const char* sigName(int signo) {
    switch (signo) {
        case SIGSEGV:
            return "SIGSEGV";
        case SIGABRT:
            return "SIGABRT";
        case SIGBUS:
            return "SIGBUS";
        case SIGFPE:
            return "SIGFPE";
        case SIGILL:
            return "SIGILL";
        case SIGTRAP:
            return "SIGTRAP";
        case SIGSYS:
            return "SIGSYS";
        default:
            return "UNKNOWN";
    }
}

/// 信号编号 -> 简要原因描述
static const char* sigReason(int signo) {
    switch (signo) {
        case SIGSEGV:
            return "Segmentation fault (invalid memory access)";
        case SIGABRT:
            return "Abort (assertion failure / std::terminate / abort())";
        case SIGBUS:
            return "Bus error (alignment fault / bad physical address)";
        case SIGFPE:
            return "Floating-point exception (division by zero / overflow)";
        case SIGILL:
            return "Illegal instruction";
        case SIGTRAP:
            return "Trace/breakpoint trap";
        case SIGSYS:
            return "Bad system call";
        default:
            return "Unknown fatal signal";
    }
}

static void signal_handler(int signo, siginfo_t* info, void* /*ucontext*/) {
    // 仅使用 async-signal-safe 函数: backtrace/open/write/close/_exit 等
    void* buffer[64];
    auto  size = backtrace(buffer, 64);

    // 文件名: crash-<pid>.log (getpid 信号安全), 避免使用 fmt/time 格式化
    char filename[64];
    {
        const char* prefix = "crash-";
        const char* suffix = ".log";
        int         pos    = 0;
        for (const char* p = prefix; *p != '\0'; ++p) {
            filename[pos++] = *p;
        }
        char pidbuf[24];
        int  pn  = 0;
        auto pid = static_cast<unsigned long long>(getpid());
        if (pid == 0) {
            pidbuf[pn++] = '0';
        } else {
            while (pid != 0) {
                pidbuf[pn++]  = static_cast<char>('0' + static_cast<int>(pid % 10));
                pid          /= 10;
            }
        }
        for (int i = pn - 1; i >= 0 && pos < static_cast<int>(sizeof(filename)) - 1; --i) {
            filename[pos++] = pidbuf[i];
        }
        for (const char* p = suffix; *p != '\0' && pos < static_cast<int>(sizeof(filename)) - 1;
             ++p) {
            filename[pos++] = *p;
        }
        filename[pos] = '\0';
    }

    int fileFd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    auto writeBoth = [&](const char* s) {
        sigSafeWriteStr(STDERR_FILENO, s);
        if (fileFd >= 0) {
            sigSafeWriteStr(fileFd, s);
        }
    };

    writeBoth("\n======= xx catch signal ");
    sigSafeWriteUInt(STDERR_FILENO, static_cast<unsigned long long>(signo));
    if (fileFd >= 0) {
        sigSafeWriteUInt(fileFd, static_cast<unsigned long long>(signo));
    }
    writeBoth(" (");
    writeBoth(sigName(signo));
    writeBoth(")\n");
    writeBoth("Reason: ");
    writeBoth(sigReason(signo));
    writeBoth("\n");

    // 输出故障地址 (对 SIGSEGV/SIGBUS/SIGFPE 等有意义)
    if (info != nullptr && info->si_addr != nullptr) {
        writeBoth("Fault address: ");
        sigSafeWriteHex(STDERR_FILENO, info->si_addr);
        if (fileFd >= 0) {
            sigSafeWriteHex(fileFd, info->si_addr);
        }
        writeBoth("\n");
    }

    writeBoth("PID: ");
    sigSafeWriteUInt(STDERR_FILENO, static_cast<unsigned long long>(getpid()));
    if (fileFd >= 0) {
        sigSafeWriteUInt(fileFd, static_cast<unsigned long long>(getpid()));
    }
    writeBoth("\n");

    sigSafeDumpStack(STDERR_FILENO, fileFd, buffer, size);

    // fork addr2line 子进程解析地址为函数名+源码位置
    sigSafeWriteStr(STDERR_FILENO, "--- addr2line ---\n");
    if (fileFd >= 0) {
        sigSafeWriteStr(fileFd, "--- addr2line ---\n");
    }
    sigSafeAddr2Line(STDERR_FILENO, fileFd, buffer, size);

    if (fileFd >= 0) {
        close(fileFd);
    }

    sigSafeWriteStr(STDERR_FILENO, "\n# See file: ");
    sigSafeWriteStr(STDERR_FILENO, filename);
    sigSafeWriteStr(STDERR_FILENO, "\n");

    // 恢复默认处理并重新抛出, 保留原始退出状态 (core dump 等)
    struct sigaction sa {};

    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(signo, &sa, nullptr);
    raise(signo);
}

void signalError(std::string_view exepath) {
    _exe_path = exepath;
    XX_LOGI("# Signal error handler: {}", exepath.data());

    struct sigaction sa {};

    sa.sa_sigaction = signal_handler;
    sa.sa_flags     = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    static constexpr int fatalSignals[] = {
        SIGSEGV,
        SIGABRT,
        SIGBUS,
        SIGFPE,
        SIGILL,
        SIGTRAP,
        SIGSYS,
    };
    for (int sig : fatalSignals) {
        sigaction(sig, &sa, nullptr);
    }
}

#elif XX_IS_WIN_D

// ---------------------------------------------------------------------------
// Windows 版 printStack / signalError (dbghelp.dll 运行时动态加载)
// ---------------------------------------------------------------------------
// 设计说明:
// - 运行时 LoadLibrary("dbghelp.dll") + GetProcAddress 解析所需函数, 避免为工程
//   新增 dbghelp.lib 链接依赖; Windows 10+ 系统均自带 dbghelp.dll
// - printStack(): 普通上下文 (非崩溃) 手动调用, 采集当前线程栈并输出函数名/文件
//   行号到 stderr, 对应 Linux 版的 backtrace + addr2line
// - signalError(): 安装 UnhandledExceptionFilter 捕获 SEH 崩溃 (访问违例/非法指令/
//   栈溢出等), 并安装 SIGABRT CRT 信号处理 (assert/abort/std::terminate 路径;
//   硬件异常不注册 CRT 信号, 否则 MSVC CRT 会在独立线程中拦截处理, 丢失崩溃现场),
//   崩溃时输出栈回溯 + 符号到 stderr 与 crash-<pid>.log, 并写 crash-<pid>.dmp
//   迷你转储; 之后交还系统默认处理终止进程 (对应 Linux 版重新 raise 的语义)
// - 崩溃处理上下文中避免 CRT 堆分配与锁等待: 全部使用栈上缓冲区 + WriteFile;
//   符号查询使用 try_lock, 拿不到锁时退化为输出裸地址, 防止崩溃现场死锁
// ---------------------------------------------------------------------------

/// dbghelp 函数指针 (动态解析)
using FnCaptureStackBackTrace = USHORT(WINAPI*)(ULONG, ULONG, PVOID*, PULONG);
using FnSymInitialize         = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
using FnSymSetOptions         = DWORD(WINAPI*)(DWORD);
using FnSymFromAddr           = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
using FnSymGetModuleInfo64    = BOOL(WINAPI*)(HANDLE, DWORD64, PIMAGEHLP_MODULE64);
using FnSymGetLineFromAddr64  = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
using FnMiniDumpWriteDump     = BOOL(WINAPI*)(
    HANDLE,
    DWORD,
    HANDLE,
    MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION,
    PMINIDUMP_USER_STREAM_INFORMATION,
    PMINIDUMP_CALLBACK_INFORMATION
);

static HMODULE                 g_dbghelpDll            = nullptr;
static FnCaptureStackBackTrace g_CaptureStackBackTrace = nullptr;
static FnSymInitialize         g_SymInitialize         = nullptr;
static FnSymSetOptions         g_SymSetOptions         = nullptr;
static FnSymFromAddr           g_SymFromAddr           = nullptr;
static FnSymGetModuleInfo64    g_SymGetModuleInfo64    = nullptr;
static FnSymGetLineFromAddr64  g_SymGetLineFromAddr64  = nullptr;
static FnMiniDumpWriteDump     g_MiniDumpWriteDump     = nullptr;

static std::string _exe_path{}; ///< 可执行文件路径 (signalError 传入), 用于符号搜索
static bool        g_symInitialized = false;
static std::mutex  g_symMutex;   ///< dbghelp 符号查询非线程安全, 需要串行化
static std::once_flag g_symOnce; ///< 保证符号初始化只执行一次

constexpr int kMaxFrames = 64; ///< 最大栈帧数

/// 动态加载 dbghelp.dll 并解析所需函数; 失败返回 false (后续退化为裸地址输出)
static bool winDbgHelpLoad() {
    if (g_dbghelpDll == nullptr) {
        g_dbghelpDll = LoadLibraryW(L"dbghelp.dll");
        if (g_dbghelpDll == nullptr) {
            return false;
        }
        // 注意: CaptureStackBackTrace 不是 dbghelp.dll 的导出, 见下方 ntdll 解析
        g_SymInitialize = (FnSymInitialize)GetProcAddress(g_dbghelpDll, "SymInitialize");
        g_SymSetOptions = (FnSymSetOptions)GetProcAddress(g_dbghelpDll, "SymSetOptions");
        g_SymFromAddr   = (FnSymFromAddr)GetProcAddress(g_dbghelpDll, "SymFromAddr");
        g_SymGetModuleInfo64
            = (FnSymGetModuleInfo64)GetProcAddress(g_dbghelpDll, "SymGetModuleInfo64");
        g_SymGetLineFromAddr64
            = (FnSymGetLineFromAddr64)GetProcAddress(g_dbghelpDll, "SymGetLineFromAddr64");
        g_MiniDumpWriteDump
            = (FnMiniDumpWriteDump)GetProcAddress(g_dbghelpDll, "MiniDumpWriteDump");
    }
    // CaptureStackBackTrace 实际位于 ntdll.dll (RtlCaptureStackBackTrace, 与
    // CaptureStackBackTrace 签名一致), dbghelp.dll 无此导出; ntdll 必然已加载
    if (g_CaptureStackBackTrace == nullptr) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll != nullptr) {
            g_CaptureStackBackTrace
                = (FnCaptureStackBackTrace)GetProcAddress(ntdll, "RtlCaptureStackBackTrace");
        }
    }
    return g_CaptureStackBackTrace != nullptr && g_SymInitialize != nullptr
           && g_SymFromAddr != nullptr;
}

/// 惰性初始化符号 (线程安全, 只执行一次); 返回 false 表示符号不可用
static bool winSymInit() {
    std::call_once(g_symOnce, [] {
        if (!winDbgHelpLoad()) {
            return;
        }
        // UNDNAME: 反修饰 C++ 符号名; DEFERRED_LOADS: 符号按需加载 (避免启动慢);
        // LOAD_LINES: 加载文件行号; FAIL_CRITICAL_ERRORS: PDB 缺失时静默不弹窗
        if (g_SymSetOptions != nullptr) {
            g_SymSetOptions(
                SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES
                | SYMOPT_FAIL_CRITICAL_ERRORS
            );
        }
        // 符号搜索路径: exe 所在目录 (PDB 通常与 exe 同目录)
        std::string searchPath;
        auto        slash = _exe_path.find_last_of("\\/");
        if (slash != std::string::npos) {
            searchPath = _exe_path.substr(0, slash);
        }
        g_symInitialized = g_SymInitialize(
                               GetCurrentProcess(),
                               searchPath.empty() ? nullptr : searchPath.c_str(),
                               TRUE
                           )
                           != FALSE;
    });
    return g_symInitialized;
}

/// 将单个栈帧地址格式化为 "模块!函数+偏移 (文件:行号)" (栈上缓冲区, 崩溃上下文安全);
/// 符号不可用或符号表被占用时退化为只输出裸地址
static void winFormatFrame(char* out, size_t outLen, const void* addr) {
    if (out == nullptr || outLen == 0) {
        return;
    }
    const uintptr_t pc = reinterpret_cast<uintptr_t>(addr);
    out[0]             = '\0';
    if (!g_symInitialized || !g_symMutex.try_lock()) {
        // 符号未就绪, 或符号表正被其他线程使用 (崩溃现场避免死锁): 只输出裸地址
        std::snprintf(out, outLen, "0x%llX", static_cast<unsigned long long>(pc));
        return;
    }
    std::lock_guard<std::mutex> lock(g_symMutex, std::adopt_lock);

    char              module[MAX_PATH]{};
    DWORD64           modBase = 0;
    IMAGEHLP_MODULE64 modInfo{};
    modInfo.SizeOfStruct = sizeof(modInfo);
    if (g_SymGetModuleInfo64 != nullptr
        && g_SymGetModuleInfo64(GetCurrentProcess(), (DWORD64)pc, &modInfo) != FALSE) {
        modBase = modInfo.BaseOfImage;
        std::snprintf(module, sizeof(module), "%s", modInfo.ModuleName);
    }

    // SYMBOL_INFO 含 8 字节对齐成员, 用 alignas 显式对齐栈缓冲区
    alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)];
    auto*                     sym = reinterpret_cast<PSYMBOL_INFO>(symBuf);
    sym->SizeOfStruct             = sizeof(SYMBOL_INFO);
    sym->MaxNameLen               = MAX_SYM_NAME;
    DWORD64 disp                  = 0;
    bool    hasSym                = g_SymFromAddr != nullptr
                  && g_SymFromAddr(GetCurrentProcess(), (DWORD64)pc, &disp, sym) != FALSE;

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisp    = 0;
    bool  hasLine
        = g_SymGetLineFromAddr64 != nullptr
          && g_SymGetLineFromAddr64(GetCurrentProcess(), (DWORD64)pc, &lineDisp, &line) != FALSE;

    if (hasSym) {
        if (module[0] != '\0') {
            if (hasLine && line.FileName != nullptr) {
                std::snprintf(
                    out,
                    outLen,
                    "%s!%s+0x%llX (%s:%lu)",
                    module,
                    sym->Name,
                    static_cast<unsigned long long>(disp),
                    line.FileName,
                    static_cast<unsigned long>(line.LineNumber)
                );
            } else {
                std::snprintf(
                    out,
                    outLen,
                    "%s!%s+0x%llX",
                    module,
                    sym->Name,
                    static_cast<unsigned long long>(disp)
                );
            }
        } else {
            std::snprintf(
                out,
                outLen,
                "%s+0x%llX",
                sym->Name,
                static_cast<unsigned long long>(disp)
            );
        }
    } else if (module[0] != '\0' && modBase != 0) {
        std::snprintf(
            out,
            outLen,
            "%s+0x%llX",
            module,
            static_cast<unsigned long long>(pc - modBase)
        );
    } else {
        std::snprintf(out, outLen, "0x%llX", static_cast<unsigned long long>(pc));
    }
}

/// 将字符串完整写入句柄 (循环 WriteFile 处理部分写入; 崩溃上下文安全, 不依赖 CRT 堆)
static void winWriteAll(HANDLE h, const char* s) {
    if (h == nullptr || h == INVALID_HANDLE_VALUE || s == nullptr) {
        return;
    }
    DWORD len = static_cast<DWORD>(strlen(s));
    while (len > 0) {
        DWORD written = 0;
        if (WriteFile(h, s, len, &written, nullptr) == FALSE || written == 0) {
            break;
        }
        s   += written;
        len -= written;
    }
}

/// 打开 crash-<pid>.log (崩溃上下文安全)
static HANDLE winOpenCrashLog() {
    char filename[MAX_PATH];
    std::snprintf(
        filename,
        sizeof(filename),
        "crash-%lu.log",
        static_cast<unsigned long>(GetCurrentProcessId())
    );
    return CreateFileA(
        filename,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
}

/// 采集并输出当前线程栈回溯到两个句柄 (stderr / crash 日志)
static void winDumpStack(HANDLE console, HANDLE file) {
    void*  frames[kMaxFrames]{};
    USHORT count = 0;
    if (g_CaptureStackBackTrace != nullptr) {
        // 跳过 2 帧: CaptureStackBackTrace 自身 + winDumpStack
        count = g_CaptureStackBackTrace(2, kMaxFrames, frames, nullptr);
    }
    winWriteAll(console, "======= Dump stack start =======\n");
    winWriteAll(file, "======= Dump stack start =======\n");
    if (count == 0) {
        winWriteAll(console, "(capture stack failed)\n");
        winWriteAll(file, "(capture stack failed)\n");
    }
    for (USHORT i = 0; i < count; ++i) {
        char frame[512];
        winFormatFrame(frame, sizeof(frame), frames[i]);
        char line[640];
        std::snprintf(line, sizeof(line), "[%02u] %s\n", static_cast<unsigned>(i), frame);
        winWriteAll(console, line);
        winWriteAll(file, line);
    }
    winWriteAll(console, "======= Dump stack end =======\n");
    winWriteAll(file, "======= Dump stack end =======\n");
}

/// 异常码 -> 名称 (供日志输出)
static const char* winExceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
            return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT:
            return "BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_OVERFLOW:
            return "FLT_OVERFLOW";
        case EXCEPTION_FLT_UNDERFLOW:
            return "FLT_UNDERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:
            return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:
            return "INT_OVERFLOW";
        case EXCEPTION_PRIV_INSTRUCTION:
            return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:
            return "STACK_OVERFLOW";
        case 0xC0000409:
            return "STACK_BUFFER_OVERRUN/FAST_FAIL";
        case 0xC0000374:
            return "HEAP_CORRUPTION";
        case 0xE06D7363:
            return "MSVC_CXX_EXCEPTION";
        case 0xC0000135:
            return "DLL_NOT_FOUND";
        case 0xC0000139:
            return "ENTRYPOINT_NOT_FOUND";
        default:
            return "UNKNOWN";
    }
}

/// 写 crash-<pid>.dmp 迷你转储 (崩溃过滤器调用; 失败静默)
static void winWriteMiniDump(EXCEPTION_POINTERS* ep) {
    if (g_MiniDumpWriteDump == nullptr) {
        return;
    }
    char filename[MAX_PATH];
    std::snprintf(
        filename,
        sizeof(filename),
        "crash-%lu.dmp",
        static_cast<unsigned long>(GetCurrentProcessId())
    );
    HANDLE h = CreateFileA(
        filename,
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId          = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers    = FALSE;
    g_MiniDumpWriteDump(
        GetCurrentProcess(),
        GetCurrentProcessId(),
        h,
        MiniDumpWithDataSegs,
        ep != nullptr ? &mei : nullptr,
        nullptr,
        nullptr
    );
    CloseHandle(h);
}

/// 未处理异常过滤器: 输出崩溃信息 + 栈回溯, 然后交还系统默认处理终止进程
/// (对应 Linux 版的 signal_handler; 返回 EXCEPTION_CONTINUE_SEARCH 保留标准行为)
static LONG WINAPI winExceptionFilter(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* rec  = ep != nullptr ? ep->ExceptionRecord : nullptr;
    const DWORD             code = rec != nullptr ? rec->ExceptionCode : 0;

    const HANDLE console = GetStdHandle(STD_ERROR_HANDLE);
    const HANDLE file    = winOpenCrashLog();

    char buf[512];
    std::snprintf(
        buf,
        sizeof(buf),
        "\n======= xx catch exception 0x%08lX (%s) =======\n",
        static_cast<unsigned long>(code),
        winExceptionName(code)
    );
    winWriteAll(console, buf);
    winWriteAll(file, buf);

    if (rec != nullptr && rec->ExceptionAddress != nullptr) {
        std::snprintf(
            buf,
            sizeof(buf),
            "Exception address: 0x%llX\n",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(rec->ExceptionAddress))
        );
        winWriteAll(console, buf);
        winWriteAll(file, buf);
    }
    if (code == EXCEPTION_ACCESS_VIOLATION && rec != nullptr && rec->NumberParameters >= 2) {
        // ExceptionInformation[0]: 0=读 1=写 8=执行; [1]: 故障地址
        const char* op = rec->ExceptionInformation[0] == 1
                             ? "write"
                             : (rec->ExceptionInformation[0] == 8 ? "execute" : "read");
        std::snprintf(
            buf,
            sizeof(buf),
            "Fault address: 0x%llX (%s)\n",
            static_cast<unsigned long long>(rec->ExceptionInformation[1]),
            op
        );
        winWriteAll(console, buf);
        winWriteAll(file, buf);
    }
    std::snprintf(
        buf,
        sizeof(buf),
        "PID: %lu, TID: %lu\n",
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentThreadId())
    );
    winWriteAll(console, buf);
    winWriteAll(file, buf);
    if (!_exe_path.empty()) {
        std::snprintf(buf, sizeof(buf), "Exe: %s\n", _exe_path.c_str());
        winWriteAll(console, buf);
        winWriteAll(file, buf);
    }

    winDumpStack(console, file);
    winWriteMiniDump(ep);

    if (file != INVALID_HANDLE_VALUE) {
        std::snprintf(
            buf,
            sizeof(buf),
            "\n# See file: crash-%lu.log\n",
            static_cast<unsigned long>(GetCurrentProcessId())
        );
        winWriteAll(console, buf);
        winWriteAll(file, buf);
        CloseHandle(file);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/// CRT 信号处理 (SIGABRT): assert/abort/std::terminate 路径;
/// 输出栈回溯后恢复默认处理并重新 raise (与 Linux 版语义一致)
/// 注: 硬件异常 (访问违例等) 不走这里, 由 winExceptionFilter 在崩溃线程现场捕获
static void winSignalHandler(int signo) {
    const HANDLE console = GetStdHandle(STD_ERROR_HANDLE);
    const HANDLE file    = winOpenCrashLog();

    char buf[256];
    std::snprintf(buf, sizeof(buf), "\n======= xx catch signal %d =======\n", signo);
    winWriteAll(console, buf);
    winWriteAll(file, buf);

    winDumpStack(console, file);

    if (file != INVALID_HANDLE_VALUE) {
        std::snprintf(
            buf,
            sizeof(buf),
            "\n# See file: crash-%lu.log\n",
            static_cast<unsigned long>(GetCurrentProcessId())
        );
        winWriteAll(console, buf);
        winWriteAll(file, buf);
        CloseHandle(file);
    }

    // 恢复默认处理并重新抛出, 保留标准退出行为
    signal(signo, SIG_DFL);
    raise(signo);
}

void printStack() {
    // 普通上下文 (非崩溃), 确保符号已初始化后输出当前线程栈到 stderr
    winSymInit();
    winDumpStack(GetStdHandle(STD_ERROR_HANDLE), INVALID_HANDLE_VALUE);
}

void signalError(std::string_view exepath) {
    _exe_path.assign(exepath.data(), exepath.size());

    // 抑制 Windows 错误报告对话框 (崩溃后不弹窗, 信息统一写入 crash-<pid>.log)
    SetErrorMode(SEM_NOGPFAULTERRORBOX);

    // 预加载符号: 在主线程启动阶段完成, 避免崩溃处理时才做重 IO/加载
    winSymInit();

    // 捕获 SEH 崩溃 (访问违例/非法指令/除零/栈溢出等)。注意: 不能同时注册
    // SIGSEGV/SIGFPE/SIGILL 的 CRT 信号处理, MSVC 的 CRT 会抢先拦截对应的硬件异常,
    // 并在独立线程中调用信号处理器 (栈回溯不可用、无 EXCEPTION_POINTERS), 导致
    // 崩溃现场信息丢失; 不注册时异常会继续传播到本过滤器, 在崩溃线程现场运行
    SetUnhandledExceptionFilter(&winExceptionFilter);

    // CRT 信号路径: 仅保留非硬件异常信号。assert/abort/std::terminate 触发 SIGABRT
    // (raise 同步调用, 位于崩溃线程, 栈回溯有效)
    signal(SIGABRT, &winSignalHandler);

    XX_LOGI("# Signal error handler: {}", _exe_path);
}

#else

void printStack() {}

void signalError(std::string_view exepath) {}

#endif

} // namespace util
} // namespace agentxx
