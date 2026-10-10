#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 功能点子系统测试 (阶段 1: 点 / 实现 / 调用三层骨架)
/// - 顺序与默认优先级带 (插件 0 / 宿主 1000) 与越界裁剪
/// - 异常隔离 / 显式置空 / 值缓存三策略 / in-flight 去重 / 按来源失效
/// - 调用保护 (caller 入载荷 / 全部是自己就不问插件层 / 重入 busy)
/// - 两处超时 (implTimeoutMs / 调用方 timeoutMs; 默认都不限)
/// - 清单字段与开发者模式门控
boost::asio::awaitable<TestResult> run_feature_points_tests();

} // namespace test
} // namespace agentxx
