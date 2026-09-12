#pragma once

#include "test_framework.h"

namespace agentxx::test {
/// 插件协程驱动桥测试（PollOneBridge / BridgeRoot / coroutine_runtime 协议）。
///
/// 覆盖设计文档第 10 节中与 kit 相关的完成条件：
/// - request_driver 永不内联回调；
/// - 每张请求恰好一次 poll_one；
/// - 空闲时请求数不增长（不自旋）；
/// - wake 在 driver 前/中/后三个窗口都不丢失；
/// - 宿主回调完成后不重入插件协程（先 post 到本地, 由下一次 driver 恢复）；
/// - 取消只产生一个终态；
/// - 宿主拒绝驱动时根被终结为失败；
/// - stop 取消排队请求并终结活跃根；
/// - 多实例互不影响。
TestResult testPluginBridge();
} // namespace agentxx::test
