/// 插件统一 Operation 驱动器（宿主内部，非 ABI）。
#pragma once

#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/log.h"
#include "asio/as_tuple.hpp"
#include "asio/bind_cancellation_slot.hpp"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "neograph/graph/cancel.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace agentxx::plugin {

struct OpDrive {
    std::function<void*(const AgentxxPluginOperatorNotify*, AgentxxPluginString*)> start;
    std::function<void(void*)> cancel;
};

using OpErrorCode = util::AsioErrorCode;
using OpGuardPtr = std::shared_ptr<PluginInstanceBase::InflightGuard>;

/// 状态只在 IO 线程访问。同步拒绝不进入完成回调协议。
enum class PluginOperationState { Accepted, Running, Cancelling, Completed, Rejected };

struct OpCore : std::enable_shared_from_this<OpCore> {
    struct CompletionPacket {
        int32_t status = AGENTXX_PLUGIN_OPERATOR_FAILED;
        std::string payload;
    };

    /// 创建时先登记到 runtime，再交给插件。等待者取消不影响 runtime 的持有。
    static std::shared_ptr<OpCore> create(
        std::shared_ptr<PluginRuntime> runtime,
        const std::shared_ptr<PluginInstanceBase>& provider,
        const std::shared_ptr<PluginInstanceBase>& caller,
        std::string label,
        bool lifecycle = false
    ) {
        // 生命周期操作 (start/stop) 正是状态切换本身：停用中的实例仍必须能收到
        // stop，关闭中的实例仍必须能收到 stop。因此这里只对业务操作检查
        // `enabled`/可注册状态，生命周期操作交给 enable_closing 的 lease 把关
        // (Closed 仍会拒绝)，见 InstanceLifetime::tryAcquire。
        if (!runtime || !runtime->executor || !provider
            || (!lifecycle
                && (!provider->enabled
                    || (provider->lifetime && !provider->lifetime->acceptsRegistration())))) {
            throw std::runtime_error("plugin operation rejected: provider is closed or disabled");
        }
        auto core = std::shared_ptr<OpCore>(new OpCore(std::move(runtime), std::move(label)));
        core->provider_ = std::make_shared<PluginInstanceBase::InflightGuard>(provider, lifecycle);
        if (!*core->provider_) {
            throw std::runtime_error("plugin operation rejected: provider is closing");
        }
        if (caller) {
            if (!caller->enabled) {
                throw std::runtime_error("plugin operation rejected: caller is disabled");
            }
            core->caller_ = std::make_shared<PluginInstanceBase::InflightGuard>(caller, lifecycle);
            if (!*core->caller_) {
                throw std::runtime_error("plugin operation rejected: caller is closing");
            }
        }
        core->handle_ = std::make_shared<AgentxxPluginOperatorHandle>();
        core->completionEndpoint_
            = std::make_shared<AgentxxPluginOperationCompletionEndpoint>();
        core->completionEndpoint_->operation = core;
        core->handle_->completionEndpoint = core->completionEndpoint_;
        core->handle_->caller = caller ? caller : provider;
        core->handle_->runtime = core->runtime_;
        core->handle_->executor = core->runtime_->executor;
        core->handle_->cancelFn = [weak = std::weak_ptr<OpCore>(core)] {
            if (auto operation = weak.lock()) {
                operation->cancel();
            }
        };
        core->id_ = core->runtime_->nextOperationId++;
        try {
            std::lock_guard runtimeLock(core->runtime_->operationsMutex);
            core->runtime_->operations.emplace(core->id_, core);
            // ABI 句柄是调用方可持有的裸指针；活动索引移除后仍保留宿主
            // tombstone，迟到 cancel 只会观察 completed，不访问已释放对象。
            provider->operatorHandles.push_back(core->handle_);
            provider->outstandingOps.push_back(core->handle_);
            provider->completionEndpoints.push_back(core->completionEndpoint_);
            if (caller && caller != provider) {
                caller->operatorHandles.push_back(core->handle_);
                caller->outstandingOps.push_back(core->handle_);
                caller->completionEndpoints.push_back(core->completionEndpoint_);
            }
        } catch (...) {
            core->releaseRecords();
            throw;
        }
        return core;
    }

