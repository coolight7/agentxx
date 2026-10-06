#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// 插件注册可逆、独占 slot 与教学式错误 (计划 PLG-1 / PLG-4 / PLG-7)
///
/// 覆盖: 统一注册清单在装载/禁用/启用/卸载四个状态下的取值、提示词贡献的多
/// owner 重算、执行图定义独占 slot 的占用与释放、装载失败建议文案。
asio::awaitable<TestResult> run_plugin_cleanup_tests();

} // namespace test
} // namespace agentxx
