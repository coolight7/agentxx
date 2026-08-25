/*
 * agentxx/plugin/op_driver.h —— 插件异步操作宿主侧驱动器 (lib 内部实现, 非 ABI)
 *
 * 统一异步操作模型的核心: 在宿主 io 线程上驱动插件三件套 (start/poll/cancel),
 * 与内置工具的 asio 协程同线程交错执行。工具执行 / 钩子分发 / 能力方法调用 /
 * call_tool_async/invoke_capability_async 全部复用本驱动。
 *
 * 两种驱动形态:
 * - awaitPluginOp(): 等待形态 —— 作为协程挂起等待插件操作终结, 结果按
 *   工具语义转换 (OK→结果串 / CANCELLED→CancelledException / FAILED→异常);
 *   支持会话取消令牌联动 + 宿主超时; 等待方提前退出 (取消/超时) 时自动
 *   转入后台收割协程继续推进直至终结 (inflight 保活转移, 卸载安全)
 * - makeHostOp(): 句柄形态 —— 后台收割式驱动, 结果写入线程安全 sink,
 *   由 AgentxxHostOp 暴露给插件任意线程轮询 (call_tool_async 等)
 *
 * 线程模型:
 * - start/poll/cancel 始终在 io 线程被调用 (非阻塞快速返回约定);
 * - AgentxxOpNotify.done 可从插件任意线程回调 → 经原子状态发布 + channel
 *   kick 投递回 io 线程唤醒等待;
 * - inflight 计数贯穿 start→终态通知全过程 (收割协程转移保活), 保证
 *   unloadAsync 等待到操作真正终结后才 dlclose
 */
#pragma once

#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/log.h"
#include "asio/as_tuple.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/error.hpp"
#include "asio/experimental/concurrent_channel.hpp"
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
#include <mutex>
#include <string>
#include <utility>

namespace agentxx {
namespace plugin {

/// 单次插件操作的抽象三件套 (由调用方把 C 函数指针与实参绑定成闭包;
/// 全部在 io 线程被调用; 插件违约抛异常时按协议失败处理)
struct OpDrive {
    /// 启动: notify 为宿主通知器 (绑定驱动器核心状态); 返回 op 句柄,
    /// NULL = 内联完成或失败 (失败时 *err 非空)
    std::function<void*(const AgentxxOpNotify* notify, char** err)> start;
    /// 推进 (可空 = 只等完成通知); op 为 start 返回的句柄;
    /// 返回 AGENTXX_OP_POLL_DONE 或建议延迟 ms
    std::function<int(void* op)> poll;
    /// 协作式取消请求 (可空); op 为 start 返回的句柄
    std::function<void(void* op)> cancel;
};

using OpErrorCode = neograph_asio_error_code;

/// 单次推进建议值上限 (防御插件返回异常大的延迟)
inline constexpr int kMaxPollHintMs = 1000;

/// 慢调用看门狗: start/poll 单次超过阈值告警 io 线程被卡 (每操作每阶段至多一次)
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

/// 操作核心状态 (driver ↔ notifier ↔ sink 共享)
/// - notified 为发布点: payload 在 CAS 成功后独占写, 读方 acquire 后读取
/// - chan 容量取大值: 多个 kick 来源 (通知器/watcher/HostOp.cancel) 不阻塞,
///   多余 kick 造成的一次空醒由循环重查状态吸收 (无害)
struct OpCore {
    std::atomic<bool> notified{false};
    std::atomic<int>  status{AGENTXX_OP_OK};
    std::string       payload;
    /// 取消请求 (会话取消 / HostOp.cancel); 驱动循环检测后转发底层一次
    std::atomic<bool> cancelRequested{false};
    std::atomic<bool> cancelSent{false}; ///< 底层 cancel 已调用过一次

    asio::experimental::concurrent_channel<void(OpErrorCode)> chan;

    explicit OpCore(const asio::any_io_executor& ex) : chan(ex, 1024) {}

    /// AgentxxOpNotify.done 实现 (任意线程可调; 恰好一次契约 + 二次通知防御)
    static void onDone(void* ud, int st, char* payload) {
        auto* self  = static_cast<OpCore*>(ud);
        bool  expect = false;
        if (!self->notified.compare_exchange_strong(expect, true, std::memory_order_acq_rel)) {
            if (payload) {
                ::free(payload); // 违约二次通知: 直接丢弃
            }
            return;
        }
        self->status.store(st, std::memory_order_release);
        if (payload) {
            self->payload.assign(payload);
            ::free(payload); // 宿主 alloc 即 malloc, 所有权已移交
        }
        self->chan.try_send(OpErrorCode());
    }

