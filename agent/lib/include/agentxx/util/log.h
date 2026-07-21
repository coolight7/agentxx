#pragma once

#include "agentxx/util/util.h"
#include "fmt/format.h"
#include <ctime>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace util {

/// 日志级别
enum class LogLevel {
  Debug,
  Info,
  Warn,
  Error,
  Out,
};

/// 日志接收接口
/// - 实现并注册到 LogDispatcher 以接收 XX_LOG 系列宏输出的日志
/// - 例如 TUI 实现此接口将日志显示到日志窗口
class LogSink {
public:
  virtual ~LogSink() = default;
  virtual void onLog(LogLevel level, const std::string &message) = 0;
};

/// 全局日志分发器 (单例)
/// - 线程安全
/// - XX_LOG 宏在输出到 stderr 的同时, 将日志分发给所有已注册的 sink
/// - sink 以 weak_ptr 持有, 注册方需自行持有 shared_ptr 以保持其有效
class LogDispatcher {
public:
  static LogDispatcher &instance() {
    static LogDispatcher inst;
    return inst;
  }

  void addSink(std::shared_ptr<LogSink> sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.push_back(std::move(sink));
  }

  void removeSink(const std::shared_ptr<LogSink> &sink) {
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

  void dispatch(LogLevel level, const std::string &message) {
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
    for (auto &sink : alive) {
      try {
        sink->onLog(level, message);
      } catch (...) {
        // 忽略 sink 异常, 避免影响日志输出
      }
    }
  }

private:
  LogDispatcher() = default;
  std::mutex mutex_;
  std::vector<std::weak_ptr<LogSink>> sinks_;
};

/// XX_LOG 宏统一入口: 输出到 stderr 并分发到已注册的 sink
inline void xxLogPrint(LogLevel level, const std::string &message) {
  std::cerr << message << std::endl;
  LogDispatcher::instance().dispatch(level, message);
}

} // namespace util
} // namespace agentxx

#if XX_IS_DEBUG_D

#define XX_LOGD(str, ...)                                                      \
  (::agentxx::util::xxLogPrint(::agentxx::util::LogLevel::Debug,               \
                               fmt::format(str, ##__VA_ARGS__)));

#define XX_LOGI(str, ...)                                                      \
  (::agentxx::util::xxLogPrint(::agentxx::util::LogLevel::Info,                \
                               fmt::format(str, ##__VA_ARGS__)));

#define XX_LOGW(str, ...)                                                      \
  (::agentxx::util::xxLogPrint(::agentxx::util::LogLevel::Warn,                \
                               fmt::format(str, ##__VA_ARGS__)));

#define XX_LOGE(str, ...)                                                      \
  (::agentxx::util::xxLogPrint(::agentxx::util::LogLevel::Error,               \
                               fmt::format(str, ##__VA_ARGS__)));

#else

#define XX_LOGD(str, ...) ;

#define XX_LOGI(str, ...) ;

#define XX_LOGW(str, ...) ;

#define XX_LOGE(str, ...) ;

#endif

#define XX_OUT(str, ...)                                                       \
  (::agentxx::util::xxLogPrint(::agentxx::util::LogLevel::Out,                 \
                               fmt::format(str, ##__VA_ARGS__)));

#if XX_IS_LINUX_D

#include <csignal>
#include <execinfo.h>

namespace agentxx {
namespace util {

inline static std::string _exe_path{};

inline void printStack() {

#define innerPrintToConsoleAndFile_d(str, ...) printf(str, ##__VA_ARGS__);

  {
    char *buffer[64];
    char **strings = nullptr;

    auto size = backtrace((void **)buffer, 64);

    innerPrintToConsoleAndFile_d("======= Dump stack start =======\n");
    {
      strings = backtrace_symbols((void **)buffer, size);
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
        sprintf(addr2line_cmd, "addr2line -f -e %s %p", _exe_path.c_str(),
                buffer[i]);
        FILE *addr2line_fp = popen(addr2line_cmd, "r");
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

inline void signal_handler(int signo) {
  const std::string filename = fmt::format("crash-{}.log", std::time(nullptr));

#define innerPrintToConsoleAndFile_d(fp, str, ...)                             \
  printf(str, ##__VA_ARGS__);                                                  \
  fprintf(fp, str, ##__VA_ARGS__);

  {
    char *buffer[64];
    char **strings = nullptr;

    auto size = backtrace((void **)buffer, 64);

    FILE *fp = fopen(filename.c_str(), "w");
    if (fp != NULL) {
      innerPrintToConsoleAndFile_d(fp, "\n======= xx catch signal %d =======\n",
                                   signo);
      innerPrintToConsoleAndFile_d(fp, "======= Dump stack start =======\n");
      {
        strings = backtrace_symbols((void **)buffer, size);
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
          sprintf(addr2line_cmd, "addr2line -f -e %s %p", _exe_path.c_str(),
                  buffer[i]);
          FILE *addr2line_fp = popen(addr2line_cmd, "r");
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

inline void signalError(std::string_view exepath) {
  _exe_path = exepath;
  printf("# Signal error handler: %s\n", exepath.data());
  signal(SIGSEGV, signal_handler);
}

}; // namespace util
}; // namespace agentxx

#else

namespace agentxx {
namespace util {
inline void printStack() {}

inline void signalError(std::string_view exepath) {}

}; // namespace util
}; // namespace agentxx

#endif