    AgentxxPluginOperatorHandle* handle() const noexcept { return handle_.get(); }
    uint64_t id() const noexcept { return id_; }
    const std::string& label() const noexcept { return label_; }
    PluginOperationState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }
    int32_t status() const noexcept { return completion_.status; }
    const std::string& payload() const noexcept { return completion_.payload; }
    bool completed() const noexcept { return state() == PluginOperationState::Completed; }

    /// 完成包已经产生（插件调用了 done）但还没有在 IO 线程提交。
    ///
    /// 这是"完成投递失败但被保留"这一状态的可观察表示：executor 停止时完成包
    /// 进入 runtime 的待重放队列，Operation 保持未终结且继续持有 caller/provider
    /// lease；实例关闭会因此等到截止时间并进入 CloseFailed（保留 ctx/DSO，可重试），
    /// 而不是静默泄漏。executor 重新绑定后完成包会重放并只提交一次。
    bool completionPending() const {
        std::lock_guard lock(submitMutex_);
        return completionSubmitted_ && state() != PluginOperationState::Completed;
    }

    void setCallback(AgentxxPluginOperatorCallback cb, void* ud) noexcept {
        callback_ = cb;
        callbackUd_ = ud;
    }

    /// 宿主已接受的 post/sleep/offload 可复用这个完成处理器。
    /// closure 仅在 IO 线程执行，并且在双方 lease 释放前销毁。
    void setCompletionHandler(std::function<void(int32_t, std::string_view)> handler) {
        completionHandler_ = std::move(handler);
    }

    /// provider 可以同步 done + NULL；只有 NULL 且没有 done 才是真正拒绝。
    /// 错误串被复制并释放，无论成功、拒绝还是违约异常均不泄漏。
    bool start(OpDrive drive, std::string& error) {
        drive_ = std::move(drive);
        AgentxxPluginString startError{};
        auto ntf = notify();
        const auto started = std::chrono::steady_clock::now();
        try {
            if (drive_.start) {
                providerHandle_ = drive_.start(&ntf, &startError);
            } else {
                error = "plugin has no start callback";
            }
        } catch (const std::exception& e) {
            error = e.what();
        } catch (...) {
            error = "plugin start threw unknown exception";
        }
        if (startError.data) {
            error.assign(startError.data, static_cast<size_t>(startError.size));
            hostMemoryFree(startError.data);
        }
        if (std::chrono::steady_clock::now() - started > std::chrono::milliseconds(100)) {
            XX_LOGW("Plugin operation `{}` start blocked IO for over 100ms", label_);
        }
        if (submitted()) {
            if (!error.empty()) {
                XX_LOGW("Plugin operation `{}` returned error after done: {}", label_, error);
                error.clear();
            }
        } else if (!error.empty() || !providerHandle_) {
            if (error.empty()) {
                error = "protocol violation: null operation without done/error";
            }
            reject();
            return false;
        }
        if (!completed() && state() != PluginOperationState::Rejected) {
            state_.store(PluginOperationState::Running, std::memory_order_release);
        }
        if (!completed() && handle_->cancelled.load(std::memory_order_acquire)) {
            cancel();
        }
        return true;
    }

    /// register_task 与宿主 scheduler 的异步执行没有 provider start 返回值。
    void accept(std::function<void()> cancel = {}) {
        drive_.cancel = [cancel = std::move(cancel)](void*) {
            if (cancel) {
                cancel();
            }
        };
        state_.store(PluginOperationState::Running, std::memory_order_release);
    }

    /// 仅 IO 线程。提交完成和调用 cancel 互斥；同步 cancel→done 可重入。
    /// worker 必须持有自己的输入，且不得在提交 done 之前释放 cancel userdata。
    void cancel() noexcept {
        const auto tid = runtime_ ? runtime_->ioThreadId.load(std::memory_order_acquire)
                                  : std::thread::id{};
        if (!runtime_ || tid == std::thread::id{} || tid != std::this_thread::get_id()) {
            auto self = shared_from_this();
            if (!enqueueRuntimeAction(runtime_, [self] { self->cancelOnIo(); }, true)) {
                XX_LOGW(
                    "Plugin operation `{}` cancellation could not reach its IO executor",
                    label_
                );
            }
            return;
        }
        cancelOnIo();
    }

