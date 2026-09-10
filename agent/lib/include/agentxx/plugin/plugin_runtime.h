/// 插件宿主运行时；所有类型仅供宿主内部使用，不属于 C ABI。
#pragma once

#include "agentxx/util/asio_error.h"
#include "agentxx/util/log.h"
#include "asio/as_tuple.hpp"
#include "asio/awaitable.hpp"
#include "asio/execution.hpp"
#include "asio/io_context.hpp"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace agentxx::plugin {

using RuntimeErrorCode = util::AsioErrorCode;

struct PluginRuntime;

/// A runtime-owned action may be observed by both an old executor queue and a
/// replay on a newly attached executor. The claim bit makes that race
/// exactly-once without requiring cancellation of an already posted handler.
struct RuntimeAction {
    std::atomic<bool> claimed{false};
    std::function<void()> fn;
};

bool enqueueRuntimeAction(
    const std::shared_ptr<PluginRuntime>& runtime,
    std::function<void()> fn,
    bool retainWhenStopped
) noexcept;
void replayRuntimeActions(const std::shared_ptr<PluginRuntime>& runtime) noexcept;

enum class PluginInstanceState : uint32_t {
    Loading,
    Ready,
    Disabled,
    Closing,
    Closed,
    CloseFailed,
};

inline const char* pluginInstanceStateName(PluginInstanceState state) noexcept {
    switch (state) {
        case PluginInstanceState::Loading: return "loading";
        case PluginInstanceState::Ready: return "ready";
        case PluginInstanceState::Disabled: return "disabled";
        case PluginInstanceState::Closing: return "closing";
        case PluginInstanceState::Closed: return "closed";
        case PluginInstanceState::CloseFailed: return "close_failed";
    }
    return "unknown";
}

/// 实例状态与执行 lease 的公共控制块。
/// - 状态变更与 idle 等待者列表只在 IO 线程访问。
/// - admission 位与计数共用一次 CAS，关闭与跨线程获取 lease 之间没有空隙。
/// - 最后一个 lease 释放后向 IO 线程投递一次通知，唤醒全部 idle 等待者。
class InstanceLifetime : public std::enable_shared_from_this<InstanceLifetime> {
public:
    using ActionEnqueuer = std::function<bool(std::function<void()>)>;

    InstanceLifetime(asio::any_io_executor executor, std::string name, uint64_t generation) :
        executor_(std::move(executor)), name_(std::move(name)), generation_(generation) {}

    InstanceLifetime(
        asio::any_io_executor executor,
        std::string           name,
        uint64_t              generation,
        ActionEnqueuer        enqueue
    ) :
        executor_(std::move(executor)),
        name_(std::move(name)),
        generation_(generation),
        enqueue_(std::move(enqueue)) {}

    InstanceLifetime(const InstanceLifetime&) = delete;
    InstanceLifetime& operator=(const InstanceLifetime&) = delete;

    const std::string& name() const noexcept { return name_; }
    uint64_t generation() const noexcept { return generation_; }
    PluginInstanceState state() const noexcept { return state_.load(std::memory_order_acquire); }

    /// 仅所属 IO 线程调用；Closed 不得重新打开，重载必须建立新的 lifetime。
    void setState(PluginInstanceState next) noexcept {
        assert(state() != PluginInstanceState::Closed || next == PluginInstanceState::Closed);
        if (next == PluginInstanceState::Loading || next == PluginInstanceState::Ready) {
            state_.store(next, std::memory_order_release);
            leases_.fetch_and(kCountMask, std::memory_order_acq_rel);
        } else {
            leases_.fetch_or(kNoAdmission, std::memory_order_acq_rel);
            state_.store(next, std::memory_order_release);
        }
    }

    bool acceptsOperations() const noexcept { return state() == PluginInstanceState::Ready; }
    bool acceptsRegistration() const noexcept {
        const auto s = state();
        return s == PluginInstanceState::Loading || s == PluginInstanceState::Ready;
    }
    void requestClose() noexcept { setState(PluginInstanceState::Closing); }
    bool closeRequested() const noexcept {
        const auto s = state();
        return s == PluginInstanceState::Closing || s == PluginInstanceState::CloseFailed
               || s == PluginInstanceState::Closed;
    }
    size_t leaseCount() const noexcept {
        return static_cast<size_t>(leases_.load(std::memory_order_acquire) & kCountMask);
    }

