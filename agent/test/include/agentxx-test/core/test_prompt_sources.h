#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>
#include <string>

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 运行时提示词来源注册表测试
///
/// 覆盖: 登记/撤销、排序、按会话隔离、刷新时机 (首次/压缩后/资源变更)、
/// 内容未变不动段落、失败与超时保留旧文本、会话回收、诊断快照
asio::awaitable<TestResult> run_prompt_source_tests();

} // namespace test
} // namespace agentxx
