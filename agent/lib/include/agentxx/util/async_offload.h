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
#include "asio/experimental/awaitable_operators.hpp"
#include "asio/experimental/cancellation_condition.hpp"
#include "asio/experimental/parallel_group.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
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

/// 超时等待协程完成
/// - timeout 使用毫秒精度, 秒级调用方可隐式转换 (秒->毫秒不损失精度, 反之则不能隐式转换)
template<typename T>
asio::awaitable<T> asyncWithTimeout(
    std::function<asio::awaitable<T>()> future,
    std::chrono::milliseconds           timeout,
    std::function<T()>                  onTimeout = nullptr
) {
    auto               ctx = co_await asio::this_coro::executor;
    asio::steady_timer timer(ctx, timeout);
    // 注意: 不能使用 `operator||` (其内部是 wait_for_one_success 语义) —— 当工作协程
    // 快速失败 (如 file_patterns 无匹配、文件读取错误) 时, 该语义不会立即返回, 而是继续
    // 等待另一个操作"成功" (定时器要等满 timeout 才成功), 导致真实错误被拖到满超时才
    // 返回 (表现为 "timed out")。因此这里改用 parallel_group + wait_for_one:
    // 协程先完成 (无论成功/失败) 都立即返回; 定时器仅在工作协程挂起过久时胜出。
    // - 本 asio 版本中 co_spawn(awaitable<T>) 的完成签名为 void(exception_ptr, T),
    //   定时器协程与 void 协程则仅携带 void(exception_ptr), 故结果元组元素个数随 T 变化,
    //   无法用固定的结构化绑定解构, 只能按索引 std::get 访问:
    //   [0]=completion_order, [1]=工作协程异常, [2]=工作协程结果(非 void 时), 末位=定时器异常
    auto groupResult
        = co_await asio::experimental::make_parallel_group(
              asio::co_spawn(ctx, future(), asio::deferred),
              asio::co_spawn(ctx, timer.async_wait(asio::use_awaitable), asio::deferred)
        )
              .async_wait(asio::experimental::wait_for_one(), asio::deferred);
    auto& order = std::get<0>(groupResult);

    if (order[0] == 0) {
        // 工作协程先完成 (无论成功/失败)
        if (std::get<1>(groupResult)) {
            std::rethrow_exception(std::get<1>(groupResult));
        }
        if constexpr (std::is_void_v<T>) {
            co_return;
        } else {
            co_return std::move(std::get<2>(groupResult));
        }
    }

    // 定时器先完成 -> 超时 (工作协程已被取消, 其结果/异常不再有意义)
    if (nullptr != onTimeout) {
        co_return onTimeout();
    }
    throw std::runtime_error{"[timeout]"};
}

} // namespace util
} // namespace agentxx
