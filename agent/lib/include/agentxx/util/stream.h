#pragma once

/// 事件流控制通用工具: 节流 (Throttle) / 防抖 (Debounce)
///
/// 高频事件源 (状态更新、日志、指标采集) 直接处理会造成负载放大或 UI 过度
/// 刷新。本文件提供两种最常用的节流语义, 均线程安全, 可从任意线程调用:
///
/// - Throttle (节流): 限制单位时间内的事件放行次数 —— 两次放行之间至少间隔
///   minInterval。典型场景: server-io 向 UI 推送 codegraph 索引进度时,
///   要求"最短 3 秒推送一次", 高频回调经节流合并, 放行后才发送。
///   尾事件 (限流窗内最后一条) 可选由调用方配合定时器 + force() 补推, 避免丢失。
///
/// - Debounce (防抖): 事件触发后推迟 wait 时长执行, 期间再次触发重置计时,
///   用于"连续事件停止后执行一次" (如输入停顿后搜索/保存、滚动停滞后刷新)。
///   调用方持定时器周期检查 ready(), 就绪后执行回调并 reset()。
///
/// 两者差异:
///   Throttle 保证"至少间隔" (leading edge, 立即放行但限频);
///   Debounce 保证"静默期后执行" (trailing edge, 合并突发事件为一次)。
///   需要 "立即放行 + 限频 + 尾事件必达" 时可组合: try_acquire() 放行立即处理,
///   未放行的事件挂起, 由调用方定时器在 time_until_acquire() 后经 force() 补推。

#include <chrono>
#include <mutex>

namespace agentxx {
namespace util {

/// 节流器: 两次放行 (try_acquire 返回 true) 之间至少间隔 minInterval
///
/// 线程安全: 内部互斥保护, 可从任意线程调用。
///
/// 用法:
/// ```c++
///   agentxx::util::Throttle throttle(std::chrono::seconds{3});
///   // 高频事件循环中:
///   if (throttle.try_acquire()) {
///       flushLatest();            // 限频放行: 立即处理
///   } else {
///       pending = latest;         // 限流窗内: 挂起后由尾推补发
///       scheduleTail(throttle.time_until_acquire()); // 窗末 force() + 补推
///   }
/// ```
class Throttle {
public:

    explicit Throttle(std::chrono::steady_clock::duration minInterval) :
        interval_(minInterval) {}

    Throttle(const Throttle&)            = delete;
    Throttle& operator=(const Throttle&) = delete;

    /// 尝试放行: 距上次放行 >= interval 时放行并记录本次放行时刻, 返回 true;
    /// 否则返回 false (调用方可丢弃该事件或将最新值挂起等待尾推)
    /// - 首次调用恒放行 (无上次放行记录)
    bool try_acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto                  now = std::chrono::steady_clock::now();
        if (last_.time_since_epoch().count() == 0 || now - last_ >= interval_) {
            last_ = now;
            return true;
        }
        return false;
    }

    /// 强制放行: 记录当前时间并返回 true
    /// - 用于限流窗末补推挂起的尾事件: force() 后下一次 try_acquire 仍从本次
    ///   时刻起算 interval, 保证推送频率不突破上限
    bool force() {
        std::lock_guard<std::mutex> lock(mutex_);
        last_ = std::chrono::steady_clock::now();
        return true;
    }

    /// 距下次可放行的剩余时间 (从未放行过 / 已过间隔返回 0)
    std::chrono::steady_clock::duration time_until_acquire() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_.time_since_epoch().count() == 0) {
            return std::chrono::steady_clock::duration::zero();
        }
        const auto now     = std::chrono::steady_clock::now();
        const auto elapsed = now - last_;
        return elapsed >= interval_ ? std::chrono::steady_clock::duration::zero()
                                    : interval_ - elapsed;
    }

    /// 最小放行间隔
    std::chrono::steady_clock::duration interval() const noexcept {
        return interval_;
    }

private:

    std::chrono::steady_clock::duration interval_;
    /// 上次放行时刻 (默认构造 = steady_clock epoch, 表示从未放行)
    std::chrono::steady_clock::time_point last_{};
    mutable std::mutex                    mutex_;
};

/// 防抖器: 触发后等待 wait 时长, 期间再次触发重置计时; 静默满 wait 后 ready
///
/// 线程安全: 内部互斥保护, 可从任意线程调用。
/// 通用计时状态类, 不持有执行上下文 (定时器/回调由调用方注入):
///
/// 用法:
/// ```c++
///   agentxx::util::Debounce debounce(std::chrono::milliseconds{200});
///   // 每次事件到达 (可为任意线程):
///   debounce.trigger();
///   // 调用方定时器 (io 线程) 周期检查:
///   if (debounce.ready()) {
///       doOnceAfterQuiet();   // 事件停止 wait 后执行一次
///       debounce.reset();
///   }
/// ```
class Debounce {
public:

    explicit Debounce(std::chrono::steady_clock::duration wait) :
        wait_(wait) {}

    Debounce(const Debounce&)            = delete;
    Debounce& operator=(const Debounce&) = delete;

    /// 触发一次事件 (重置静默计时); 返回触发时刻
    std::chrono::steady_clock::time_point trigger() {
        std::lock_guard<std::mutex> lock(mutex_);
        last_ = std::chrono::steady_clock::now();
        return last_;
    }

    /// 距最近一次触发已满 wait (且至少触发过一次) 时返回 true
    bool ready() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_.time_since_epoch().count() == 0) {
            return false;
        }
        return std::chrono::steady_clock::now() - last_ >= wait_;
    }

    /// 距 ready 的剩余时间 (从未触发过返回 wait)
    std::chrono::steady_clock::duration time_until_ready() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_.time_since_epoch().count() == 0) {
            return wait_;
        }
        const auto now     = std::chrono::steady_clock::now();
        const auto elapsed = now - last_;
        return elapsed >= wait_ ? std::chrono::steady_clock::duration::zero() : wait_ - elapsed;
    }

    /// 清除触发记录 (计时复位, ready 返回 false)
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        last_ = {};
    }

    /// 静默等待时长
    std::chrono::steady_clock::duration wait() const noexcept {
        return wait_;
    }

private:

    std::chrono::steady_clock::duration wait_;
    /// 最近触发时刻 (默认构造 = steady_clock epoch, 表示从未触发)
    std::chrono::steady_clock::time_point last_{};
    mutable std::mutex                    mutex_;
};

} // namespace util
} // namespace agentxx