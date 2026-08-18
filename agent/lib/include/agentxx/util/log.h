#pragma once

#include "agentxx/util/util.h"
#include "fmt/format.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace agentxx {
namespace util {

/// 日志级别
enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Out,
};

/// 日志条目: 格式化一次, 经 shared_ptr<const LogEntry> 共享给所有 sink,
/// sink 只读不可 move, 保证每个 sink 都能拿到完整内容
struct LogEntry {
    LogLevel    level;
    uint64_t    seq;     ///< 全局递增序号 (入队时分配, 用于排序)
    int64_t     wallNs;  ///< 墙钟时间 ns since epoch (入队时打, 反映产生时刻)
    std::string message; ///< 已格式化的日志内容
};

/// 日志接收基类
/// - 内置线程安全有界队列: 生产者线程调 enqueue() 入队, 宿主线程调 pump() 消费
/// - onLog() 总在宿主线程串行执行, 子类无需自行加锁
/// - 队列满时丢弃新条目并计数, 保证生产者永不阻塞
class LogSink {
public:

    virtual ~LogSink() = default;

    /// 生产者线程调用, 线程安全入队 (满则丢弃)
    void enqueue(std::shared_ptr<const LogEntry> entry);

    /// 宿主线程调用, 排空队列并逐条调 onLog; 返回消费条数
    size_t pump();

    /// 等待队列排空 (默认循环调 pump; ThreadedLogSink 覆写为等待后台线程)
    virtual void flush();

    /// 已丢弃的条目累计数
    uint64_t droppedCount() const {
        return dropped_.load(std::memory_order_relaxed);
    }

protected:

    /// 宿主线程串行调用, 子类实现具体输出逻辑 (无需加锁)
    virtual void onLog(const LogEntry& entry) = 0;

    /// 队列溢出丢弃时调用 (默认写 stderr; 子类可覆写, 如 TUI 显示提示)
    virtual void onDropped(uint64_t count);

    std::mutex                                  mutex_;
    std::condition_variable                     cv_;
    std::deque<std::shared_ptr<const LogEntry>> queue_;
    size_t                                      maxQueue_ = 4096;
    std::atomic<uint64_t>                       dropped_{0};
};

/// 自带后台消费线程的 LogSink
/// - 构造时启动线程, 析构时停止并 drain 剩余日志
/// - 适用于 stderr 输出、网络转发等无需绑定特定线程的 sink
class ThreadedLogSink : public LogSink {
public:

    ThreadedLogSink();
    ~ThreadedLogSink() override;

    ThreadedLogSink(const ThreadedLogSink&)            = delete;
    ThreadedLogSink& operator=(const ThreadedLogSink&) = delete;

    /// 等待后台线程处理完队列中所有条目
    void flush() override;

protected:

    /// 停止后台线程: 等待队列排空且线程空闲后停止并 join
    /// - 必须在最派生类析构中调用 (此时虚表仍为最派生类): 线程执行 onLog
    ///   虚调用解析安全; 若延迟到基类析构 (虚表已切换为基类), 线程执行纯虚
    ///   onLog 虚调用会 purecall -> abort (进程退出瞬间日志刚入队时最易触发)
    /// - 基类析构会幂等兜底 (已 join 后 joinable() 为 false 直接跳过)
    void shutdownThread();

    void onLog(const LogEntry& entry) override = 0;

private:

    void threadLoop();

    std::thread thread_;
    bool        running_ = true; // 受 LogSink::mutex_ 保护
    bool        idle_    = true; // 受 LogSink::mutex_ 保护; 线程未在 onLog 中时为 true
};

/// 全局日志分发器 (单例)
/// - 线程安全
/// - XX_LOG 宏将日志格式化后入队到所有已注册的 sink (非阻塞)
/// - sink 以 weak_ptr 持有, 注册方需自行持有 shared_ptr 以保持其有效
/// - 性能: dispatch 为最热路径, 采用 copy-on-write 快照实现无锁读取;
///   仅 addSink/removeSink (罕见) 在 mutex_ 下复制并原子替换快照
class LogDispatcher {
public:

    static LogDispatcher& instance();

    ~LogDispatcher();

    void addSink(std::shared_ptr<LogSink> sink);

    void removeSink(const std::shared_ptr<LogSink>& sink);

    /// 创建 LogEntry (打序号+时间戳) 并入队到所有 sink (线程安全, 非阻塞)
    void dispatch(LogLevel level, std::string message);

    /// 等待所有 sink 队列排空 (用于进程退出前确保日志不丢)
    void flush();

private:

    using SinkList = std::vector<std::weak_ptr<LogSink>>;

    LogDispatcher() :
        sinks_(std::make_shared<const SinkList>()) {}

    /// 仅用于序列化 add/remove 的 copy-on-write (注册罕见, 不在热路径)
    std::mutex mutex_;
    /// sink 快照: dispatch 无锁 load, add/remove 复制后原子 store
    std::atomic<std::shared_ptr<const SinkList>> sinks_;
    /// 全局日志序号
    std::atomic<uint64_t> seq_{0};
};

/// XX_LOG 宏统一入口: 格式化后入队到所有已注册的 sink
void xxLogPrint(LogLevel level, std::string message);

#if XX_IS_LINUX_D

void printStack();

void signalError(std::string_view exepath);

#else

void printStack();

void signalError(std::string_view exepath);

#endif

} // namespace util
} // namespace agentxx

#define XX_LOGT(str, ...)                 \
    (::agentxx::util::xxLogPrint(         \
        ::agentxx::util::LogLevel::Trace, \
        fmt::format(str, ##__VA_ARGS__)   \
    ));

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

#define XX_OUT(str, ...) \
    (::agentxx::util::xxLogPrint(::agentxx::util::LogLevel::Out, fmt::format(str, ##__VA_ARGS__)));