    /// 设置一次性 idle 收尾动作。动作只在所属 IO 线程执行，适用于 manager
    /// 已经开始关闭但自身 owner 即将析构的场景；动作执行前会从 lifetime
    /// 中取出，避免 lifetime 与插件实例形成永久循环引用。
    bool setIdleCleanup(std::function<void()> cleanup) {
        if (!cleanup || idleCleanup_ || idleCleanupRegistered_) {
            return false;
        }
        idleCleanupRegistered_ = true;
        // 关闭请求与最后一个 lease 释放在同一 IO 线程串行时通常不会走到这里，
        // 但保留这个分支可处理调用方观察到 idle 后才登记收尾的边界。
        if (leaseCount() == 0) {
            idleCleanup_ = std::move(cleanup);
            const auto weakSelf = weak_from_this();
            if (enqueueAction([weakSelf] {
                    if (auto self = weakSelf.lock()) {
                        self->publishIdle();
                    }
                })) {
                return true;
            }
            XX_LOGE("Plugin `{}` failed to publish idle cleanup", name_);
            return false;
        }
        idleCleanup_ = std::move(cleanup);
        return true;
    }

    /// 仅供测试/直接收尾路径取消尚未执行的 idle 动作。
    void clearIdleCleanup() noexcept {
        idleCleanup_ = {};
        idleCleanupRegistered_ = false;
    }

    /// `lifecycle`: 仅 IO 线程用于宿主显式 start/stop 调用；不得供新业务操作使用。
    bool tryAcquire(bool lifecycle = false) noexcept {
        auto count = leases_.load(std::memory_order_acquire);
        for (;;) {
            if ((!lifecycle && (count & kNoAdmission)) || state() == PluginInstanceState::Closed
                || (count & kCountMask) == kCountMask) {
                return false;
            }
            if (leases_.compare_exchange_weak(count, count + 1, std::memory_order_acq_rel)) {
                return true;
            }
        }
    }

    void release() noexcept {
        const auto previous = leases_.fetch_sub(1, std::memory_order_acq_rel);
        assert((previous & kCountMask) != 0);
        if ((previous & kCountMask) == 1) {
            const auto weakSelf = weak_from_this();
            if (!enqueueAction([weakSelf] {
                    if (auto self = weakSelf.lock()) {
                        self->publishIdle();
                    }
                })) {
                // 不从释放 lease 的 worker 线程调用任何插件代码。若 executor
                // 停止，runtime action 会保留闭包并在恢复时重放。
                XX_LOGE("Plugin `{}` failed to publish idle event", name_);
            }
        }
    }

    /// 无轮询的绝对截止等待；并行等待者各有自己的 timer，通知会广播。
    asio::awaitable<bool> waitIdleUntil(std::chrono::steady_clock::time_point deadline) {
        auto self = shared_from_this();
        if (leaseCount() == 0) {
            co_return true;
        }
        auto waiter = std::make_shared<IdleWaiter>(executor_, deadline);
        std::erase_if(idleWaiters_, [](const auto& w) { return w.expired(); });
        idleWaiters_.push_back(waiter);
        auto [ec] = co_await waiter->timer.async_wait(asio::as_tuple(asio::use_awaitable));
        std::erase_if(idleWaiters_, [&waiter](const auto& w) {
            auto p = w.lock();
            return !p || p == waiter;
        });
        if (ec && !waiter->idle) {
            throw util::AsioSystemError(ec);
        }
        co_return waiter->idle || leaseCount() == 0;
    }

private:
    struct IdleWaiter {
        asio::steady_timer timer;
        bool idle = false;
        IdleWaiter(const asio::any_io_executor& ex, std::chrono::steady_clock::time_point deadline) :
            timer(ex, deadline) {}
    };
    static constexpr uint64_t kNoAdmission = uint64_t{1} << 63;
    static constexpr uint64_t kCountMask = kNoAdmission - 1;