private:
    /// 取消与完成提交的线性化协议（违反其中任何一条都会重新引入死锁或重复终态）：
    ///
    /// 1. `completionSubmitted_` 是"done 已被接受"的唯一切换点：任意线程都只在
    ///    持有 `submitMutex_` 时读取与置位，因此重复 done、done 与 cancel/reject
    ///    竞争只会有一个赢家。
    /// 2. 持有 `submitMutex_` 期间**绝不调用插件或调用方代码**。插件可能在自己的
    ///    cancel 实现里同步调用 notify.done，若锁内进入插件就会自锁；因此这里
    ///    用普通 `std::mutex` 也安全（原先的 recursive_mutex 不再需要）。
    /// 3. 终态（Completed/Rejected）与调用方回调只在 IO 线程的 [commit] /
    ///    [reject] 中生效，且只在 `completionSubmitted_` 置位之后；
    ///    [cancelOnIo] 本身不产生终态，只把状态推进到 Cancelling。
    void cancelOnIo() noexcept {
        std::function<void(void*)> cancel;
        void*                     providerHandle = nullptr;
        {
            std::lock_guard lock(submitMutex_);
            const auto state = state_.load(std::memory_order_acquire);
            if (completionSubmitted_ || state == PluginOperationState::Completed
                || state == PluginOperationState::Rejected
                || state == PluginOperationState::Cancelling) {
                return;
            }
            state_.store(PluginOperationState::Cancelling, std::memory_order_release);
            handle_->cancelled.store(true, std::memory_order_release);
            cancel         = drive_.cancel;
            providerHandle = providerHandle_;
        }
        try {
            // 不在提交锁内进入插件。插件可能同步调用 notify.done，完成端点
            // 必须能够重新获取同一把锁，否则 cancel/done 会形成自锁。
            if (cancel) {
                cancel(providerHandle);
            }
        } catch (const std::exception& e) {
            XX_LOGW("Plugin operation `{}` cancel threw: {}", label_, e.what());
        } catch (...) {
            XX_LOGW("Plugin operation `{}` cancel threw unknown exception", label_);
        }
    }

public:

    bool submitted() const {
        std::lock_guard lock(submitMutex_);
        return completionSubmitted_;
    }

    /// 已拒绝请求无回调。登记到 provider/caller 的所有记录在这里回滚。
    void reject() {
        {
            std::lock_guard lock(submitMutex_);
            completionSubmitted_ = true;
        }
        state_.store(PluginOperationState::Rejected, std::memory_order_release);
        callback_ = nullptr;
        callbackUd_ = nullptr;
        completionHandler_ = {};
        drive_ = {};
        releaseRecords();
    }

    /// notify 的 host_ud 只指向宿主拥有的完成端点；Operation 已回收时，
    /// 迟到 done 只记录并丢弃，不访问已经失效的插件操作状态。
    static void AGENTXX_PLUGIN_CALL onEndpointDone(
        void* ud, int32_t status, const AgentxxPluginStringView* payload
    ) noexcept {
        auto* endpoint = static_cast<AgentxxPluginOperationCompletionEndpoint*>(ud);
        if (!endpoint) {
            return;
        }
        if (auto operation = endpoint->takeOperation()) {
            onDoneCore(operation.get(), status, payload);
        } else {
            XX_LOGW("Late plugin completion ignored after operation cleanup");
        }
    }

    AgentxxPluginOperatorNotify notify() noexcept {
        return {&OpCore::onEndpointDone, completionEndpoint_.get()};
    }

    static void onDoneCore(
        OpCore* self, int32_t status, const AgentxxPluginStringView* payload
    ) noexcept {
        if (!self) {
            return;
        }
        try {
            auto keep = self->shared_from_this();
            CompletionPacket packet;
            packet.status = status;
            try {
                if (payload && payload->data) {
                    packet.payload.assign(payload->data, static_cast<size_t>(payload->size));
                }
            } catch (...) {
                packet.status = AGENTXX_PLUGIN_OPERATOR_FAILED;
                packet.payload.clear();
            }
            {
                std::lock_guard lock(self->submitMutex_);
                if (self->completionSubmitted_) {
                    XX_LOGW("Plugin operation `{}` duplicate done ignored", self->label_);
                    return;
                }
                // 先置位再投递，避免两个 worker 同时完成时重复接受。
                self->completionSubmitted_ = true;
            }
            auto completion = std::make_shared<std::function<void()>>(
                [weak = std::weak_ptr<OpCore>(keep), packet = std::move(packet)]() mutable {
                    if (auto operation = weak.lock()) {
                        operation->commit(std::move(packet));
                    }
                }
            );
            if (!enqueueRuntimeAction(
                    self->runtime_,
                    [completion] { (*completion)(); },
                    true
                )) {
                // Never invoke plugin or caller code from this completion
                // thread. The runtime retains accepted actions while its IO
                // executor is stopped.
                XX_LOGE("Unable to enqueue plugin completion; runtime is unavailable");
            }
        } catch (...) {
            XX_LOGE("Plugin completion endpoint failed unexpectedly");
        }
    }

