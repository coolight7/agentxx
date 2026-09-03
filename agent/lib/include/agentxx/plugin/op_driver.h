/*
 * agentxx/plugin/op_driver.h —— 插件异步操作宿主侧驱动器 (lib 内部实现, 非 ABI)
 */
#pragma once

#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/log.h"
#include "asio/as_tuple.hpp"
#include "asio/bind_cancellation_slot.hpp"
#include "asio/co_spawn.hpp"
#include "asio/deferred.hpp"
#include "asio/detached.hpp"
#include "asio/error.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "asio/experimental/parallel_group.hpp"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "boost/system/error_code.hpp"
#include "fmt/format.h"
#include "neograph/graph/cancel.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace agentxx {
namespace plugin {

struct OpDrive {
    std::function<void*(const AgentxxPluginOperatorNotify* notify, AgentxxPluginString* err)> start;
    std::function<void(void* op)>                                                             cancel;
};

using OpErrorCode = neograph_asio_error_code;

struct OpWatchdog {
    std::chrono::steady_clock::time_point t0{};
    const char*                           stage  = "";
    bool                                  warned = false;

    void enter(const char* in_stage) {
        stage = in_stage;
        t0    = std::chrono::steady_clock::now();
    }

    void exit(std::string_view pluginName, std::string_view label) {
        if (warned) {
            return;
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0
        )
                      .count();
        if (ms > kSlowCallWarnMs) {
            warned = true;
            XX_LOGW(
                "Plugin `{}` `{}` slow {}() call took {}ms (>{}ms, io thread stalled)",
                pluginName,
                label,
                stage,
                ms,
                kSlowCallWarnMs
            );
        }
    }

    static constexpr long long kSlowCallWarnMs = 100;
};

using OpGuardPtr = std::shared_ptr<PluginInstance::InflightGuard>;

struct OpCore : std::enable_shared_from_this<OpCore> {
    std::atomic<bool> notified{false};
    std::atomic<int>  status{AGENTXX_PLUGIN_OPERATOR_OK};
    std::string       payload;
    std::atomic<bool> cancelRequested{false};
    std::atomic<bool> cancelSent{false};

    asio::experimental::concurrent_channel<void(OpErrorCode)> chan;
    asio::cancellation_signal                                 doneSignal;
    OpGuardPtr                                                guard;
    AgentxxPluginOperatorCallback                             cb   = nullptr;
    void*                                                     cbUd = nullptr;

    explicit OpCore(const asio::any_io_executor& ex, OpGuardPtr g = nullptr) :
        chan(ex, 4),
        guard(std::move(g)) {}

    static void AGENTXX_PLUGIN_CALL onDone(void* ud, int32_t st, const AgentxxPluginStringView* payload_sv) {
        auto* self = static_cast<OpCore*>(ud);
        // 生命周期守卫: 插件的 done 可能来自任意线程 (阻塞池/自管线程), 调用方
        // 仅持裸指针。通知到达时宿主侧至少有一个等待协程 (chan/doneSignal 等待
        // 或 sentinelReap) 仍持有 shared_ptr, 故此处 shared_from_this 必然成功;
        // 自持引用保证本函数执行期间 (含 guard.reset / 回调派发) 对象存活 ——
        // 否则 io 线程等待协程收到通知后会退出并释放最后一个引用, 与 onDone
        // 剩余代码并发访问 self 构成 use-after-free
        std::shared_ptr<OpCore> selfKeep;
        try {
            selfKeep = self->shared_from_this();
        } catch (const std::bad_weak_ptr&) {
            return;
        }
        bool expect = false;
        if (!self->notified.compare_exchange_strong(expect, true, std::memory_order_acq_rel)) {
            return;
        }
        self->status.store(st, std::memory_order_release);
        if (payload_sv && payload_sv->data && payload_sv->size > 0) {
            self->payload.assign(payload_sv->data, static_cast<size_t>(payload_sv->size));
        } else {
            self->payload.clear();
        }
        self->chan.try_send(OpErrorCode());
        self->doneSignal.emit(asio::cancellation_type::all);
        self->guard.reset();
        if (self->cb) {
            auto  cb                = self->cb;
            auto* cbUd              = self->cbUd;
            self->cb                = nullptr;
            std::string payloadCopy = self->payload;
            // 宿主约定：完成回调在 io 线程派发且经 post 入队，禁止同步重入；
            // 插件的 done 可能来自任意线程（阻塞池/自管线程），此处恒异步投递回 io
            // (selfKeep 已自持, post 回调也捕获它, 派发期间对象必然存活)
            asio::post(self->chan.get_executor(), [selfKeep, cb, cbUd, st, payloadCopy]() {
                try {
                    auto payloadSv = agentxx::plugin::PluginStringView::from(payloadCopy.data(), payloadCopy.size());
                    cb(cbUd, st, &payloadSv);
                } catch (...) {
                }
            });
        }
    }

