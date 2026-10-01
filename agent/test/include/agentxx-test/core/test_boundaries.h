#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 架构边界检查 (ARC-1): 以源码与构建配置为输入, 静态检查分层约定
/// - client 不引用 lib 私有实现头 (`agent/lib/src/**`), 且 `agentxx/*` 引用都能
///   落到 lib/client 的公开 include 根
/// - 插件只引用 SDK 公开头 (`agentxx/plugin/api/*` + `agentxx/util/exception.h`)
///   与工具库公开头 (`pluginxx/*` / `utilxx*/*`), 不引用宿主内部头
/// - lib 不反向依赖 client
/// - 插件导出仍是入口白名单: 构建侧保留 version script / exported_symbols_list,
///   源码侧不手写 dllexport / visibility("default")
TestResult testBoundaries();

} // namespace test
} // namespace agentxx
