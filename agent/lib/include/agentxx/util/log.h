#pragma once

#include "agentxx/util/util.h"
#include "fmt/format.h"
#include <ctime>
#include <iostream>
#include <string_view>

#if XX_IS_DEBUG_D

#define XX_LOGD(str, ...)                                                      \
  (std::cerr << fmt::format(str, ##__VA_ARGS__) << std::endl);

#define XX_LOGI(str, ...)                                                      \
  (std::cerr << fmt::format(str, ##__VA_ARGS__) << std::endl);

#define XX_LOGW(str, ...)                                                      \
  (std::cerr << fmt::format(str, ##__VA_ARGS__) << std::endl);

#define XX_LOGE(str, ...)                                                      \
  (std::cerr << fmt::format(str, ##__VA_ARGS__) << std::endl);

#define XX_OUT(str, ...)                                                       \
  (std::cerr << fmt::format(str, ##__VA_ARGS__) << std::endl);

#else

#define XX_LOGD(str, ...) ;

#define XX_LOGI(str, ...) ;

#define XX_LOGW(str, ...) ;

#define XX_LOGE(str, ...) ;

#define XX_OUT(str, ...)                                                       \
  (std::cerr << fmt::format(str, ##__VA_ARGS__) << std::endl);

#endif

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