    bool enqueueAction(std::function<void()> fn) noexcept {
        if (enqueue_) {
            return enqueue_(std::move(fn));
        }
        try {
            if (!executor_) {
                return false;
            }
            asio::post(executor_, [fn = std::move(fn)]() mutable {
                try {
                    fn();
                } catch (const std::exception& e) {
                    XX_LOGE("Plugin idle action threw: {}", e.what());
                } catch (...) {
                    XX_LOGE("Plugin idle action threw unknown exception");
                }
            });
            return true;
        } catch (...) {
            return false;
        }
    }

    bool publishIdle() noexcept {
        if (leaseCount() != 0) {
            return true;
        }
        auto waiters = std::move(idleWaiters_);
        idleWaiters_.clear();
        for (const auto& weak : waiters) {
            if (auto waiter = weak.lock()) {
                waiter->idle = true;
                waiter->timer.cancel();
            }
        }
        auto cleanup = std::move(idleCleanup_);
        idleCleanup_ = {};
        if (cleanup) {
            try {
                cleanup();
            } catch (const std::exception& e) {
                XX_LOGE("Plugin `{}` idle cleanup threw: {}", name_, e.what());
            } catch (...) {
                XX_LOGE("Plugin `{}` idle cleanup threw unknown exception", name_);
            }
        }
        return true;
    }

    asio::any_io_executor executor_;
    std::string name_;
    uint64_t generation_;
    std::atomic<PluginInstanceState> state_{PluginInstanceState::Loading};
    std::atomic<uint64_t> leases_{0};
    std::vector<std::weak_ptr<IdleWaiter>> idleWaiters_;
    std::function<void()> idleCleanup_;
    ActionEnqueuer enqueue_;
    bool idleCleanupRegistered_ = false;
};

class InstanceLease {
public:
    InstanceLease() = default;
    static InstanceLease acquire(const std::shared_ptr<InstanceLifetime>& lifetime,
                                 bool lifecycle = false) noexcept {
        return lifetime && lifetime->tryAcquire(lifecycle) ? InstanceLease(lifetime) : InstanceLease{};
    }
    ~InstanceLease() { reset(); }
    InstanceLease(const InstanceLease&) = delete;
    InstanceLease& operator=(const InstanceLease&) = delete;
    InstanceLease(InstanceLease&& other) noexcept : lifetime_(std::move(other.lifetime_)) {}
    InstanceLease& operator=(InstanceLease&& other) noexcept {
        if (this != &other) {
            reset();
            lifetime_ = std::move(other.lifetime_);
        }
        return *this;
    }
    explicit operator bool() const noexcept { return lifetime_ != nullptr; }
    void reset() noexcept {
        if (auto lifetime = std::exchange(lifetime_, {})) {
            lifetime->release();
        }
    }
private:
    explicit InstanceLease(std::shared_ptr<InstanceLifetime> lifetime) : lifetime_(std::move(lifetime)) {}
    std::shared_ptr<InstanceLifetime> lifetime_;
};

struct OpCore;

/// 不捕获 manager 裸指针的公共运行时状态。Operation 表只由 IO 线程操作。
/// 已接受 Operation 持有 runtime，runtime 持有 Operation，直到最终清理时断开；
/// 等待者取消或 manager 被错误销毁不能提前释放仍在执行的 DSO/context。
struct PluginRuntime {
    asio::any_io_executor executor;
    std::atomic<std::thread::id> ioThreadId{};
    /// 正常路径只在 IO 线程访问；executor 停止后，完成线程仍可能需要
    /// 收束一个已接受 Operation，因此为这条故障路径提供最小互斥保护。
    mutable std::mutex operationsMutex;
    std::map<uint64_t, std::shared_ptr<OpCore>> operations;
    mutable std::mutex pendingMutex;
    std::deque<std::shared_ptr<RuntimeAction>> pendingActions;
    uint64_t nextOperationId = 1;
    uint64_t nextGeneration = 1;

