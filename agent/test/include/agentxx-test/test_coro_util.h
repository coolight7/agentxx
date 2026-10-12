#pragma once

/// 测试用协程工具: 分离协程的闭包保活
///
/// 背景 (与 `agentxx::util::TaskScope::spawn` 同一条规则):
/// lambda 协程调用只把 **this 指针**存进协程帧, **闭包对象本身不入帧**。因此
/// ```cpp
/// asio::co_spawn(ex, [x]() -> asio::awaitable<void> { ... }, asio::detached);
/// ```
/// 里的临时闭包在**语句结束**时就析构了 (命名局部闭包则在它所在作用域结束时析构),
/// 之后被唤醒的协程再访问捕获量、或访问被捕获对象 (端点/传输/记录器) —— 都是访问
/// 已释放内存。表现为偶发 SIGSEGV (无输出、虚调用跳野指针) 或 ASan 报 use-after-free,
/// 且只在"分离协程比用例函数活得久"的时序下出现, 很难复现。

#include "asio/awaitable.hpp"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 分离运行一个协程, 并把可调用对象按值复制进协程帧 (闭包随协程存活)
///
/// - 可调用对象作为**按值协程参数**传入: 协程参数会复制进帧, 于是闭包以及它按值
///   捕获的 shared_ptr (端点 / 传输 / 记录器 / 共享状态) 活到协程结束
/// - 用例里的分离协程都应当这样启动; 只按 `asio::detached` 直接传闭包是不安全的
/// - 协程体在挂起点之后不要再用 `[&]` 捕获的用例栈局部量, 除非用例自身会等到它退出
///
/// 用法:
/// ```cpp
/// asio::co_spawn(
///     ex,
///     keepAliveRun([ep = fx->endpoint]() -> asio::awaitable<void> {
///         co_await ep->runTransportLoop();
///     }),
///     asio::detached
/// );
/// ```
template <class Fn>
asio::awaitable<void> keepAliveRun(Fn fn) {
    co_await fn();
}

} // namespace test
} // namespace agentxx
