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
#include <utility>

namespace agentxx::plugin {

struct OpDrive {
    std::function<void*(const AgentxxPluginOperatorNotify*, AgentxxPluginString*)> start;
    std::function<void(void*)> cancel;
};

using OpErrorCode = util::AsioErrorCode;
using OpGuardPtr = std::shared_ptr<PluginInstance::InflightGuard>;

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
        const std::shared_ptr<PluginInstance>& provider,
        const std::shared_ptr<PluginInstance>& caller,
        std::string label,
        bool lifecycle = false
    ) {
        if (!runtime || !runtime->executor || !provider || !provider->enabled
            || (!lifecycle && provider->lifetime && !provider->lifetime->acceptsRegistration())) {
            throw std::runtime_error("plugin operation rejected: provider is closed or disabled");
        }
        auto core = std::shared_ptr<OpCore>(new OpCore(std::move(runtime), std::move(label)));
        core->provider_ = std::make_shared<PluginInstance::InflightGuard>(provider, lifecycle);
        if (!*core->provider_) {
            throw std::runtime_error("plugin operation rejected: provider is closing");
        }
        if (caller) {
            if (!caller->enabled) {
                throw std::runtime_error("plugin operation rejected: caller is disabled");
            }
            core->caller_ = std::make_shared<PluginInstance::InflightGuard>(caller, lifecycle);
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
        core->handle_->executor = core->runtime_->executor;
        core->handle_->cancelFn = [weak = std::weak_ptr<OpCore>(core)] {
            if (auto operation = weak.lock()) {
                operation->cancel();
            }
        };
        core->id_ = core->runtime_->nextOperationId++;
        try {
            core->runtime_->operations.emplace(core->id_, core);
            provider->outstandingOps.push_back(core->handle_);
            provider->completionEndpoints.push_back(core->completionEndpoint_);
            if (caller && caller != provider) {
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
    PluginOperationState state() const noexcept { return state_; }
    int32_t status() const noexcept { return completion_.status; }
    const std::string& payload() const noexcept { return completion_.payload; }
    bool completed() const noexcept { return state_ == PluginOperationState::Completed; }

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
        state_ = PluginOperationState::Running;
        if (handle_->cancelled.load(std::memory_order_acquire)) {
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
        state_ = PluginOperationState::Running;
    }

    /// 仅 IO 线程。提交完成和调用 cancel 互斥；同步 cancel→done 可重入。
    /// worker 必须持有自己的输入，且不得在提交 done 之前释放 cancel userdata。
    void cancel() noexcept {
        std::lock_guard lock(submitMutex_);
        if (completionSubmitted_ || state_ == PluginOperationState::Completed
            || state_ == PluginOperationState::Rejected || state_ == PluginOperationState::Cancelling) {
            return;
        }
        state_ = PluginOperationState::Cancelling;
        handle_->cancelled.store(true, std::memory_order_release);
        try {
            if (drive_.cancel) {
                drive_.cancel(providerHandle_);
            }
        } catch (const std::exception& e) {
            XX_LOGW("Plugin operation `{}` cancel threw: {}", label_, e.what());
        } catch (...) {
            XX_LOGW("Plugin operation `{}` cancel threw unknown exception", label_);
        }
    }

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
        state_ = PluginOperationState::Rejected;
        callback_ = nullptr;
        callbackUd_ = nullptr;
        completionHandler_ = {};
        drive_ = {};
        releaseRecords();
    }

    /// 完成端点使用 weak_ptr，不依赖插件继续持有/调用 OpCore 裸指针。
    static void AGENTXX_PLUGIN_CALL onDone(
        void* ud, int32_t status, const AgentxxPluginStringView* payload
    ) noexcept {
        onDoneCore(static_cast<OpCore*>(ud), status, payload);
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
        if (auto operation = endpoint->operation.lock()) {
            onDoneCore(operation.get(), status, payload);
        } else {
            XX_LOGW("Late plugin completion ignored after operation cleanup");
        }
    }

    AgentxxPluginOperatorNotify notify() noexcept {
        return {&OpCore::onEndpointDone, completionEndpoint_.get()};
    }

private:
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
            try {
                asio::post(
                    self->runtime_->executor,
                    [keep, packet = std::move(packet)]() mutable {
                        keep->commit(std::move(packet));
                    }
                );
            } catch (...) {
                // executor 已停止时不能从完成线程调用插件 callback。保留 runtime
                // 和 lease，关闭路径会报告 CloseFailed，而不是提前 dlclose。
                XX_LOGE("Unable to enqueue plugin completion; runtime retains the operation");
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
        if (completed() || state_ == PluginOperationState::Rejected) {
            return;
        }
        completion_ = std::move(packet);
        state_ = PluginOperationState::Completed;
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
        releaseRecords();
        finished_.cancel();
    }

    void releaseRecords() {
        if (handle_) {
            handle_->completed.store(true, std::memory_order_release);
            auto erase = [this](const OpGuardPtr& guard) {
                if (guard && guard->inst) {
                    std::erase(guard->inst->outstandingOps, handle_);
                }
            };
            erase(provider_);
            erase(caller_);
        }
        runtime_->operations.erase(id_);
        caller_.reset();
        provider_.reset();
    }

    std::shared_ptr<PluginRuntime> runtime_;
    uint64_t id_ = 0;
    std::string label_;
    PluginOperationState state_ = PluginOperationState::Accepted;
    mutable std::recursive_mutex submitMutex_;
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
    try {
        auto keep = handle->shared_from_this();
        asio::post(keep->executor, [keep] {
            if (!keep->completed.load(std::memory_order_acquire) && keep->cancelFn) {
                keep->cancelFn();
            }
        });
    } catch (...) {
        XX_LOGE("Unable to enqueue plugin cancellation");
    }
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

} // namespace agentxx::plugin
