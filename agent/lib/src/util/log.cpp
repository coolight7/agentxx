#include "agentxx/util/log.h"

#include <ctime>
#include <iostream>

#if XX_IS_LINUX_D
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#endif

namespace agentxx {
namespace util {

LogDispatcher& LogDispatcher::instance() {
    static LogDispatcher inst;
    return inst;
}

void LogDispatcher::addSink(std::shared_ptr<LogSink> sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto cur  = sinks_.load(std::memory_order_acquire);
    auto next = std::make_shared<SinkList>();
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
    auto cur  = sinks_.load(std::memory_order_acquire);
    auto next = std::make_shared<SinkList>();
    next->reserve(cur->size());
    for (const auto& wp : *cur) {
        auto sp = wp.lock();
        if (sp && sp != sink) {
            next->push_back(wp);
        }
    }
    sinks_.store(std::move(next), std::memory_order_release);
}

void LogDispatcher::dispatch(LogLevel level, const std::string& message) {
    // 无锁加载快照 (copy-on-write); 多线程并发 dispatch 互不阻塞, 且不持锁回调 sink
    auto snapshot = sinks_.load(std::memory_order_acquire);
    for (const auto& wp : *snapshot) {
        if (auto sp = wp.lock()) {
            try {
                sp->onLog(level, message);
            } catch (...) {
                // 忽略 sink 异常, 避免影响日志输出
            }
        }
    }
}

void xxLogPrint(LogLevel level, const std::string& message) {
    // std::cerr << message << std::endl;
    LogDispatcher::instance().dispatch(level, message);
}

#if XX_IS_LINUX_D

static std::string _exe_path{};

void printStack() {
#define innerPrintToConsoleAndFile_d(str, ...) printf(str, ##__VA_ARGS__);

    {
        char*  buffer[64];
        char** strings = nullptr;

        auto size = backtrace((void**)buffer, 64);

        innerPrintToConsoleAndFile_d("======= Dump stack start =======\n");
        {
            strings = backtrace_symbols((void**)buffer, size);
            if (strings == nullptr) {
                innerPrintToConsoleAndFile_d("backtrace_symbols return nullptr");
            }
        }
        for (int i = 0; i < size; i++) {
            if (nullptr == strings[i]) {
                innerPrintToConsoleAndFile_d("[%02d] %p\n", i, buffer[i]);
            } else {
                innerPrintToConsoleAndFile_d("[%02d] %s\n", i, strings[i]);
            }
            if (buffer[i] != NULL) {
                char addr2line_cmd[256];
                sprintf(addr2line_cmd, "addr2line -f -e %s %p", _exe_path.c_str(), buffer[i]);
                FILE* addr2line_fp = popen(addr2line_cmd, "r");
                if (addr2line_fp != NULL) {
                    char line[256]{};
                    while (fgets(line, sizeof(line), addr2line_fp) != NULL) {
                        innerPrintToConsoleAndFile_d("%s", line);
                    }
                    pclose(addr2line_fp);
                }
            } else {
                innerPrintToConsoleAndFile_d("(unknown)\n");
            }
        }
        innerPrintToConsoleAndFile_d("======= Dump stack end =======\n");
        free(strings);
    }
#undef innerPrintToConsoleAndFile_d
}

void signal_handler(int signo) {
    const std::string filename = fmt::format("crash-{}.log", std::time(nullptr));

#define innerPrintToConsoleAndFile_d(fp, str, ...) \
    printf(str, ##__VA_ARGS__);                    \
    fprintf(fp, str, ##__VA_ARGS__);

    {
        char*  buffer[64];
        char** strings = nullptr;

        auto size = backtrace((void**)buffer, 64);

        FILE* fp = fopen(filename.c_str(), "w");
        if (fp != NULL) {
            innerPrintToConsoleAndFile_d(fp, "\n======= xx catch signal %d =======\n", signo);
            innerPrintToConsoleAndFile_d(fp, "======= Dump stack start =======\n");
            {
                strings = backtrace_symbols((void**)buffer, size);
                if (strings == nullptr) {
                    innerPrintToConsoleAndFile_d(fp, "backtrace_symbols return nullptr");
                }
            }
            for (int i = 0; i < size; i++) {
                if (nullptr == strings[i]) {
                    innerPrintToConsoleAndFile_d(fp, "[%02d] %p\n", i, buffer[i]);
                } else {
                    innerPrintToConsoleAndFile_d(fp, "[%02d] %s\n", i, strings[i]);
                }
                if (buffer[i] != NULL) {
                    char addr2line_cmd[256];
                    sprintf(addr2line_cmd, "addr2line -f -e %s %p", _exe_path.c_str(), buffer[i]);
                    FILE* addr2line_fp = popen(addr2line_cmd, "r");
                    if (addr2line_fp != NULL) {
                        char line[256]{};
                        while (fgets(line, sizeof(line), addr2line_fp) != NULL) {
                            innerPrintToConsoleAndFile_d(fp, "%s", line);
                        }
                        pclose(addr2line_fp);
                    }
                } else {
                    innerPrintToConsoleAndFile_d(fp, "(unknown)\n");
                }
            }
            innerPrintToConsoleAndFile_d(fp, "======= Dump stack end =======\n");
            fclose(fp);
            free(strings);
        }
    }
    printf("\n# See file: %s\n", filename.c_str());
    signal(signo, SIG_DFL);
    raise(signo);
#undef innerPrintToConsoleAndFile_d
}

void signalError(std::string_view exepath) {
    _exe_path = exepath;
    printf("# Signal error handler: %s\n", exepath.data());
    signal(SIGSEGV, signal_handler);
}

#else

void printStack() {}

void signalError(std::string_view exepath) {}

#endif

} // namespace util
} // namespace agentxx
