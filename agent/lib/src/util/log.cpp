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
    sinks_.push_back(std::move(sink));
}

void LogDispatcher::removeSink(const std::shared_ptr<LogSink>& sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = sinks_.begin(); it != sinks_.end();) {
        auto sp = it->lock();
        if (!sp || sp == sink) {
            it = sinks_.erase(it);
        } else {
            ++it;
        }
    }
}

void LogDispatcher::dispatch(LogLevel level, const std::string& message) {
    std::vector<std::shared_ptr<LogSink>> alive;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 顺带清理已释放的 sink
        for (auto it = sinks_.begin(); it != sinks_.end();) {
            if (auto sp = it->lock()) {
                alive.push_back(sp);
                ++it;
            } else {
                it = sinks_.erase(it);
            }
        }
    }
    for (auto& sink : alive) {
        try {
            sink->onLog(level, message);
        } catch (...) {
            // 忽略 sink 异常, 避免影响日志输出
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
