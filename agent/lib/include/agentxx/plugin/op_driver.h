/*
 * agentxx/plugin/op_driver.h —— 插件异步操作宿主侧驱动器 (lib 内部实现, 非 ABI)
 */
#pragma once

#include "agentxx/plugin/plugin_api.h"
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
    std::function<void*(const AgentxxPluginOperatorNotify* notify, char** err)> start;
    std::function<void(void* op)>                                               cancel;
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

    static void onDone(void* ud, int st, char* payload_cstr) {
        auto* self   = static_cast<OpCore*>(ud);
        bool  expect = false;
        if (!self->notified.compare_exchange_strong(expect, true, std::memory_order_acq_rel)) {
            if (payload_cstr) {
                ::free(payload_cstr);
            }
            return;
        }
        self->status.store(st, std::memory_order_release);
        if (payload_cstr) {
            self->payload.assign(payload_cstr);
            ::free(payload_cstr);
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
            try {
                auto selfKeep = self->shared_from_this();
                asio::post(self->chan.get_executor(), [selfKeep, cb, cbUd, st, payloadCopy]() {
                    char* pl = payloadCopy.empty() ? nullptr : ::strdup(payloadCopy.c_str());
                    try {
                        cb(cbUd, st, pl);
                    } catch (...) {
                    }
                });
            } catch (const std::bad_weak_ptr&) {
                // 极端：不在 shared_ptr 管理下（不应发生），回退为直接调用
                char* pl = payloadCopy.empty() ? nullptr : ::strdup(payloadCopy.c_str());
                try {
                    cb(cbUd, st, pl);
                } catch (...) {
                }
            }
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

    char*                       err        = nullptr;
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

    if (startThrew || err != nullptr
        || (op == nullptr && !core->notified.load(std::memory_order_acquire))) {
        guard.reset();
        std::string msg;
        if (err != nullptr) {
            msg = err;
            ::free(err);
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