public:
    /// 一个 Operation 只有一个 await 等待者；完成处理器与等待者可同时使用。
    asio::awaitable<void> wait() {
        if (completed()) {
            co_return;
        }
        auto [ec] = co_await finished_.async_wait(asio::as_tuple(asio::use_awaitable));
        if (!completed()) {
            throw util::AsioSystemError(ec ? ec : OpErrorCode(asio::error::operation_aborted));
        }
    }

private:
    OpCore(std::shared_ptr<PluginRuntime> runtime, std::string label) :
        runtime_(std::move(runtime)), label_(std::move(label)),
        finished_(runtime_->executor, std::chrono::steady_clock::time_point::max()) {}

    /// 唯一终态入口：复制结果→失效取消→回调→清理记录/lease→唤醒等待者。
    void commit(CompletionPacket packet) {
        if (completed() || state() == PluginOperationState::Rejected) {
            return;
        }
        completion_ = std::move(packet);
        state_.store(PluginOperationState::Completed, std::memory_order_release);
        handle_->completed.store(true, std::memory_order_release);
        providerHandle_ = nullptr;
        auto callback = std::exchange(callback_, nullptr);
        auto* ud = std::exchange(callbackUd_, nullptr);
        try {
            if (callback) {
                auto sv = PluginStringView::from(completion_.payload);
                callback(ud, completion_.status, &sv);
            }
        } catch (const std::exception& e) {
            XX_LOGW("Plugin operation `{}` completion callback threw: {}", label_, e.what());
        } catch (...) {
            XX_LOGW("Plugin operation `{}` completion callback threw unknown exception", label_);
        }
        /// 外部 callback 违约抛异常不能跳过宿主完成处理器（例如 sleep 索引清理）。
        try {
            if (completionHandler_) {
                completionHandler_(completion_.status, completion_.payload);
            }
        } catch (const std::exception& e) {
            XX_LOGW("Plugin operation `{}` completion handler threw: {}", label_, e.what());
        } catch (...) {
            XX_LOGW("Plugin operation `{}` completion handler threw unknown exception", label_);
        }
        completionHandler_ = {};
        drive_ = {};
        if (completionEndpoint_) {
            completionEndpoint_->releaseOperation();
        }
        releaseRecords();
        finished_.cancel();
    }

    void releaseRecords(bool eraseActive = true) {
        if (completionEndpoint_) {
            completionEndpoint_->releaseOperation();
        }
        if (handle_) {
            handle_->completed.store(true, std::memory_order_release);
            if (eraseActive) {
                auto erase = [this](const OpGuardPtr& guard) {
                    if (guard && guard->inst) {
                        std::erase(guard->inst->outstandingOps, handle_);
                    }
                };
                erase(provider_);
                erase(caller_);
            }
        }
        {
            std::lock_guard lock(runtime_->operationsMutex);
            runtime_->operations.erase(id_);
        }
        caller_.reset();
        provider_.reset();
    }

    std::shared_ptr<PluginRuntime> runtime_;
    uint64_t id_ = 0;
    std::string label_;
    std::atomic<PluginOperationState> state_{PluginOperationState::Accepted};
    /// 取消/完成提交的互斥点。协议见 [cancelOnIo] 上方的说明：锁内不调用插件。
    mutable std::mutex submitMutex_;
    bool completionSubmitted_ = false;
    CompletionPacket completion_;
    OpDrive drive_;
    void* providerHandle_ = nullptr;
    OpGuardPtr provider_, caller_;
    std::shared_ptr<AgentxxPluginOperatorHandle> handle_;
    std::shared_ptr<AgentxxPluginOperationCompletionEndpoint> completionEndpoint_;
    AgentxxPluginOperatorCallback callback_ = nullptr;
    void* callbackUd_ = nullptr;
    std::function<void(int32_t, std::string_view)> completionHandler_;
    asio::steady_timer finished_;
};

/// 任意线程取消：先捕获独立句柄，再把完整检查与插件 cancel 调用交给 IO。
inline void cancelPluginOperation(AgentxxPluginOperatorHandle* handle) noexcept {
    if (!handle) {
        return;
    }
    std::shared_ptr<AgentxxPluginOperatorHandle> keep;
    try {
        keep = handle->shared_from_this();
        if (enqueueRuntimeAction(
                keep->runtime.lock(),
                [keep] {
                    if (!keep->completed.load(std::memory_order_acquire) && keep->cancelFn) {
                        keep->cancelFn();
                    }
                },
                true
            )) {
            return;
        }
    } catch (...) {
        // Fall through to the diagnostic below. Calling plugin code here
        // would violate the ABI's IO-thread contract.
    }
    if (keep) {
        if (auto runtime = keep->runtime.lock()) {
            if (enqueueRuntimeAction(
                    runtime,
                    [keep] {
                        if (!keep->completed.load(std::memory_order_acquire) && keep->cancelFn) {
                            keep->cancelFn();
                        }
                    },
                    true
                )) {
                return;
            }
        }
    }
    XX_LOGE("Unable to enqueue plugin cancellation");
}

