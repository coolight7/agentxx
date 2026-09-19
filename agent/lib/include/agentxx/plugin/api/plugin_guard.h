/// agentxx 插件侧 C ABI 边界异常处理 (C++ header-only) —— agentxx 侧补充
///
/// 分层 (框架内核已拆分为 cxx_pluginxx 独立工程):
/// - **通用部分**位于 `pluginxx/kit/guard.h`: logTo (agent 侧日志接口表) /
///   reportCurrentException / guardCall / guardCallVoid —— 本头包含它并把名字引入
///   `agentxx::plugin`, 插件源码书写 `agentxx::plugin::guardCall` 与拆分前一致;
/// - **本头只补 client 领域部分**: client 插件使用的 `AgentxxClientLogIface`
///   (与 agent 侧日志表为独立类型) 之上的 logTo 重载。
///
/// 定位: 纯头文件内联设施, 编译进插件本体;
/// 【非跨边界 ABI】, 第三方插件可不用本头而自行 try/catch。
#ifndef AGENTXX_PLUGIN_GUARD_H
#define AGENTXX_PLUGIN_GUARD_H

#include "agentxx/plugin/api/plugin_kit.h"
#include "pluginxx/kit/guard.h"

#include <cstdio>
#include <exception>
#include <string_view>
#include <type_traits>
#include <utility>

namespace agentxx {
namespace plugin {

/* ==================== 通用名引入 (pluginxx/kit/guard.h) ==================== */

using pluginxx::guardCall;
using pluginxx::guardCallVoid;
using pluginxx::logTo;
using pluginxx::reportCurrentException;

/* ==================== client 侧日志接口表重载 ====================
 *
 * client 插件使用 `AgentxxClientLogIface` (与 agent 侧日志表为独立类型),
 * 故在 agentxx::plugin 内补一组同语义重载; 与上面 using 引入的重载共同参与
 * 重载决议, 调用点写法与拆分前完全一致。
 */

inline void logTo(
    const PluginxxHost*     host,
    const AgentxxClientLogIface* logIf,
    int32_t                      level,
    PluginxxStringView      pluginName,
    PluginxxStringView      msg
) noexcept {
    if (!host || !logIf || !logIf->log || !msg.data) {
        return;
    }
    char buf[512];
    std::snprintf(
        buf,
        sizeof(buf),
        "[%.*s] exception: %.*s",
        static_cast<int>(pluginName.size),
        pluginName.data ? pluginName.data : "plugin",
        static_cast<int>(msg.size > 460 ? 460 : msg.size),
        msg.data
    );
    PluginxxStringView sv = PluginStringView::fromCstr(buf);
    logIf->log(host, level, &sv);
}

inline void logTo(
    const PluginxxHost*     host,
    const AgentxxClientLogIface* logIf,
    int32_t                      level,
    std::string_view             pluginName,
    std::string_view             msg
) noexcept {
    logTo(host, logIf, level, PluginStringView::from(pluginName), PluginStringView::from(msg));
}

inline void logTo(
    const PluginxxHost*     host,
    const AgentxxClientLogIface* logIf,
    int32_t                      level,
    const char*                  pluginName,
    const char*                  msg
) noexcept {
    logTo(
        host,
        logIf,
        level,
        PluginStringView::fromCstr(pluginName),
        PluginStringView::fromCstr(msg)
    );
}

} // namespace plugin
} // namespace agentxx

#endif /* AGENTXX_PLUGIN_GUARD_H */
