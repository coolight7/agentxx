#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 插件执行器 (插件独立线程) 的框架级用例
///
/// 验证: "主程序 → 插件"的每一类回调都跑在插件执行器上, 且 io 线程不再执行插件代码。
/// 覆盖:
/// 1. 派发落到插件线程, 同实例严格 FIFO;
/// 2. lease 在派发时取 (排队中也算在途), 实例进入关闭后新派发被丢弃;
/// 3. 回调耗时统计与超预算计数;
/// 4. `OpCore` 的 start / 完成回调 / 取消三处都在插件线程执行;
/// 5. 生命周期入口 (`awaitPluginLifecycle`, 即 start / stop / mount / unmount 的底座);
/// 6. 协程驱动票 (插件的 `request_driver`) 在插件线程推进;
/// 7. 广播订阅处理器在插件线程执行;
/// 8. 插件执行器停止后派发被记为 unschedulable, 不会悬挂。
asio::awaitable<TestResult> run_plugin_executor_tests();

} // namespace test
} // namespace agentxx