struct PluginOpAwaitArgs {
    std::shared_ptr<PluginInstance> inst;
    std::string label;
    asio::any_io_executor ex;
    std::shared_ptr<neograph::graph::CancelToken> cancelToken;
    OpDrive drive;
};

inline asio::awaitable<std::string> awaitPluginOp(PluginOpAwaitArgs args) {
    auto manager = args.inst ? args.inst->manager.lock() : nullptr;
    if (!manager || !manager->isIoThread()) {
        throw std::runtime_error("plugin operation requires its IO executor");
    }
    if (!args.inst->enabled || (args.inst->lifetime && !args.inst->lifetime->acceptsOperations())) {
        throw std::runtime_error("plugin is closed or disabled");
    }
    auto core = OpCore::create(manager->runtime(), args.inst, nullptr, args.label);
    std::string error;
    if (!core->start(std::move(args.drive), error)) {
        throw std::runtime_error(fmt::format("plugin `{}` op {} failed: {}", args.inst->name, args.label, error));
    }

    std::shared_ptr<neograph::graph::CancelToken> cancel;
    if (args.cancelToken) {
        cancel = args.cancelToken->fork();
        cancel->bind_executor(args.ex);
        cancel->slot().assign([weak = std::weak_ptr<OpCore>(core)](asio::cancellation_type) {
            if (auto op = weak.lock()) {
                op->cancel();
            }
        });
        if (cancel->is_cancelled()) {
            core->cancel();
        }
    }
    std::exception_ptr abort;
    try {
        co_await core->wait();
    } catch (...) {
        // 清理后原样传播取消/NodeInterrupt；不把协程控制流转换为普通失败。
        abort = std::current_exception();
    }
    if (cancel) {
        cancel->slot().clear();
    }
    if (abort) {
        core->cancel();
        std::rethrow_exception(abort);
    }
    if (core->status() == AGENTXX_PLUGIN_OPERATOR_CANCELLED) {
        throw neograph::graph::CancelledException(fmt::format("plugin op `{}` cancelled", args.label));
    }
    if (core->status() != AGENTXX_PLUGIN_OPERATOR_OK) {
        throw std::runtime_error(core->payload().empty() ? "plugin operation failed" : core->payload());
    }
    co_return core->payload();
}

/// Invoke an optional Reset-v1 instance lifecycle hook through the same
/// completion protocol as tools and capabilities. The hook is called while a
/// lifecycle lease is held, and its completion is always observed on the
/// manager's IO executor before the caller proceeds to the next phase.
template<typename HookFn>
inline asio::awaitable<bool> awaitPluginLifecycle(
    const std::shared_ptr<PluginRuntime>& runtime,
    const std::shared_ptr<PluginInstanceBase>& instance,
    void* pluginCtx,
    HookFn hook,
    std::string_view label,
    std::string& error
) {
    error.clear();
    if (!hook || !runtime || !instance) {
        co_return true;
    }
    std::shared_ptr<OpCore> core;
    try {
        core = OpCore::create(runtime, instance, nullptr, std::string(label), true);
        OpDrive drive;
        drive.start = [hook, pluginCtx](const auto* notify, auto* errorOut) -> void* {
            return hook(pluginCtx, notify, errorOut);
        };
        if (!core->start(std::move(drive), error)) {
            co_return false;
        }
        try {
            co_await core->wait();
        } catch (const std::exception& e) {
            error = e.what();
            co_return false;
        } catch (...) {
            error = "plugin lifecycle wait failed";
            co_return false;
        }
        if (core->status() != AGENTXX_PLUGIN_OPERATOR_OK) {
            error = core->payload().empty() ? "plugin lifecycle hook failed" : core->payload();
            co_return false;
        }
        co_return true;
    } catch (const std::exception& e) {
        if (core && !core->submitted()) {
            core->reject();
        }
        error = e.what();
        co_return false;
    } catch (...) {
        if (core && !core->submitted()) {
            core->reject();
        }
        error = "plugin lifecycle hook failed unexpectedly";
        co_return false;
    }
}

} // namespace agentxx::plugin
