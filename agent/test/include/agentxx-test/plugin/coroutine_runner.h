///
/// 内容:
/// - [runPluginCoroutine]: 在插件管理器的 io 执行器上把一个协程跑到结束 (测试用)
///
#pragma once

#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/execution/context.hpp"
#include "asio/io_context.hpp"
#include "asio/query.hpp"
#include "asio/use_future.hpp"

#include <chrono>
#include <future>
#include <thread>
#include <utility>

namespace agentxx {
namespace test {

/// 在给定执行器上把一个协程跑到结束 (**测试用**)
///
/// 启停 (`disable` / `enable`) 等表成员已经是协程: 普通测试函数要当场拿结果,
/// 就靠它把协程挂在插件管理器的 io 上跑, 同时由本线程推进那个 io。
///
/// 为什么要取 io_context 自己推进: 用例里 io 通常由测试线程驱动 (`poll` / `run_for`),
/// 没有另一条常驻线程; 协程里还会等插件线程的完成包, 因此循环 "poll + 让出" 直到就绪。
///
/// - `args`:
///     - [ex] 目标执行器 (一般传 `manager.ioExecutor()`)
///     - [awaiter] 要跑的协程
/// - `return` 协程的结果 (协程抛异常时重新抛出)
template<typename T>
T runPluginCoroutine(const asio::any_io_executor& ex, asio::awaitable<T> awaiter) {
    auto& io     = static_cast<asio::io_context&>(asio::query(ex, asio::execution::context));
    auto  future = asio::co_spawn(ex, std::move(awaiter), asio::use_future);
    while (future.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
        io.restart();
        io.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return future.get();
}

/// 同上, void 结果 (协程异常同样重新抛出)
inline void runPluginCoroutine(const asio::any_io_executor& ex, asio::awaitable<void> awaiter) {
    auto& io     = static_cast<asio::io_context&>(asio::query(ex, asio::execution::context));
    auto  future = asio::co_spawn(ex, std::move(awaiter), asio::use_future);
    while (future.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
        io.restart();
        io.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    future.get();
}

} // namespace test
} // namespace agentxx
