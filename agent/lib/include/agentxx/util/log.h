#pragma once

#include "agentxx/util/util.h"
#include "asio/awaitable.hpp"
#include "boost/exception/diagnostic_information.hpp"
#include "boost/exception/exception.hpp"
#include "fmt/format.h"
#include "neograph/api.h"
#include <exception>
#include <expected>
#include <functional>
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

    virtual ~LogSink()                                             = default;
    virtual void onLog(LogLevel level, const std::string& message) = 0;
};

/// 全局日志分发器 (单例)
/// - 线程安全
/// - XX_LOG 宏在输出到 stderr 的同时, 将日志分发给所有已注册的 sink
/// - sink 以 weak_ptr 持有, 注册方需自行持有 shared_ptr 以保持其有效
class LogDispatcher {
public:

    static LogDispatcher& instance();

    void addSink(std::shared_ptr<LogSink> sink);

    void removeSink(const std::shared_ptr<LogSink>& sink);

    void dispatch(LogLevel level, const std::string& message);

private:

    LogDispatcher() = default;
    std::mutex                          mutex_;
    std::vector<std::weak_ptr<LogSink>> sinks_;
};

/// XX_LOG 宏统一入口: 输出到 stderr 并分发到已注册的 sink
void xxLogPrint(LogLevel level, const std::string& message);

#if XX_IS_LINUX_D

void printStack();

void signal_handler(int signo);

void signalError(std::string_view exepath);

#else

void printStack();

void signalError(std::string_view exepath);

#endif

} // namespace util
} // namespace agentxx

#if XX_IS_DEBUG_D

#define XX_LOGD(str, ...)                 \
    (::agentxx::util::xxLogPrint(         \
        ::agentxx::util::LogLevel::Debug, \
        fmt::format(str, ##__VA_ARGS__)   \
    ));

#define XX_LOGI(str, ...) \
    (::agentxx::util::xxLogPrint(::agentxx::util::LogLevel::Info, fmt::format(str, ##__VA_ARGS__)));

#define XX_LOGW(str, ...) \
    (::agentxx::util::xxLogPrint(::agentxx::util::LogLevel::Warn, fmt::format(str, ##__VA_ARGS__)));

#define XX_LOGE(str, ...)                 \
    (::agentxx::util::xxLogPrint(         \
        ::agentxx::util::LogLevel::Error, \
        fmt::format(str, ##__VA_ARGS__)   \
    ));

#else

#define XX_LOGD(str, ...) ;

#define XX_LOGI(str, ...) ;

#define XX_LOGW(str, ...) ;

#define XX_LOGE(str, ...) ;

#endif

#define XX_OUT(str, ...) \
    (::agentxx::util::xxLogPrint(::agentxx::util::LogLevel::Out, fmt::format(str, ##__VA_ARGS__)));
