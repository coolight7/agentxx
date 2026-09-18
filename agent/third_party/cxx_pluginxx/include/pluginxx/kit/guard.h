/// 插件侧 C ABI 边界异常处理 (C++ header-only, 与宿主领域无关)
///
/// 归属: cxx_pluginxx (插件框架内核)。
///
/// 定位: 纯头文件内联设施, 编译进插件本体;
/// 【非跨边界 ABI】, 第三方插件可不用本头而自行 try/catch。
///
/// 内容:
/// - [logTo]: 把边界异常经宿主日志接口表输出 (noexcept; 日志缺失时静默丢弃);
/// - [reportCurrentException]: 重抛检查当前异常并分类上报;
/// - [guardCall] / [guardCallVoid]: 有/无返回值的 C ABI 边界守卫 (异常 → 上报 + 回退值)。
///
/// 宿主领域部分 (client 侧日志接口表的重载) 位于宿主仓库: agentxx 侧见
/// [plugin_guard.h](/agent/lib/include/agentxx/plugin/api/plugin_guard.h), 该头包含本头并把
/// 通用名引入 `agentxx::plugin`。
#pragma once
#include "pluginxx/kit/kit.h"

#include <cstdio>
#include <exception>
#include <string_view>
#include <type_traits>
#include <utility>

namespace pluginxx {

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
///   return pluginxx::guardCall(pluginCatchLog, nullptr,
///       [&]() -> const AgentxxPluginInfo* { ... });
template<typename LogFn, typename Fn>
[[nodiscard]] inline auto
    guardCall(LogFn&& logFn, std::invoke_result_t<Fn&> fallback, Fn&& fn) noexcept
    -> std::invoke_result_t<Fn&> {
    using Ret = std::invoke_result_t<Fn&>;
    static_assert(!std::is_void_v<Ret>, "void callable: use pluginxx::guardCallVoid");
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
///   pluginxx::guardCallVoid(pluginCatchLog, [&] { ... });
template<typename LogFn, typename Fn>
inline void guardCallVoid(LogFn&& logFn, Fn&& fn) noexcept {
    try {
        std::forward<Fn>(fn)();
    } catch (...) {
        reportCurrentException(std::forward<LogFn>(logFn));
    }
}

} // namespace pluginxx
