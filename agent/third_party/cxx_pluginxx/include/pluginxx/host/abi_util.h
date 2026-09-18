/// pluginxx 宿主侧 ABI 辅助 (跨边界字符串转换 / 异常兜底 / io 线程同步投递)
///
/// 内容:
/// - [svToSv] / [svToStr] / [strToSv] / [pluginStringView2std]: C ABI 字符串视图与
///   C++ 字符串视图/字符串之间的零拷贝或有拷贝转换;
/// - [guardVtableCall] / [guardVtableCallVoid]: C ABI 边界异常兜底 —— vtable 函数
///   内部不得让 C++ 异常逃逸 (跨边界 UB), 统一捕获转日志并按失败返回值返回;
/// - [ioCallSync] / [ioCallSyncVoid] / [ioCallSyncKeep] / [ioCallSyncVoidKeep]:
///   跨线程投递到宿主 io 线程并同步等待结果 (duck typing: Mgr 须提供
///   `isIoThread()` / `postToIo()`)。
///
/// 线程约定: 本头内非模板函数均可在任意线程调用 (纯函数); 模板函数按调用方
/// 管理器的线程模型工作。
#ifndef PLUGINXX_HOST_ABI_UTIL_H
#define PLUGINXX_HOST_ABI_UTIL_H

#include "pluginxx/api/abi.h"
#include "utilxx_base/log.h"

#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace pluginxx {

/// C ABI 字符串视图 → std::string_view (**零拷贝**)
/// - 视图指针为空时返回空视图 (**非**空指针解引用)
inline std::string_view svToSv(AgentxxPluginStringView sv) noexcept {
    return sv.data ? std::string_view{sv.data, static_cast<size_t>(sv.size)} : std::string_view{};
}

/// C ABI 字符串视图指针 → std::string_view (**零拷贝**)
/// - 指针或视图为空时返回空视图
inline std::string_view svToSv(const AgentxxPluginStringView* sv) noexcept {
    return (sv && sv->data) ? std::string_view{sv->data, static_cast<size_t>(sv->size)}
                            : std::string_view{};
}

/// C ABI 字符串视图 → std::string (**拷贝**)
inline std::string svToStr(AgentxxPluginStringView sv) {
    return sv.data ? std::string{sv.data, static_cast<size_t>(sv.size)} : std::string{};
}

/// C ABI 字符串视图指针 → std::string (**拷贝**)
inline std::string svToStr(const AgentxxPluginStringView* sv) {
    return (sv && sv->data) ? std::string{sv->data, static_cast<size_t>(sv->size)} : std::string{};
}

/// std::string_view → C ABI 字符串视图 (**借用, 不复制**)
/// - 调用方须保证被引用内存比视图长命 (跨边界只读入参场景)
inline AgentxxPluginStringView strToSv(std::string_view sv) noexcept {
    return AgentxxPluginStringView{sv.data(), static_cast<uint64_t>(sv.size())};
}

/// C ABI 字符串视图 → std::string_view (实现见 src/manifest.cpp)
std::string_view pluginStringView2std(AgentxxPluginStringView str);

/// ==================== C ABI 边界异常兜底 ====================
namespace detail {
template<typename Fn, typename Ret>
inline Ret guardVtableCallImpl(Ret fallback, Fn&& fn) noexcept {
    try {
        return fn();
    } catch (const std::exception& e) {
        XX_LOGE("plugin vtable exception: {}", e.what());
        return fallback;
    } catch (...) {
        XX_LOGE("plugin vtable unknown exception");
        return fallback;
    }
}

template<typename Fn>
inline void guardVtableCallVoidImpl(Fn&& fn) noexcept {
    try {
        fn();
    } catch (const std::exception& e) {
        XX_LOGE("plugin vtable exception: {}", e.what());
    } catch (...) {
        XX_LOGE("plugin vtable unknown exception");
    }
}
} // namespace detail

/// 边界异常兜底: 执行 fn, 异常时记日志并返回 fallback
/// - 用法: `return pluginxx::guardVtableCall(-1, [&]() { ... });`
template<
    typename Ret,
    typename Fn,
    typename = std::enable_if_t<!std::is_same_v<std::decay_t<Ret>, std::nullptr_t>>>
inline Ret guardVtableCall(Ret fallback, Fn&& fn) noexcept {
    return detail::guardVtableCallImpl<Fn, Ret>(std::move(fallback), std::forward<Fn>(fn));
}

