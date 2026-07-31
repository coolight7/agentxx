#pragma once

/**
 * @file util/async_offload.h
 * @brief 将阻塞操作卸载到线程池执行的协程工具函数
 *
 * 提供 co_offload / co_offload_cancellable 两个工具函数:
 * - co_offload: 将同步阻塞 lambda 卸载到线程池, 主协程挂起等待, 不阻塞 io_context
 * - co_offload_cancellable: 同上, 但支持取消传播 —— 当父协程被 CancelToken 取消时,
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
 *   auto result = co_await agentxx::util::co_offload_cancellable<neograph::json>(
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
#include "asio/thread_pool.hpp"
#include "asio/use_awaitable.hpp"
#include "neograph/define.h"
#include "neograph/graph/cancel.h"
#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace agentxx {
namespace util {

/// 将同步阻塞函数卸载到线程池执行, 主协程挂起等待结果
/// - fn: 无参可调用对象, 返回 T
/// - 不支持取消 (fn 无法感知取消请求); 若需要取消支持请用 co_offload_cancellable
template <typename T, typename F>
asio::awaitable<T> co_offload(asio::thread_pool& pool, F&& fn) {
    co_return co_await asio::co_spawn(
        pool.get_executor(),
        [fn = std::forward<F>(fn)]() -> asio::awaitable<T> {
            co_return fn();
        },
        asio::use_awaitable
    );
}

/// void 特化
template <typename F>
asio::awaitable<void> co_offload_void(asio::thread_pool& pool, F&& fn) {
    co_await asio::co_spawn(
        pool.get_executor(),
        [fn = std::forward<F>(fn)]() -> asio::awaitable<void> {
            fn();
            co_return;
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
/// @tparam T 返回值类型
/// @tparam F 可调用类型, 签名: T(std::atomic<bool>&)
template <typename T, typename F>
asio::awaitable<T> co_offload_cancellable(asio::thread_pool& pool, F&& fn) {
    // 共享取消标志: 父协程 (io_context 线程) 写, 工作线程读
    auto cancel_flag = std::make_shared<std::atomic<bool>>(false);

    try {
        co_return co_await asio::co_spawn(
            pool.get_executor(),
            [fn = std::forward<F>(fn), cancel_flag]() -> asio::awaitable<T> {
                co_return fn(*cancel_flag);
            },
            asio::use_awaitable
        );
    } catch (const neograph_asio_system_error& e) {
        if (e.code() == asio::error::operation_aborted) {
            // 父协程被取消 → 通知工作线程退出
            cancel_flag->store(true, std::memory_order_release);
            throw neograph::graph::CancelledException("offload task cancelled");
        }
        throw;
    }
}

/// void 特化: 支持取消的卸载 (无返回值)
template <typename F>
asio::awaitable<void> co_offload_cancellable_void(asio::thread_pool& pool, F&& fn) {
    auto cancel_flag = std::make_shared<std::atomic<bool>>(false);

    try {
        co_await asio::co_spawn(
            pool.get_executor(),
            [fn = std::forward<F>(fn), cancel_flag]() -> asio::awaitable<void> {
                fn(*cancel_flag);
                co_return;
            },
            asio::use_awaitable
        );
    } catch (const neograph_asio_system_error& e) {
        if (e.code() == asio::error::operation_aborted) {
            cancel_flag->store(true, std::memory_order_release);
            throw neograph::graph::CancelledException("offload task cancelled");
        }
        throw;
    }
}

} // namespace util
} // namespace agentxx
