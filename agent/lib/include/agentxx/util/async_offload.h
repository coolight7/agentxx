#pragma once

/**
 * @file util/async_offload.h
 * @brief 将阻塞操作卸载到线程池执行的协程工具函数
 *
 * 提供 offloadAsync / offloadCancellableAsync 两个工具函数:
 * - offloadAsync: 将同步阻塞 lambda 卸载到线程池, 主协程挂起等待, 不阻塞 io_context
 * - offloadCancellableAsync: 同上, 但支持取消传播 —— 当父协程被 CancelToken 取消时,
 *   设置 cancel_flag 通知工作线程提前退出, 释放线程执行下一个任务
 *
 * 取消传播链:
 *   CancelToken::cancel()
 *     → asio::cancellation_signal::emit(all)
 *     → 父协程 co_await 点收到 operation_aborted
 *     → catch 设置 atomic cancel_flag = true
 *     → 工作线程轮询 is_cancelled() 提前退出
 *     → 线程释放, 可执行下一个任务
 *
 * 用法示例:
 * @code
 *   auto& pool = agentCtx->blockingPool;
 *   auto result = co_await agentxx::util::offloadCancellableAsync<neograph::json>(
 *       *pool,
 *       [](std::atomic<bool>& cancel_flag) -> neograph::json {
 *           for (auto& entry : std::filesystem::directory_iterator(path)) {
 *               if (cancel_flag.load(std::memory_order_acquire)) {
 *                   throw neograph::graph::CancelledException("listing cancelled");
 *               }
 *               // ... process entry
 *           }
 *           return result;
 *       }
 *   );
 * @endcode
 */

#include "asio/co_spawn.hpp"
#include "asio/error.hpp"
#include "asio/thread_pool.hpp"
#include "asio/use_awaitable.hpp"
#include "neograph/define.h"
#include "neograph/graph/cancel.h"
#include <atomic>
#include <functional>
#include <memory>
#include <system_error>
#include <utility>

namespace agentxx {
namespace util {

/// 将同步阻塞函数卸载到线程池执行, 主协程挂起等待结果
/// - fn: 无参可调用对象, 返回 T
/// - 不支持取消 (fn 无法感知取消请求); 若需要取消支持请用 offloadCancellableAsync
///
/// 注意: fn 必须按值传入。本函数是惰性协程 (asio::awaitable), 调用时不会立即执行协程体,
/// 若 fn 用右值引用参数, 其引用的临时对象可能在协程体真正执行前已析构 (use-after-return)。
/// 按值传入会在调用时把 fn 移入协程帧, 保证其生命周期覆盖整个协程。
template<typename T>
asio::awaitable<T> offloadAsync(asio::thread_pool& pool, std::function<asio::awaitable<T>()> fn) {
    co_return co_await asio::co_spawn(
        pool.get_executor(),
        [fn = std::move(fn)]() -> asio::awaitable<T> {
            co_return co_await fn();
        },
        asio::use_awaitable
    );
}

/// 将同步阻塞函数卸载到线程池执行, 支持取消传播
/// - fn: 接受 `std::atomic<bool>&` 参数的可调用对象, 返回 T
///   fn 内部应周期性检查该 flag, 为 true 时抛出 CancelledException 提前退出
/// - 当父协程被取消 (CancelToken emit / operation_aborted) 时:
///   1. 设置 cancel_flag = true, 通知工作线程
///   2. 抛出 CancelledException, 沿协程栈向上传播
/// - 工作线程检测到 cancel_flag 后应尽快退出, 释放线程资源
///
/// 注意: fn 按值传入 (理由同 offloadAsync, 避免惰性协程引用已析构的临时对象)。
///
/// @tparam T 返回值类型
/// @tparam F 可调用类型, 签名: T(std::atomic<bool>&)
template<typename T>
asio::awaitable<T> offloadCancellableAsync(
    asio::thread_pool&                                    pool,
    std::function<asio::awaitable<T>(std::atomic<bool>&)> fn
) {
    // 共享取消标志: 父协程 (io_context 线程) 写, 工作线程读
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);

    try {
        co_return co_await asio::co_spawn(
            pool.get_executor(),
            [fn = std::move(fn), cancelFlag]() -> asio::awaitable<T> {
                co_return co_await fn(*cancelFlag);
            },
            asio::use_awaitable
        );
    } catch (...) {
        // 异常，通知工作线程退出
        cancelFlag->store(true, std::memory_order_release);
        throw;
    }
}

/// 将同步阻塞函数卸载到线程池执行, 支持取消传播 (外部提供 cancelFlag 版本)
/// - 与上面的重载相同, 但 cancelFlag 由调用方提供, 允许外部 (如超时定时器) 设置取消标志
/// - 当父协程被取消或外部设置 cancelFlag 时, 工作线程检测后提前退出
///
/// 注意:
/// - fn 按值传入 (理由同 offloadAsync, 避免惰性协程引用已析构的临时对象)。
/// - 工作协程按值捕获 cancelFlag (shared_ptr 拷贝), 保证工作线程持有有效的取消标志;
///   不能按引用捕获协程参数, 否则协程结束/挂起时引用可能失效。
///
/// - pool 线程池
/// - [cancelFlag] 外部提供的共享取消标志
/// - fn 可调用对象, 签名: awaitable<T>(std::atomic<bool>&)
///
/// return T 返回值类型
template<typename T>
asio::awaitable<T> offloadCancellableAsync(
    asio::thread_pool&                                    pool,
    std::shared_ptr<std::atomic<bool>>                    cancelFlag,
    std::function<asio::awaitable<T>(std::atomic<bool>&)> fn
) {
    try {
        co_return co_await asio::co_spawn(
            pool.get_executor(),
            [fn = std::move(fn), cancelFlag]() -> asio::awaitable<T> {
                co_return co_await fn(*cancelFlag);
            },
            asio::use_awaitable
        );
    } catch (...) {
        // 异常，通知工作线程退出
        cancelFlag->store(true, std::memory_order_release);
        throw;
    }
}

} // namespace util
} // namespace agentxx
