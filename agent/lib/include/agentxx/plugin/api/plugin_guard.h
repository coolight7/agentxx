/// agentxx 插件侧 C ABI 边界异常处理 (C++ header-only)
///
/// 分层 (插件框架已拆分为 cxx_pluginxx 独立工程):
/// - 通用部分位于 `pluginxx/kit/guard.h`: logTo / reportCurrentException /
///   guardCall / guardCallVoid —— 本头包含它并把名字引入 `agentxx::plugin`,
///   插件源码书写 `agentxx::plugin::guardCall` 与拆分前一致;
/// - client 侧日志表 (`agentxx.client.log`) 已删除, 两侧统一用插件框架的
///   `pluginxx.log` 表, 因此本头不再需要额外重载。
///
/// 定位: 纯头文件内联设施, 编译进插件本体;
/// 【非跨边界 ABI】, 第三方插件可不用本头而自行 try/catch。
#ifndef AGENTXX_PLUGIN_GUARD_H
#define AGENTXX_PLUGIN_GUARD_H

#include "agentxx/plugin/api/plugin_kit.h"
#include "pluginxx/kit/guard.h"

namespace agentxx {
namespace plugin {

using pluginxx::guardCall;
using pluginxx::guardCallVoid;
using pluginxx::logTo;
using pluginxx::reportCurrentException;

} // namespace plugin
} // namespace agentxx

#endif /* AGENTXX_PLUGIN_GUARD_H */
