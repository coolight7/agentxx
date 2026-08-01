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

#else

void printStack() {}

void signalError(std::string_view exepath) {}

#endif

} // namespace util
} // namespace agentxx