    AgentxxPluginOperatorNotify notify() {
        return AgentxxPluginOperatorNotify{&OpCore::onDone, this};
    }
};

namespace detail {

inline void safeCancelOnce(OpCore& core, const OpDrive& d, void* op) {
    if (!core.cancelSent.exchange(true, std::memory_order_acq_rel)) {
        if (!d.cancel) {
            return;
        }
        try {
            d.cancel(op);
        } catch (const std::exception& e) {
            XX_LOGW("plugin op cancel() threw: {}", e.what());
        } catch (...) {
            XX_LOGW("plugin op cancel() threw unknown exception");
        }
    }
}

/// 自动回收调用方 outstandingOps 里的宿主托管句柄 (io 线程):
/// 操作终态后 (doneSignal) 把句柄从列表移除, 避免悬垂 handle 在后续 unload
/// 时触发 UAF。零轮询: 等待 doneSignal 事件 (与 awaitPluginOp 的 sentinel
/// 协程不竞争 chan —— 各自独立通知通道)。
/// - 被 callToolAsync / invokeCapabilityAsync / registerTask 三处共用
/// - core 持 shared_ptr 直到 notify 到达 (onDone 内 guard.reset +
///   doneSignal.emit), 故 handle 在列表中存留期间即使 detachAll 并发 cancel
///   也安全 (cancelFn 转发给已完成的 op 无副作用; erase 与 detachAll 的
///   clear 同在 io 线程串行)
/// - 与 waitInflightZero 无因果: 计数归零由 OpCore::onDone 的 guard.reset
///   原子完成 (通知即归零), 本协程只负责"收尾整洁"
inline void spawnHandleReaper(
    const asio::any_io_executor&                      ex,
    const std::shared_ptr<OpCore>&                    core,
    const std::weak_ptr<PluginInstance>&              weakCaller,
    const std::weak_ptr<AgentxxPluginOperatorHandle>& weakHandle
) {
    asio::co_spawn(
        ex,
        [core, weakCaller, weakHandle]() -> asio::awaitable<void> {
            if (!core->notified.load(std::memory_order_acquire)) {
                asio::steady_timer t(co_await asio::this_coro::executor);
                t.expires_at(std::chrono::steady_clock::time_point::max());
                co_await t.async_wait(asio::bind_cancellation_slot(
                    core->doneSignal.slot(),
                    asio::as_tuple(asio::use_awaitable)
                ));
            }
            // 确保在 io 线程执行移除 (caller 的 vector 非线程安全)
            auto callerSp = weakCaller.lock();
            auto handleSp = weakHandle.lock();
            if (!callerSp || !handleSp) {
                co_return;
            }
            auto& vec = callerSp->outstandingOps;
            vec.erase(
                std::remove_if(
                    vec.begin(),
                    vec.end(),
                    [&handleSp](const std::shared_ptr<AgentxxPluginOperatorHandle>& h) {
                        return h == handleSp;
                    }
                ),
                vec.end()
            );
        },
        asio::detached
    );
}

inline asio::awaitable<void> sentinelReap(
    std::shared_ptr<OpCore> core,
    OpDrive                 drive,
    void*                   op,
    OpGuardPtr              guard,
    std::string             name,
    std::string             label
) {
    (void)drive;
    (void)op;
    XX_LOGD("Plugin `{}` abandoned op `{}`, sentinel parking until done", name, label);
    while (!core->notified.load(std::memory_order_acquire)) {
        auto [ec] = co_await core->chan.async_receive(asio::as_tuple(asio::use_awaitable));
        (void)ec;
    }
    guard.reset();
    XX_LOGD("Plugin `{}` abandoned op `{}` finalized", name, label);
}

} // namespace detail

struct PluginOpAwaitArgs {
    std::shared_ptr<PluginInstance>               inst;
    std::string                                   label;
    asio::any_io_executor                         ex;
    std::shared_ptr<neograph::graph::CancelToken> cancelToken;
    OpDrive                                       drive;
};

inline asio::awaitable<std::string> awaitPluginOp(PluginOpAwaitArgs args) {
    auto        guard = std::make_shared<PluginInstance::InflightGuard>(args.inst.get());
    auto        core  = std::make_shared<OpCore>(args.ex, guard);
    std::string name  = args.inst ? args.inst->name : std::string{};
    std::string label = args.label;
    OpWatchdog  wd;

    std::shared_ptr<neograph::graph::CancelToken> cancelOp;
    if (args.cancelToken) {
        cancelOp = args.cancelToken->fork();
        cancelOp->bind_executor(args.ex);
        if (cancelOp->is_cancelled()) {
            core->cancelRequested.store(true, std::memory_order_release);
        }
    }

    AgentxxPluginString         err{nullptr, 0};
    void*                       op         = nullptr;
    bool                        startThrew = false;
    AgentxxPluginOperatorNotify ntf        = core->notify();
    wd.enter("start");
    try {
        op = args.drive.start(&ntf, &err);
    } catch (...) {
        startThrew = true;
    }
    wd.exit(name, label);

    if (startThrew || err.data != nullptr
        || (op == nullptr && !core->notified.load(std::memory_order_acquire))) {
        guard.reset();
        std::string msg;
        if (err.data != nullptr) {
            msg.assign(err.data, err.size);
            if (args.inst) {
                agentxx::plugin::PluginString::free(&args.inst->host, &err);
            } else {
                ::free(err.data);
                err.data = nullptr;
                err.size = 0;
            }
        } else if (startThrew) {
            msg = "plugin start() threw";
        } else {
            msg = "protocol violation: null op without notify/error";
        }
        throw std::runtime_error(fmt::format("plugin `{}` op {} failed: {}", name, label, msg));
    }

    if (cancelOp && cancelOp->is_cancelled()) {
        core->cancelRequested.store(true, std::memory_order_release);
    }
    if (core->cancelRequested.load(std::memory_order_acquire)) {
        detail::safeCancelOnce(*core, args.drive, op);
    }

    // 零轮询事件驱动取消：通过 watcher 协程监听 cancelOp->slot() 信号
    std::shared_ptr<asio::steady_timer> cancelWatcherTimer;
    std::shared_ptr<std::atomic<bool>>  cancelWatcherDone;
    if (cancelOp && !core->cancelRequested.load(std::memory_order_acquire)
        && !core->notified.load(std::memory_order_acquire)) {
        cancelWatcherDone  = std::make_shared<std::atomic<bool>>(false);
        cancelWatcherTimer = std::make_shared<asio::steady_timer>(args.ex);
        cancelWatcherTimer->expires_at(std::chrono::steady_clock::time_point::max());
        asio::co_spawn(
            args.ex,
            [core, drive = args.drive, op, cancelOp, cancelWatcherDone, cancelWatcherTimer](
            ) -> asio::awaitable<void> {
                auto _ = co_await cancelWatcherTimer->async_wait(asio::bind_cancellation_slot(
                    cancelOp->slot(),
                    asio::as_tuple(asio::use_awaitable)
                ));
                if (cancelWatcherDone->load(std::memory_order_acquire)) {
                    co_return;
                }
                if (cancelOp->is_cancelled()) {
                    core->cancelRequested.store(true, std::memory_order_release);
                    detail::safeCancelOnce(*core, drive, op);
                }
            },
            asio::detached
        );
    }

    std::exception_ptr abort;
    try {
        while (!core->notified.load(std::memory_order_acquire)) {
            auto [ec] = co_await core->chan.async_receive(asio::as_tuple(asio::use_awaitable));
            (void)ec;
        }
    } catch (...) {
        abort = std::current_exception();
    }

    if (cancelWatcherDone) {
        cancelWatcherDone->store(true, std::memory_order_release);
        if (cancelWatcherTimer) {
            cancelWatcherTimer->cancel();
        }
    }

    if (abort) {
        detail::safeCancelOnce(*core, args.drive, op);
        asio::co_spawn(
            args.ex,
            detail::sentinelReap(core, std::move(args.drive), op, std::move(guard), name, label),
            asio::detached
        );
        std::rethrow_exception(abort);
    }

    guard.reset();
    int         st      = core->status.load(std::memory_order_acquire);
    std::string payload = std::move(core->payload);
    if (st == AGENTXX_PLUGIN_OPERATOR_CANCELLED) {
        throw neograph::graph::CancelledException(fmt::format("plugin op `{}` cancelled", label));
    }
    if (st != AGENTXX_PLUGIN_OPERATOR_OK) {
        throw std::runtime_error(
            payload.empty() ? fmt::format("plugin op `{}` failed", label) : payload
        );
    }
    co_return payload;
}

} // namespace plugin
} // namespace agentxx
