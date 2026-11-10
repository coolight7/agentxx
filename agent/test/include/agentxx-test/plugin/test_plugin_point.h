#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 功能扩展点 (point) 的主程序侧
///
/// 一个扩展点 = 一个 id, 同时是能力 (实现面) 与同名广播频道 (通知面)。本模块只验证
/// 主程序侧的声明与分发基础: 声明表与 id 头一致、没有实现行时按"没意见"完成、
/// 通知面真的把载荷发给订阅者、取值型频道保留当前值。
/// 具体调用点 (轮次/模型/工具/压缩) 的接线在后续步骤里逐个加断言。
asio::awaitable<TestResult> run_plugin_point_tests();

} // namespace test
} // namespace agentxx