    /// 尚未终结（未提交完成）的 Operation 摘要，形如 `label#id, label#id`。
    ///
    /// 用途：关闭超时（CloseFailed）时给出"到底是哪些操作没终结"的可观察信息。
    /// 完成包在 executor 停止期间会保留在待重放队列里，此时 Operation 仍是未终结
    /// 状态；这个方法就是它的外部可观察表示。空串表示没有未终结操作。
    ///
    /// 定义放在 .cpp（OpCore 在此处只有前置声明）。
    std::string pendingOperationSummary() const;
};

inline bool runtimeExecutorStopped(const asio::any_io_executor& executor) noexcept {
    if (!executor) {
        return true;
    }
    try {
        // A direct io_context executor is the runtime's supported binding.
        // Type-erased execution_context is not polymorphic, so do not use
        // RTTI here. Unknown executor adapters remain usable; their owner is
        // responsible for rebinding before a synchronous call is made.
        if (const auto* ioExecutor = executor.target<asio::io_context::executor_type>()) {
            return ioExecutor->context().stopped();
        }
    } catch (...) {
        return true;
    }
    return false;
}

inline void runRuntimeAction(
    const std::weak_ptr<PluginRuntime>&    weakRuntime,
    const std::shared_ptr<RuntimeAction>&  action
) noexcept {
    if (!action || action->claimed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (auto runtime = weakRuntime.lock()) {
        std::lock_guard lock(runtime->pendingMutex);
        std::erase(runtime->pendingActions, action);
    }
    try {
        if (action->fn) {
            action->fn();
        }
    } catch (const std::exception& e) {
        XX_LOGE("Plugin runtime action threw: {}", e.what());
    } catch (...) {
        XX_LOGE("Plugin runtime action threw unknown exception");
    }
}

inline bool enqueueRuntimeAction(
    const std::shared_ptr<PluginRuntime>& runtime,
    std::function<void()> fn,
    bool retainWhenStopped
) noexcept {
    if (!runtime || !fn) {
        return false;
    }
    auto action = std::make_shared<RuntimeAction>();
    action->fn = std::move(fn);
    auto executor = runtime->executor;
    if (!executor) {
        if (!retainWhenStopped) {
            return false;
        }
        std::lock_guard lock(runtime->pendingMutex);
        runtime->pendingActions.push_back(action);
        return true;
    }
    if (!retainWhenStopped && runtimeExecutorStopped(executor)) {
        return false;
    }
    // Register reliable actions before posting. An io_context can transition
    // to stopped between the probe and asio::post (and a naturally idle
    // context also reports stopped); the posted handler and this pending
    // record then race safely through RuntimeAction::claimed.
    if (retainWhenStopped) {
        std::lock_guard lock(runtime->pendingMutex);
        runtime->pendingActions.push_back(action);
    }
    try {
        const std::weak_ptr<PluginRuntime> weakRuntime = runtime;
        asio::post(executor, [weakRuntime, action] { runRuntimeAction(weakRuntime, action); });
        return true;
    } catch (...) {
        if (retainWhenStopped) {
            std::lock_guard lock(runtime->pendingMutex);
            if (std::find(runtime->pendingActions.begin(), runtime->pendingActions.end(), action)
                == runtime->pendingActions.end()) {
                runtime->pendingActions.push_back(std::move(action));
            }
            return true;
        }
        return false;
    }
}

inline void replayRuntimeActions(const std::shared_ptr<PluginRuntime>& runtime) noexcept {
    if (!runtime || !runtime->executor || runtimeExecutorStopped(runtime->executor)) {
        return;
    }
    std::vector<std::shared_ptr<RuntimeAction>> pending;
    {
        std::lock_guard lock(runtime->pendingMutex);
        pending.reserve(runtime->pendingActions.size());
        for (const auto& action : runtime->pendingActions) {
            if (action && !action->claimed.load(std::memory_order_acquire)) {
                pending.push_back(action);
            }
        }
    }
    for (const auto& action : pending) {
        try {
            const std::weak_ptr<PluginRuntime> weakRuntime = runtime;
            asio::post(runtime->executor, [weakRuntime, action] {
                runRuntimeAction(weakRuntime, action);
            });
        } catch (...) { /* retain the action for the next executor binding */ }
    }
}

} // namespace agentxx::plugin
