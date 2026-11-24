#pragma once

/// 测试专用的「提交一次表调用并同步取结果」
///
/// 生产代码里插件侧不再有同步等待（一律协程 / 回调形态，见 `pluginxx::asyncTableOp`
/// 与 `pluginxx::tableOpCallback`）。用例里仍有很多同步函数需要"当场拿到结果"，
/// 这个头提供那个口子：提交 + 完成回调 + 条件变量等待，**只编进测试目标**。
///
/// 用法上的两条约束：
/// - **不能在插件执行线程上调用**：完成回调要投回那条线程，在那里等就是自己等自己。
///   生命周期入口这类跑在插件执行线程上的探针，请写成"提交 + 完成回调里 notify"
///   （参考 `test_plugin_runtime.cpp` 的 `lifecycleCreateHook`）；
/// - 主程序 IO 线程上可以调用：完成在插件执行线程派发，这边只是等一下通知。

#include "pluginxx/api/abi.h"
#include "pluginxx/api/broadcast_handle.h"
#include "pluginxx/kit/kit.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

namespace agentxx {
namespace test {

namespace detail {

/// 把 host / cancel / scheduler 收成 `tableOpCallback` 认得的上下文形状
struct TableCallCtx {
    const PluginxxHost* host = nullptr;

    struct Ifaces {
        const PluginxxCancelIface*    cancel    = nullptr;
        const PluginxxSchedulerIface* scheduler = nullptr;
    } iface;
};

/// 一次表调用的等待状态
struct TableCallWait {
    std::mutex                      mutex;
    std::condition_variable         cv;
    bool                            done = false;
    pluginxx::detail::TableOpResult result;
};

} // namespace detail

/// 提交一次表调用并等它完成
///
/// - 等超时返回 `accepted() == false`、`startError` 写明是测试等超时（不是主程序拒绝），
///   便于把"用例等太久"与"主程序没收"分开定位
template<typename StartFn>
pluginxx::detail::TableOpResult tableOpWait(
    const PluginxxHost*           host,
    const PluginxxCancelIface*    cancel,
    const PluginxxSchedulerIface* scheduler,
    StartFn&&                     start,
    std::chrono::milliseconds     timeout = std::chrono::seconds{15}
) {
    auto state = std::make_shared<detail::TableCallWait>();
    detail::TableCallCtx ctx;
    ctx.host            = host;
    ctx.iface.cancel    = cancel;
    ctx.iface.scheduler = scheduler;
    pluginxx::tableOpCallback(
        ctx,
        std::forward<StartFn>(start),
        [state](pluginxx::detail::TableOpResult r) {
            std::lock_guard lock(state->mutex);
            state->result = std::move(r);
            state->done   = true;
            state->cv.notify_all();
        }
    );
    std::unique_lock lock(state->mutex);
    if (!state->cv.wait_for(lock, timeout, [&state]() {
            return state->done;
        })) {
        pluginxx::detail::TableOpResult r;
        r.startError = "test: table call did not complete in time";
        return r;
    }
    return state->result;
}

/// 同上（从插件实例上下文取 host / cancel / scheduler）
template<typename Ctx, typename StartFn>
pluginxx::detail::TableOpResult tableOpWait(
    const Ctx&                ctx,
    StartFn&&                 start,
    std::chrono::milliseconds timeout = std::chrono::seconds{15}
) {
    return tableOpWait(
        ctx.host,
        ctx.iface.cancel,
        ctx.iface.scheduler,
        std::forward<StartFn>(start),
        timeout
    );
}

/// 订阅一条广播并等句柄 (用例用: 生产代码没有同步等待)
///
/// 订阅句柄只能经完成回调的 `payload` 交回, 所以提交后由 [tableOpWait] 等它到; 
/// 失败/被拒/超时返回 `nullptr`。
inline PluginxxSubscription* subscribeBroadcastWait(
    const PluginxxHost*            host,
    const PluginxxBroadcastsIface* broadcasts,
    const PluginxxScopeRef*        scope,
    std::string_view               id,
    void(PLUGINXX_CALL* handler)(const PluginxxStringView*, void*),
    void*                          ud
) {
    if (!host || !broadcasts || !broadcasts->subscribe_broadcast || id.empty() || !handler) {
        return nullptr;
    }
    const std::string idText = std::string{id};
    auto              result = tableOpWait(
        host,
        nullptr,
        nullptr,
        [&](PluginxxOperatorCallback cb, void* opUd, PluginxxString* err)
            -> PluginxxOperatorHandle* {
            auto idSv = pluginxx::PluginStringView::from(idText);
            return broadcasts
                ->subscribe_broadcast(host, scope, &idSv, handler, ud, nullptr, cb, opUd, err);
        }
    );
    if (!result.ok() || result.text().empty()) {
        return nullptr;
    }
    return pluginxx::pluginxxSubscriptionFromText(result.text());
}

/// 提交一次表调用, 完成时调 `done`（不等待）
///
/// 插件执行线程上用这个 —— 那里不能等自己的完成回调。实际插件写法是
/// `tableOpCallback` + 全部完成后回答 `notify`，这里只是把 host/cancel/scheduler
/// 收成上下文形状的同一件事。
template<typename StartFn, typename DoneFn>
void tableOpSubmit(
    const PluginxxHost*           host,
    const PluginxxCancelIface*    cancel,
    const PluginxxSchedulerIface* scheduler,
    StartFn&&                     start,
    DoneFn&&                      done
) {
    detail::TableCallCtx ctx;
    ctx.host            = host;
    ctx.iface.cancel    = cancel;
    ctx.iface.scheduler = scheduler;
    pluginxx::tableOpCallback(
        ctx,
        std::forward<StartFn>(start),
        std::forward<DoneFn>(done)
    );
}

} // namespace test
} // namespace agentxx
