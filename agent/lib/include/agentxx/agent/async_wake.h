///
/// 后台任务用的唤醒器: 完成 / 取消 / 到期都叫醒等待方一次
///
/// 为什么不用轮询: 后台工具任务要等**两个信号** —— 执行体跑完 (只有一个原子标志)
/// 与取消置位 (取消令牌里的原子标志)。等待普通条件没有现成的异步原语, 旧实现在
/// 一个 50 ms 定时器循环里查标志: 每个运行中的工具任务每秒 20 次空转唤醒, 取消
/// 还要等下一次醒来。这里改成通知式: 谁改变状态谁 `signal()`, 等待方只醒一次。
///
/// 线程约定:
/// - `signal()` 任意线程可调: 只加锁记一次信号, 再投递到容器 io 线程取消等待
/// - `wait()` **只能在容器 io 线程调**: 醒一次就返回, 由调用方复核 done / token / 到期
/// - 信号不会丢也不会累积: 每轮等待最多消费一次已发生的信号 (旧信号不会把后续
///   等待连续打醒成忙循环)
#pragma once

#include "asio/io_context.hpp"
#include "asio/post.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "utilxx_base/asio_error.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace agentxx {
namespace agent {

class AsyncWake {
public:

    explicit AsyncWake(asio::io_context::executor_type executor) :
        timer_(std::make_shared<asio::steady_timer>(std::move(executor))) {}

    /// 叫醒等待方一次 (完成 / 取消 / 到期)
    void signal() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++signaled_;
        }
        // 投递体捕获定时器本身: 管理器/任务已经释放时也能安全取消
        asio::post(timer_->get_executor(), [timer = timer_]() { timer->cancel(); });
    }

    /// 等一次唤醒
    /// - `timeout` <= 0: 不设到期, 只等 `signal()`
    /// - 到期 (`timeout` > 0) 时也返回, 由调用方按自己的截止时刻判断
    asio::awaitable<void> wait(std::chrono::milliseconds timeout) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (consumed_ != signaled_) {
                consumed_ = signaled_;
                co_return; // 信号已经发生: 不必再等
            }
        }
        if (timeout.count() > 0) {
            timer_->expires_after(timeout);
        } else {
            timer_->expires_at(asio::steady_timer::time_point::max());
        }
        utilxx_base::AsioErrorCode ec;
        co_await timer_->async_wait(asio::redirect_error(asio::use_awaitable, ec));
        std::lock_guard<std::mutex> lock(mutex_);
        consumed_ = signaled_;
    }

private:

    std::shared_ptr<asio::steady_timer> timer_;
    std::mutex                          mutex_;
    /// 已发生的信号数与已消费的信号数 (差值 = 还没被看到的信号)
    uint64_t                            signaled_ = 0;
    uint64_t                            consumed_ = 0;
};

} // namespace agent
} // namespace agentxx
