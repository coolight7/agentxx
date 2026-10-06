#pragma once

#include <asio/awaitable.hpp>
#include <neograph/api.h>

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 会话管理用例 (计划 RET-1a / STO-12b): 会话列表检索与改名的协议与端点处理
/// - 重命名: 写入 meta.title + 来源为用户; 空标题/会话不存在/无持久化各回原因
/// - 检索: 关键词命中标题或正文, 命中片段随列表项回传; 不续取分页
asio::awaitable<TestResult> run_session_admin_tests();

} // namespace test
} // namespace agentxx
