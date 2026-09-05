/// 插件侧 C ABI 边界异常处理 (header-only)
///
/// 命名空间: agentxx::plugin
///
/// 定位: 纯头文件内联设施, 编译进插件本体;
/// 【非跨边界 ABI】, 第三方插件可不用本头而自行 try/catch。
/// (接口表聚合 AgentIfaces/ClientIfaces 与同步工具适配器均已并入
/// [plugin_kit.h](/agent/lib/include/agentxx/plugin/api/plugin_kit.h))
#ifndef AGENTXX_PLUGIN_GUARD_H
#define AGENTXX_PLUGIN_GUARD_H

#include "agentxx/plugin/api/plugin_kit.h"

#include <cstdio>
#include <exception>
#include <string_view>
#include <type_traits>
#include <utility>

namespace agentxx {
namespace plugin {

/// 栈缓冲日志 (noexcept): "[插件名] exception: msg" 经宿主 log 接口表输出;
/// host/logIf 缺失时静默丢弃 (catch 路径不得再失败)
inline void logTo(
    const AgentxxPluginHost*     host,
    const AgentxxPluginLogIface* logIf,
    int32_t                      level,
    AgentxxPluginStringView      pluginName,
    AgentxxPluginStringView      msg
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
    AgentxxPluginStringView sv = PluginStringView::fromCstr(buf);
    logIf->log(host, level, &sv);
}

inline void logTo(
    const AgentxxPluginHost*     host,
    const AgentxxPluginLogIface* logIf,
    int32_t                      level,
    std::string_view             pluginName,
    std::string_view             msg
) noexcept {
    logTo(host, logIf, level, PluginStringView::from(pluginName), PluginStringView::from(msg));
}

inline void logTo(
    const AgentxxPluginHost*     host,
    const AgentxxPluginLogIface* logIf,
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

/// client 侧宿主重载 (AgentxxPluginHost/AgentxxClientLogIface 为独立类型)
inline void logTo(
    const AgentxxPluginHost*     host,
    const AgentxxClientLogIface* logIf,
    int32_t                      level,
    AgentxxPluginStringView      pluginName,
    AgentxxPluginStringView      msg
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
    AgentxxPluginStringView sv = PluginStringView::fromCstr(buf);
    logIf->log(host, level, &sv);
}

inline void logTo(
    const AgentxxPluginHost*     host,
    const AgentxxClientLogIface* logIf,
    int32_t                      level,
    std::string_view             pluginName,
    std::string_view             msg
) noexcept {
    logTo(host, logIf, level, PluginStringView::from(pluginName), PluginStringView::from(msg));
}

inline void logTo(
    const AgentxxPluginHost*     host,
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

/// 重抛检查当前异常并分类上报 (noexcept):
template<typename LogFn>
inline void reportCurrentException(LogFn&& logFn) noexcept {
    try {
        throw;
    } catch (const std::exception& e) {
        logFn(e.what());
    } catch (...) {
        logFn("unknown non-standard exception");
    }
}

/// 有返回值的 C ABI 边界异常处理:
/// 正常执行返回 fn() 的结果; fn 抛异常时经 reportCurrentException 分类上报
/// logFn 并返回 fallback
///
/// 用法 (fallback 类型须可转换为 fn 的返回类型; lambda 返回类型建议显式标注):
///   return agentxx::plugin::guardCall(pluginCatchLog, nullptr,
///       [&]() -> const AgentxxPluginInfo* { ... });
template<typename LogFn, typename Fn>
[[nodiscard]] inline auto
    guardCall(LogFn&& logFn, std::invoke_result_t<Fn&> fallback, Fn&& fn) noexcept
    -> std::invoke_result_t<Fn&> {
    using Ret = std::invoke_result_t<Fn&>;
    static_assert(!std::is_void_v<Ret>, "void callable: use agentxx::plugin::guardCallVoid");
    try {
        return fn();
    } catch (...) {
        reportCurrentException(std::forward<LogFn>(logFn));
        return static_cast<Ret>(std::move(fallback));
    }
}

/// 无返回值的 C ABI 边界异常处理:
/// 正常执行调用 fn(); fn 抛异常时经 reportCurrentException 分类上报 logFn 后
/// 返回 (吞掉)
///
/// 用法:
///   agentxx::plugin::guardCallVoid(pluginCatchLog, [&] { ... });
template<typename LogFn, typename Fn>
inline void guardCallVoid(LogFn&& logFn, Fn&& fn) noexcept {
    try {
        std::forward<Fn>(fn)();
    } catch (...) {
        reportCurrentException(std::forward<LogFn>(logFn));
    }
}

} // namespace plugin
} // namespace agentxx

#endif /* AGENTXX_PLUGIN_GUARD_H */
