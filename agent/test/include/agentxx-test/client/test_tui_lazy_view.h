#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 懒构建列表视口测试 (LazyScrollable: 每帧成本与列表长度无关 / 前插稳定 /
/// 实测高度精确 / 滚动边界 / 估算偏差不影响视口内容)
TestResult testTuiLazyView();

} // namespace test
} // namespace agentxx
