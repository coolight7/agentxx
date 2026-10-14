#pragma once

#include <asio/awaitable.hpp>

#include "agentxx-test/test_framework.h"

namespace asio = ::boost::asio;

namespace agentxx {
namespace test {

/// agentxx_llm_uncensored 插件 (附加系统提示词注入)
///
/// 覆盖: 默认文本注入 (并确认确实进了拼好的系统提示词)、插件参数覆盖
/// (`text` / `key`)、空参数按未配置处理、禁用摘除与启用恢复、卸载回到基线、
/// 与用户直写的附加段并存互不影响。
asio::awaitable<TestResult> run_llm_uncensored_tests();

} // namespace test
} // namespace agentxx
