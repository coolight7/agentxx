/*
 * agentxx/plugin/api/plugin_guard.h —— 插件侧 C ABI 边界异常守卫 (header-only)
 *
 * 命名空间: agentxx::plugin (历史命名空间 agentxx::plugin_guard 已并入)
 *
 * 定位: 与 plugin_iface_helper.h / plugin_tool_sync.h 同类 —— 纯头文件内联
 * 设施, 编译进插件本体; 【非跨边界 ABI】, 第三方插件可不用本头而自行 try/catch。
 *
 * 背景: 插件导出函数与注册回调 (get_info/entry/unload、工具 execute 三件套、
 * 钩子/事件/定时器/命令回调、offload work/done 等) 都是【宿主调用的 C ABI 函数】,
 * C++ 异常绝不能穿越该边界 (跨编译器/异常 ABI 不同时是 UB; 线程体内逃逸直接
 * std::terminate)。部分 C++ 操作会隐含抛异常 (std::bad_alloc /
 * std::filesystem::filesystem_error / JSON 解析 / std::stoi / std::thread 构造...),
 * 必须在每个边界统一拦截。
 *
 * 与宿主侧 XX_PLUGIN_CATCH_* (plugin_common.h, 宿主 vtable 兜底) 对称;
 * 与 AGENTS.md 的 catchError 系列 (放行取消/中断) 不冲突 —— 取消/中断语义由
 * 宿主 op_driver 层负责, 本边界处必须 catch(...) 全拦截。
 *
 * 用法 (函数式封装, 以 lambda 包裹原函数体取代旧 XX_PGUARD_* 宏):
 *   1. 插件翻译单元内定义 noexcept 日志可调用对象, 调用处按需传入:
 *      【多实例推荐形态】捕获本实例上下文的闭包 (日志归属精确到实例,
 *      零进程级全局 —— 同库可被多个宿主各自加载为独立实例):
 *        auto logFn = [ctx](const char* msg) noexcept { ctxLog(*ctx, msg); };
 *        return agentxx::plugin::guardCall(logFn, -1, [&]() -> int { ... });
 *      无实例边界的入口 (如 get_info, 纯静态元数据) 可传不捕获的静态
 *      函数或空操作闭包。
 *      旧式固定签名函数指针 void(*)(const char*) 仍可传入, 但其实现只能
 *      依赖进程级状态 —— 与多实例契约冲突, 仅限纯静态元数据边界使用。
 *   2. 在每个 C ABI 边界函数体内用守卫函数包裹原函数体 (lambda 捕获引用,
 *      体内提前 return 的值即边界返回值):
 *        // 有返回值: 异常时记日志并返回 fallback
 *        return agentxx::plugin::guardCall(pluginCatchLog, nullptr,
 *            [&]() -> const AgentxxPluginInfo* {
 *                ... 原函数体 ...
 *            });
 *        // 无返回值: 异常时仅记日志返回
 *        agentxx::plugin::guardCallVoid(pluginCatchLog, [&] {
 *            ... 原函数体 ...
 *        });
 *
 * 边界语义约定 (异常时的返回值):
 *   - get_info            → nullptr (宿主按"未导出"处理, 从库名推导插件名)
 *   - entry               → -1     (加载失败, 宿主走既有失败清理路径)
 *   - unload              → void   (吞掉 + 日志)
 *   - 工具/能力 start     → nullptr (映射 OP_FAILED; catch 路径禁止分配,
 *                            故不在此设置 error_out —— 错误文本由函数体内
 *                            自行 try/catch 设置, 见各工具 execute 实现)
 *   - poll                → AGENTXX_OP_POLL_DONE (终结请求; 与宿主 stepPoll
 *                            违约处理一致)
 *   - cancel / 事件 / 定时器 / offload done / 命令等 void 回调 → 吞掉 + 日志
 *
 * 安全约束: catch 块内禁止任何可能再抛异常的操作 (fmt/std::string/new),
 * 日志一律走栈缓冲 snprintf + 宿主 log 接口表; 接口缺失时静默丢弃。
 */
#ifndef AGENTXX_PLUGIN_GUARD_H
#define AGENTXX_PLUGIN_GUARD_H

#include "agentxx/plugin/api/client_plugin_api.h" /* 含 plugin_api.h (双端类型齐备) */

#include <cstdio>
#include <exception> /* std::exception */
#include <type_traits>
#include <utility>

#ifdef __cplusplus

namespace agentxx {
namespace plugin {
/* 注意: 历史命名空间 agentxx::plugin_guard 已并入 agentxx::plugin,
 * 与宿主侧 plugin 命名空间统一 (见 git 历史) */

/// 栈缓冲日志 (noexcept): "[插件名] exception: msg" 经宿主 log 接口表输出;
/// host/logIf 缺失时静默丢弃 (catch 路径不得再失败)
inline void logTo(
    const AgentxxPluginHost*     host,
    const AgentxxPluginLogIface* logIf,
    int                          level,
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
    logIf->log(host, level, agentxx_plugin_sv_cstr(buf));
}

inline void logTo(
    const AgentxxPluginHost*     host,
    const AgentxxPluginLogIface* logIf,
    int                          level,
    const char*                  pluginName,
    const char*                  msg
) noexcept {
    logTo(host, logIf, level, agentxx_plugin_sv_cstr(pluginName), agentxx_plugin_sv_cstr(msg));
}

/// client 侧宿主重载 (AgentxxPluginHost/AgentxxClientLogIface 为独立类型)
inline void logTo(
    const AgentxxPluginHost*     host,
    const AgentxxClientLogIface* logIf,
    int                          level,
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
    logIf->log(host, level, agentxx_plugin_sv_cstr(buf));
}

inline void logTo(
    const AgentxxPluginHost*     host,
    const AgentxxClientLogIface* logIf,
    int                          level,
    const char*                  pluginName,
    const char*                  msg
) noexcept {
    logTo(host, logIf, level, agentxx_plugin_sv_cstr(pluginName), agentxx_plugin_sv_cstr(msg));
}

/// 重抛检查当前异常并分类上报 (noexcept):
/// - std::exception 族上报 e.what() (含 bad_alloc/filesystem_error/runtime_error...)
/// - 其余 (非标准异常/纯 C 场景不可达) 上报 unknown
/// - logFn 须为 noexcept 可调用对象, 签名 void(const char*)
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

/// 有返回值的 C ABI 边界异常守卫 (取代旧 XX_PGUARD_BEGIN + XX_PGUARD_END_RET):
/// 正常执行返回 fn() 的结果; fn 抛异常时经 reportCurrentException 分类上报
/// logFn 并返回 fallback。整体 noexcept, 异常绝不外泄。
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

/// 无返回值的 C ABI 边界异常守卫 (取代旧 XX_PGUARD_BEGIN + XX_PGUARD_END_VOID):
/// 正常执行调用 fn(); fn 抛异常时经 reportCurrentException 分类上报 logFn 后
/// 返回 (吞掉)。整体 noexcept, 异常绝不外泄。
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

#endif /* __cplusplus */

#endif /* AGENTXX_PLUGIN_GUARD_H */
