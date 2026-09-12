#include "agentxx/plugin/op_driver.h"

#include "agentxx/agent/context.h"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace agentxx::plugin {

namespace {

void setSchedulerError(AgentxxPluginString* out, std::string_view message) {
    hostMemorySetString(out, message);
}

/// CancelToken 只在 worker 调用期间存在。其状态由宿主 Operation 的取消路径
/// 设置，插件不能保存 token 或其中的 host_ud。
struct OffloadCancelState {
    std::atomic<bool> requested{false};
};

int32_t AGENTXX_PLUGIN_CALL isOffloadCancelled(const AgentxxPluginCancelToken* token) {
    auto* state = token ? static_cast<const OffloadCancelState*>(token->host_ud) : nullptr;
    return state && state->requested.load(std::memory_order_acquire) ? 1 : 0;
}

} // namespace

AgentxxPluginOperatorHandle* PluginManager::postCallback(
    PluginInstance* inst,
    void(AGENTXX_PLUGIN_CALL* fn)(void*),
    void* ud
) {
    if (!inst || !fn || !isIoThread()) {
        return nullptr;
    }
    auto core = OpCore::create(runtime(), inst->self.lock(), nullptr, "scheduler post");
    try {
        core->setCompletionHandler([fn, ud](int32_t, std::string_view) {
            fn(ud);
        });
        core->accept();
        // OpCore always posts its completion to the IO executor, so post_to_io never
        // re-enters the caller while await_suspend is still active.
        auto notify = core->notify();
        notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
        return core->handle();
    } catch (...) {
        core->reject();
        throw;
    }
}

AgentxxPluginOperatorHandle* PluginManager::sleep(
    PluginInstance*               inst,
    int64_t                       ms,
    AgentxxPluginOperatorCallback cb,
    void*                         ud,
    AgentxxPluginString*          error_out
) {
    if (!inst || !cb || !isIoThread()) {
        setSchedulerError(error_out, "scheduler sleep: invalid instance, callback, or thread");
        return nullptr;
    }
    if (ms < 0) {
        setSchedulerError(error_out, "scheduler sleep: negative duration");
        return nullptr;
    }

    auto owner = inst->self.lock();
    if (!owner) {
        setSchedulerError(error_out, "scheduler sleep: instance is unavailable");
        return nullptr;
    }

    std::shared_ptr<OpCore> core;
    try {
        core         = OpCore::create(runtime(), owner, nullptr, "scheduler sleep");
        auto  timer  = std::make_shared<asio::steady_timer>(ioExecutor_);
        auto* handle = core->handle();
        core->setCallback(cb, ud);
        core->setCompletionHandler([owner, handle](int32_t, std::string_view) {
            owner->sleepTimers.erase(handle);
        });
        core->accept([timer] {
            timer->cancel();
        });
        owner->sleepTimers.emplace(handle, handle->shared_from_this());
        timer->expires_after(std::chrono::milliseconds(ms));
        timer->async_wait([core, timer](const util::AsioErrorCode& ec) {
            auto notify = core->notify();
            notify.done(
                notify.host_ud,
                ec ? AGENTXX_PLUGIN_OPERATOR_CANCELLED : AGENTXX_PLUGIN_OPERATOR_OK,
                nullptr
            );
        });
        return handle;
    } catch (const std::exception& e) {
        if (core) {
            if (auto owner = inst->self.lock()) {
                owner->sleepTimers.erase(core->handle());
            }
            core->reject();
        }
        setSchedulerError(error_out, e.what());
        return nullptr;
    } catch (...) {
        if (core) {
            if (auto owner = inst->self.lock()) {
                owner->sleepTimers.erase(core->handle());
            }
            core->reject();
        }
        setSchedulerError(error_out, "scheduler sleep: failed to create timer");
        return nullptr;
    }
}

AgentxxPluginOperatorHandle* PluginManager::offload(
    PluginInstance* inst,
    void*(AGENTXX_PLUGIN_CALL* work)(void*, const AgentxxPluginCancelToken*, AgentxxPluginString*),
    void(AGENTXX_PLUGIN_CALL* done)(void*, int32_t, void*, const AgentxxPluginStringView*),
    void*                ud,
    AgentxxPluginString* error_out
) {
    if (!inst || !work || !isIoThread()) {
        setSchedulerError(error_out, "scheduler offload: invalid instance, work, or thread");
        return nullptr;
    }

    auto owner = inst->self.lock();
    if (!owner) {
        setSchedulerError(error_out, "scheduler offload: instance is unavailable");
        return nullptr;
    }

    struct WorkResult {
        void* value = nullptr;
    };

    std::shared_ptr<OpCore> core;
    try {
        core             = OpCore::create(runtime(), owner, nullptr, "scheduler offload");
        auto result      = std::make_shared<WorkResult>();
        auto cancelState = std::make_shared<OffloadCancelState>();
        core->setCompletionHandler([result, done, ud](int32_t status, std::string_view error) {
            if (!done) {
                return;
            }
            auto view = PluginStringView::from(error);
            if (status == AGENTXX_PLUGIN_OPERATOR_FAILED && error.empty()) {
                view = PluginStringView::fromCstr("plugin offload failed");
            }
            done(ud, status, result->value, &view);
        });
        core->accept([cancelState] {
            cancelState->requested.store(true, std::memory_order_release);
        });

        auto ctx = agentContext_.lock();
        if (!ctx || !ctx->threadPool) {
            auto notify = core->notify();
            auto error  = PluginStringView::fromCstr("plugin offload: no thread pool");
            notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &error);
            return core->handle();
        }

        AgentxxPluginCancelToken token{&isOffloadCancelled, cancelState.get()};
        try {
            asio::post(*ctx->threadPool, [core, result, cancelState, token, work, ud]() mutable {
                AgentxxPluginString workError{};
                int32_t             status = AGENTXX_PLUGIN_OPERATOR_OK;
                std::string         error;
                try {
                    result->value = work(ud, &token, &workError);
                    if (workError.data) {
                        error.assign(workError.data, static_cast<size_t>(workError.size));
                        status = AGENTXX_PLUGIN_OPERATOR_FAILED;
                    }
                    if (cancelState->requested.load(std::memory_order_acquire)) {
                        status = AGENTXX_PLUGIN_OPERATOR_CANCELLED;
                    }
                } catch (const std::exception& e) {
                    status = AGENTXX_PLUGIN_OPERATOR_FAILED;
                    try {
                        error = e.what();
                    } catch (...) {
                    }
                } catch (...) {
                    status = AGENTXX_PLUGIN_OPERATOR_FAILED;
                    error  = "plugin offload worker threw unknown exception";
                }
                hostMemoryFree(workError.data);
                auto notify = core->notify();
                auto view   = PluginStringView::from(error);
                notify.done(notify.host_ud, status, &view);
            });
        } catch (const std::exception& e) {
            auto notify = core->notify();
            auto view   = PluginStringView::from(e.what());
            notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &view);
        } catch (...) {
            auto notify = core->notify();
            auto view   = PluginStringView::fromCstr("plugin offload: failed to queue worker");
            notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &view);
        }
        return core->handle();
    } catch (const std::exception& e) {
        if (core) {
            core->reject();
        }
        setSchedulerError(error_out, e.what());
        return nullptr;
    } catch (...) {
        if (core) {
            core->reject();
        }
        setSchedulerError(error_out, "scheduler offload: failed to create operation");
        return nullptr;
    }
}

} // namespace agentxx::plugin
