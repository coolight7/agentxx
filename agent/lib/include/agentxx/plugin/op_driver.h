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
    std::function<void*(const AgentxxOpNotify* notify, char** err)> start;
    std::function<void(void* op)>                                  cancel;
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
    std::atomic<int>  status{AGENTXX_OP_OK};
    std::string       payload;
    std::atomic<bool> cancelRequested{false};
    std::atomic<bool> cancelSent{false};

    asio::experimental::concurrent_channel<void(OpErrorCode)> chan;
    OpGuardPtr                                                guard;
    AgentxxOpCb                                               cb   = nullptr;
    void*                                                     cbUd = nullptr;

    explicit OpCore(const asio::any_io_executor& ex, OpGuardPtr g = nullptr) :
        chan(ex, 4), guard(std::move(g)) {}

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
        self->guard.reset();
        if (self->cb) {
            auto  cb   = self->cb;
            auto* cbUd = self->cbUd;
            self->cb   = nullptr;
            char* pl   = self->payload.empty() ? nullptr : ::strdup(self->payload.c_str());
            try {
                cb(cbUd, st, pl);
            } catch (...) {}
        }
    }

    AgentxxOpNotify notify() {
        return AgentxxOpNotify{&OpCore::onDone, this};
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
    auto guard = std::make_shared<PluginInstance::InflightGuard>(args.inst.get());
    auto core  = std::make_shared<OpCore>(args.ex, guard);
    std::string name  = args.inst ? args.inst->name : std::string{};
    std::string label = args.label;
    OpWatchdog   wd;

    std::shared_ptr<neograph::graph::CancelToken> cancelOp;
    if (args.cancelToken) {
        cancelOp = args.cancelToken->fork();
        cancelOp->bind_executor(args.ex);
        if (cancelOp->is_cancelled()) {
            core->cancelRequested.store(true, std::memory_order_release);
        }
    }

    char* err        = nullptr;
    void* op         = nullptr;
    bool  startThrew = false;
    AgentxxOpNotify ntf = core->notify();
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

    std::exception_ptr abort;
    try {
        while (!core->notified.load(std::memory_order_acquire)) {
            if (cancelOp && cancelOp->is_cancelled()) {
                core->cancelRequested.store(true, std::memory_order_release);
            }
            if (core->cancelRequested.load(std::memory_order_acquire)) {
                detail::safeCancelOnce(*core, args.drive, op);
            }
            if (core->notified.load(std::memory_order_acquire)) {
                break;
            }

            if (cancelOp) {
                // 零轮询：已请求取消则仅等待完成；否则同时等待完成或取消信号（经 cancelOp->slot 事件驱动）
                if (core->cancelRequested.load(std::memory_order_acquire)) {
                    auto [ec] = co_await core->chan.async_receive(asio::as_tuple(asio::use_awaitable));
                    (void)ec;
                } else {
                    asio::steady_timer cancelTimer(args.ex);
                    cancelTimer.expires_at(std::chrono::steady_clock::time_point::max());
                    auto [order, ec_chan, ec_cancel] = co_await asio::experimental::make_parallel_group(
                        core->chan.async_receive(asio::deferred),
                        cancelTimer.async_wait(
                            asio::bind_cancellation_slot(cancelOp->slot(), asio::deferred)
                        )
                    )
                                                           .async_wait(
                                                               asio::experimental::wait_for_one(),
                                                               asio::use_awaitable
                                                           );
                    (void)order;
                    (void)ec_chan;
                    (void)ec_cancel;
                    // 若取消分支先完成（operation_aborted），循环顶部将检测 is_cancelled 并触发 safeCancelOnce
                }
            } else {
                auto [ec] = co_await core->chan.async_receive(asio::as_tuple(asio::use_awaitable));
                (void)ec;
            }
        }
    } catch (...) {
        abort = std::current_exception();
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
    if (st == AGENTXX_OP_CANCELLED) {
        throw neograph::graph::CancelledException(fmt::format("plugin op `{}` cancelled", label));
    }
    if (st != AGENTXX_OP_OK) {
        throw std::runtime_error(payload.empty()
                                     ? fmt::format("plugin op `{}` failed", label)
                                     : payload);
    }
    co_return payload;
}

} // namespace plugin
} // namespace agentxx