    AgentxxOpNotify notify() {
        return AgentxxOpNotify{&OpCore::onDone, this};
    }
};

using OpGuardPtr = std::shared_ptr<PluginInstance::InflightGuard>;

namespace detail {

/// 安全调用底层闭包 (插件违约抛异常按协议失败处理)
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

/// 推进一步: 返回下一等待 hint (无 poll = 只等通知)
inline int stepPoll(
    OpCore&                  core,
    const OpDrive&           d,
    OpWatchdog&              wd,
    void*                    op,
    std::string_view         name,
    std::string_view         label
) {
    if (!d.poll) {
        return -1; // 只等通知
    }
    wd.enter("poll");
    int hint = 0;
    try {
        hint = d.poll(op);
    } catch (const std::exception& e) {
        XX_LOGW("Plugin `{}` `{}` poll() threw: {}", name, label, e.what());
        return AGENTXX_OP_POLL_DONE; // 违约: 视作终结请求 (等 notifier 兜底)
    } catch (...) {
        XX_LOGW("Plugin `{}` `{}` poll() threw unknown exception", name, label);
        return AGENTXX_OP_POLL_DONE;
    }
    wd.exit(name, label);
    if (hint > kMaxPollHintMs) {
        hint = kMaxPollHintMs;
    }
    return hint;
}

/// 等待一个推进周期: hint>=1 定时小睡; ==0 post 让出 (与其他协程交错公平点);
/// <0 等完成通知通道 (notifier/kick 即时唤醒)
inline asio::awaitable<void> waitDriveHint(OpCore& core, int hint) {
    if (hint >= 1) {
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(hint));
        auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
        (void)ec; // 取消中断按轮询重查处理
        co_return;
    }
    if (hint == 0) {
        co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
        co_return;
    }
    auto [ec] = co_await core.chan.async_receive(asio::as_tuple(asio::use_awaitable));
    (void)ec;
}

/// 后台收割协程体: 继续驱动直至终结 (放弃路径接管 inflight 保活)
inline asio::awaitable<void> reapUntilDone(
    std::shared_ptr<OpCore> core,
    OpDrive                 drive,
    void*                   op,
    OpGuardPtr              guard,
    std::string             name,
    std::string             label
) {
    XX_LOGD("Plugin `{}` abandoned op `{}`, reaping in background", name, label);
    OpWatchdog wd;
    while (!core->notified.load(std::memory_order_acquire)) {
        if (core->cancelRequested.load(std::memory_order_acquire)) {
            safeCancelOnce(*core, drive, op);
        }
        if (core->notified.load(std::memory_order_acquire)) {
            break;
        }
        int hint = stepPoll(*core, drive, wd, op, name, label);
        if (hint < 0 && drive.poll) {
            hint = 25; ///< 收割路径对"只等通知"型也保持低频轮询兜底
        }
        co_await waitDriveHint(*core, hint);
    }
    guard.reset(); // 释放 inflight 保活 (卸载放行点)
    XX_LOGD("Plugin `{}` abandoned op `{}` reaped", name, label);
}

} // namespace detail

// =====================================================================
// 等待形态: awaitPluginOp
// =====================================================================

/// 驱动插件操作直至终结并返回结果 payload:
/// - OK        → co_return payload (可为空串)
/// - CANCELLED → throw CancelledException
/// - FAILED    → throw runtime_error(payload)
/// - 启动失败   → throw runtime_error(err)
/// - 等待方被取消/超时中断 → 转入后台收割协程 (保活转移) 后原样 rethrow
struct PluginOpAwaitArgs {
    std::shared_ptr<PluginInstance>               inst;        ///< 保活/日志/归属
    std::string                                   label;       ///< 日志标签 (工具名等)
    asio::any_io_executor                         ex;
    std::shared_ptr<neograph::graph::CancelToken> cancelToken; ///< 可空
    OpDrive                                       drive;
};

inline asio::awaitable<std::string> awaitPluginOp(PluginOpAwaitArgs args) {
    auto core  = std::make_shared<OpCore>(args.ex);
    auto guard = std::make_shared<PluginInstance::InflightGuard>(args.inst.get());
    const auto& name  = args.inst->name;
    const auto& label = args.label;
    OpWatchdog   wd;

    // 会话取消 watcher: 20ms 轮询令牌, 取消时置请求标志并踢醒等待
    // (含无限通道等待); 循环正常终结后经 watcherDone 自行退出
    auto watcherDone = std::make_shared<std::atomic<bool>>(false);
    if (args.cancelToken && !args.cancelToken->is_cancelled()) {
        asio::co_spawn(
            args.ex,
            [core, token = args.cancelToken, watcherDone, ex = args.ex]() -> asio::awaitable<void> {
                asio::steady_timer timer(ex);
                while (!watcherDone->load(std::memory_order_acquire)) {
                    timer.expires_after(std::chrono::milliseconds(20));
                    auto [ec] = co_await timer.async_wait(asio::as_tuple(asio::use_awaitable));
                    if (ec || core->notified.load(std::memory_order_acquire)) {
                        break;
                    }
                    if (token->is_cancelled()) {
                        core->cancelRequested.store(true, std::memory_order_release);
                        core->chan.try_send(OpErrorCode());
                        break;
                    }
                }
                watcherDone->store(true, std::memory_order_release);
            },
            asio::detached
        );
    } else if (args.cancelToken) {
        // 初始即已取消
        core->cancelRequested.store(true, std::memory_order_release);
    }

    // ---- 启动 (注入宿主通知器) ----
    char* err = nullptr;
    void* op  = nullptr;
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
        // 启动失败 / 协议违约 (NULL 且无错误且未通知)
        watcherDone->store(true, std::memory_order_release);
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
    (void)op; // 句柄回传给 poll/cancel 闭包 (插件自管其内容)

    // ---- 推进循环 ----
    std::exception_ptr abort;
    try {
        while (!core->notified.load(std::memory_order_acquire)) {
            if (args.cancelToken && args.cancelToken->is_cancelled()) {
                core->cancelRequested.store(true, std::memory_order_release);
                core->chan.try_send(OpErrorCode());
            }
            if (core->cancelRequested.load(std::memory_order_acquire)) {
                detail::safeCancelOnce(*core, args.drive, op);
            }
            if (core->notified.load(std::memory_order_acquire)) {
                break;
            }
            int hint = detail::stepPoll(*core, args.drive, wd, op, name, label);
            if (hint == AGENTXX_OP_POLL_DONE && !core->notified.load(std::memory_order_acquire)) {
                hint = 1; // poll 报终结但通知未达: 极短缓冲等待
            }
            co_await detail::waitDriveHint(*core, hint);
        }
    } catch (...) {
        abort = std::current_exception();
    }
    watcherDone->store(true, std::memory_order_release);

    if (abort) {
        // 放弃路径: 请求底层取消 + 收割协程接管保活, 再向外传播
        detail::safeCancelOnce(*core, args.drive, op);
        asio::co_spawn(
            args.ex,
            detail::reapUntilDone(core, std::move(args.drive), op, std::move(guard), name, label),
            asio::detached
        );
        std::rethrow_exception(abort);
    }

    // ---- 正常终结: 按状态转换 ----
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

// =====================================================================
// 句柄形态: HostOpShared / AgentxxHostOp (call_tool_async / invoke_capability_async)
// =====================================================================

/// 句柄共享状态 (后台驱动协程 ↔ 句柄方法 共享; sink 经互斥保护)
struct HostOpShared {
    OpCore                                        core;
    mutable std::mutex                            mtx;
    bool                                          done   = false;
    bool                                          taken  = false;
    int                                           status = AGENTXX_OP_OK;
    std::string                                   payload;
    /// 孤儿化 (free 早于终结): 驱动照常跑完履行插件契约, 结果无人消费
    std::atomic<bool>                             orphaned{false};
    std::shared_ptr<neograph::graph::CancelToken> cancelToken; ///< 可空
    OpDrive                                       drive;
    std::string                                   name;  ///< 日志: 目标插件名
    std::string                                   label;

    explicit HostOpShared(const asio::any_io_executor& ex) : core(ex) {}
};

/// 后台驱动协程体 (io executor 上运行; 不抛出)
inline asio::awaitable<void> runHostOpDriver(std::shared_ptr<HostOpShared> s) {
    if (s->orphaned.load(std::memory_order_acquire)) {
        co_return; ///< free 早于启动: 不启动底层操作 (inflight 未递增过)
    }
    OpWatchdog wd;
    char* err = nullptr;
    void* op  = nullptr;
    AgentxxOpNotify ntf = s->core.notify();
    try {
        op = s->drive.start(&ntf, &err);
    } catch (...) {
        err = nullptr;
    }
    if (err != nullptr || (op == nullptr && !s->core.notified.load(std::memory_order_acquire))) {
        std::lock_guard lk(s->mtx);
        s->done    = true;
        s->status  = AGENTXX_OP_FAILED;
        s->payload = err != nullptr
                         ? std::string(err)
                         : fmt::format("host op `{}` protocol violation", s->label);
        if (err != nullptr) {
            ::free(err);
        }
        s->core.notified.store(true, std::memory_order_release);
        co_return;
    }
    while (!s->core.notified.load(std::memory_order_acquire)) {
        if (s->cancelToken && s->cancelToken->is_cancelled()) {
            s->core.cancelRequested.store(true, std::memory_order_release);
            s->core.chan.try_send(OpErrorCode());
        }
        if (s->core.cancelRequested.load(std::memory_order_acquire)) {
            detail::safeCancelOnce(s->core, s->drive, op);
        }
        if (s->core.notified.load(std::memory_order_acquire)) {
            break;
        }
        int hint = detail::stepPoll(s->core, s->drive, wd, op, s->name, s->label);
        if (hint == AGENTXX_OP_POLL_DONE && !s->core.notified.load(std::memory_order_acquire)) {
            hint = 1;
        }
        co_await detail::waitDriveHint(s->core, hint);
    }
    std::lock_guard lk(s->mtx);
    s->done    = true;
    s->status  = s->core.status.load(std::memory_order_acquire);
    s->payload = std::move(s->core.payload);
}

/// 句柄方法 (C ABI trampolines; 契约: 同一句柄的方法不得并发调用)

inline int hop_poll(AgentxxHostOp* o) {
    auto* sp = static_cast<std::shared_ptr<HostOpShared>*>(o->internal);
    std::lock_guard lk((*sp)->mtx);
    return (*sp)->done ? AGENTXX_OP_POLL_DONE : 2; ///< 建议 2ms 后再查
}

inline int hop_take(AgentxxHostOp* o, int* out_status, char** out_payload) {
    auto* sp = static_cast<std::shared_ptr<HostOpShared>*>(o->internal);
    std::lock_guard lk((*sp)->mtx);
    if (!(*sp)->done || (*sp)->taken) {
        return -1;
    }
    (*sp)->taken = true;
    if (out_status) {
        *out_status = (*sp)->status;
    }
    if (out_payload) {
        size_t n = (*sp)->payload.size();
        char*  p = static_cast<char*>(::malloc(n + 1));
        if (p) {
            if (n) {
                memcpy(p, (*sp)->payload.data(), n);
            }
            p[n] = '\0';
        }
        *out_payload = p;
    }
    return 0;
}

inline void hop_cancel(AgentxxHostOp* o) {
    auto* sp = static_cast<std::shared_ptr<HostOpShared>*>(o->internal);
    (*sp)->core.cancelRequested.store(true, std::memory_order_release);
    (*sp)->core.chan.try_send(OpErrorCode()); // 即时唤醒驱动转发底层取消
}

inline void hop_free(AgentxxHostOp* o) {
    auto* sp = static_cast<std::shared_ptr<HostOpShared>*>(o->internal);
    (*sp)->orphaned.store(true, std::memory_order_release);
    (*sp)->core.chan.try_send(OpErrorCode());
    delete sp;
    delete o;
}

/// 创建句柄并在 io executor 上启动后台驱动 (任意线程可调用)
/// - guard: 调用方装配的 inflight 保活; 孤儿化早于启动时不启动且释放
inline AgentxxHostOp* makeHostOp(
    const asio::any_io_executor&                   ex,
    std::shared_ptr<HostOpShared>                  shared,
    std::shared_ptr<PluginInstance::InflightGuard> guard
) {
    auto* sp  = new std::shared_ptr<HostOpShared>(std::move(shared));
    auto* ops = new AgentxxHostOp{hop_poll, hop_take, hop_cancel, hop_free, sp};
    asio::post(ex, [ex, sp, guard]() mutable {
        auto s = *sp;
        if (s->orphaned.load(std::memory_order_acquire)) {
            return; // guard 随协程帧析构释放 (从未递增则无影响)
        }
        asio::co_spawn(
            ex,
            [ex, s, guard]() -> asio::awaitable<void> {
                try {
                    co_await runHostOpDriver(s);
                } catch (const std::exception& e) {
                    // 防御: 驱动内部不抛出, 兜底转失败终态
                    std::lock_guard lk(s->mtx);
                    if (!s->done) {
                        s->done    = true;
                        s->status  = AGENTXX_OP_FAILED;
                        s->payload = e.what();
                        s->core.notified.store(true, std::memory_order_release);
                    }
                }
                (void)ex;
            },
            asio::detached
        );
    });
    return ops;
}

} // namespace plugin
} // namespace agentxx
