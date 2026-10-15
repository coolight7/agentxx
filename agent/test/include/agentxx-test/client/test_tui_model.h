#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 客户端模型层: 历史分页窗口 + 本地消息条数上限判定 + 消息队列镜像(含投递回执记账)
/// - 三个模型都不依赖 FTXUI / 网络 / 会话, 可脱离终端直接单测
TestResult testTuiModels();

} // namespace test
} // namespace agentxx