/// std::nullptr_t 重载: fallback 为 nullptr 时按 lambda 返回类型推导指针类型
template<typename Fn>
inline auto guardVtableCall(std::nullptr_t, Fn&& fn) noexcept -> std::invoke_result_t<Fn> {
    using Ret = std::invoke_result_t<Fn>;
    try {
        return fn();
    } catch (const std::exception& e) {
        XX_LOGE("plugin vtable exception: {}", e.what());
        if constexpr (std::is_pointer_v<Ret>) {
            return Ret(nullptr);
        } else {
            return Ret{};
        }
    } catch (...) {
        XX_LOGE("plugin vtable unknown exception");
        if constexpr (std::is_pointer_v<Ret>) {
            return Ret(nullptr);
        } else {
            return Ret{};
        }
    }
}

/// 边界异常兜底 (void 返回): 异常只记日志
template<typename Fn>
inline void guardVtableCallVoid(Fn&& fn) noexcept {
    detail::guardVtableCallVoidImpl(std::forward<Fn>(fn));
}

/// ==================== io 线程同步投递 ====================

/// 在 io 线程执行并同步等待结果 (调用方为 io 线程时直接执行)
/// - 供 vtable 的 io 线程约束操作跨线程调用 (JS 线程/宿主线程池) 使用;
///   调用方线程阻塞等待, io 线程为事件循环 (挂起而非忙等), 无死锁风险
/// - Mgr 须提供 isIoThread()/postToIo(); T 为第一个显式模板参数, Mgr 推导
template<typename T, typename Mgr>
T ioCallSync(Mgr* mgr, std::function<T()> fn) {
    if (!mgr) {
        throw std::runtime_error("plugin manager released");
    }
    if (mgr->isIoThread()) {
        return fn();
    }
    auto p   = std::make_shared<std::promise<T>>();
    auto fut = p->get_future();
    mgr->postToIo([p, fn = std::move(fn)]() {
        try {
            p->set_value(fn());
        } catch (...) {
            p->set_exception(std::current_exception());
        }
    });
    return fut.get();
}

/// ioCallSync 的 void 特化
template<typename Mgr>
void ioCallSyncVoid(Mgr* mgr, std::function<void()> fn) {
    if (!mgr) {
        return;
    }
    if (mgr->isIoThread()) {
        fn();
        return;
    }
    auto p   = std::make_shared<std::promise<void>>();
    auto fut = p->get_future();
    mgr->postToIo([p, fn = std::move(fn)]() {
        try {
            fn();
            p->set_value();
        } catch (...) {
            p->set_exception(std::current_exception());
        }
    });
    fut.get();
}

/// ioCallSync + 保活: 把 `keep` 复制进投递闭包, 闭包执行期间额外持有一份
/// 引用与 admission lease。
///
/// 用途: vtable 入口在调用方线程解析出实例/管理器后投递到 IO 线程。若只把原始
/// 指针放进闭包, 请求可能在实例卸载 (dlclose) 之后才被执行; `keep` 通常就是
/// [pluginxx::PluginHostCall], 携带实例/管理器强引用与 admission lease, 使卸载的
/// idle 等待覆盖"已排队但尚未执行"的阶段。
///
/// - `keep` 本身不参与调用, 只为延长生命周期;
/// - 其余语义与 [ioCallSync] 完全一致。
template<typename T, typename Keep, typename Mgr>
T ioCallSyncKeep(Keep keep, Mgr* mgr, std::function<T()> fn) {
    if (!mgr) {
        throw std::runtime_error("plugin manager released");
    }
    if (mgr->isIoThread()) {
        return fn();
    }
    auto p   = std::make_shared<std::promise<T>>();
    auto fut = p->get_future();
    mgr->postToIo([keep, p, fn = std::move(fn)]() {
        try {
            p->set_value(fn());
        } catch (...) {
            p->set_exception(std::current_exception());
        }
    });
    return fut.get();
}

/// ioCallSyncKeep 的 void 特化
template<typename Keep, typename Mgr>
void ioCallSyncVoidKeep(Keep keep, Mgr* mgr, std::function<void()> fn) {
    if (!mgr) {
        return;
    }
    if (mgr->isIoThread()) {
        fn();
        return;
    }
    auto p   = std::make_shared<std::promise<void>>();
    auto fut = p->get_future();
    mgr->postToIo([keep, p, fn = std::move(fn)]() {
        try {
            fn();
            p->set_value();
        } catch (...) {
            p->set_exception(std::current_exception());
        }
    });
    fut.get();
}

} // namespace pluginxx

#endif /* PLUGINXX_HOST_ABI_UTIL_H */
