#include "agentxx/plugin/op_driver.h"

#include "agentxx/agent/context.h"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"

#include <chrono>

namespace agentxx::plugin {

/// 旧 scheduler ABI 的宿主适配层。排队、worker 执行和完成回调都由同一个
/// Operation 持有实例；R3 替换 ABI 签名时不再另建生命周期计数。
AgentxxPluginOperatorHandle* PluginManager::postCallback(
    PluginInstance* inst, void(AGENTXX_PLUGIN_CALL* fn)(void*), void* ud
) {
    if (!inst || !fn || !isIoThread()) {
        return nullptr;
    }
    auto core = OpCore::create(runtime(), inst->self.lock(), nullptr, "scheduler post");
    try {
        core->setCompletionHandler([fn, ud](int32_t, std::string_view) { fn(ud); });
        core->accept();
    } catch (...) {
        core->reject();
        throw;
    }
    /// 即便当前已经在 IO 线程，完成也只能排队执行，不能重入 await_suspend。
    auto notify = core->notify();
    notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    return core->handle();
}

void* PluginManager::sleep(
    PluginInstance* inst, int64_t ms, void(AGENTXX_PLUGIN_CALL* cb)(void*), void* ud
) {
    if (!inst || !cb || ms < 0 || !isIoThread()) {
        return nullptr;
    }
    auto owner = inst->self.lock();
    auto core = OpCore::create(runtime(), owner, nullptr, "scheduler sleep");
    try {
        auto timer = std::make_shared<asio::steady_timer>(ioExecutor_);
        auto* handle = core->handle();
        core->setCompletionHandler([owner, handle, cb, ud](int32_t, std::string_view) {
            /// 回调可以再次 sleep/cancel；先移除本次记录，避免重复取消或增长。
            owner->sleepTimers.erase(handle);
            cb(ud);
        });
        core->accept([timer] { timer->cancel(); });
        owner->sleepTimers.emplace(handle, handle->shared_from_this());
        timer->expires_after(std::chrono::milliseconds(ms));
        timer->async_wait([core, timer](const util::AsioErrorCode& ec) {
            auto notify = core->notify();
            notify.done(notify.host_ud, ec ? AGENTXX_PLUGIN_OPERATOR_CANCELLED
                                          : AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
        });
        return handle;
    } catch (...) {
        owner->sleepTimers.erase(core->handle());
        core->reject();
        throw;
    }
}

void PluginManager::cancelSleep(PluginInstance* inst, void* timer) {
    if (!inst || !timer || !isIoThread()) {
        return;
    }
    auto it = inst->sleepTimers.find(timer);
    if (it != inst->sleepTimers.end()) {
        cancelPluginOperation(it->second.get());
    }
}

void PluginManager::offload(
    PluginInstance* inst, volatile int32_t* cancel_flag,
    void*(AGENTXX_PLUGIN_CALL* work)(void*, volatile int32_t*, AgentxxPluginString*),
    void(AGENTXX_PLUGIN_CALL* done)(void*, void*, const AgentxxPluginStringView*), void* ud
) {
    if (!inst || !work || !isIoThread()) {
        return;
    }
    auto core = OpCore::create(runtime(), inst->self.lock(), nullptr, "scheduler offload");
    /// 只有 work 写 result，只有 IO completion 读 result；发布完成包建立同步。
    /// 函数指针和 ud 仅在 Operation lease 内调用，不在 task 析构时执行插件代码。
    struct WorkResult { void* value = nullptr; };
    std::shared_ptr<WorkResult> result;
    try {
        result = std::make_shared<WorkResult>();
        core->setCompletionHandler([result, done, ud](int32_t status, std::string_view error) {
            if (done) {
                auto view = PluginStringView::from(error);
                if (status == AGENTXX_PLUGIN_OPERATOR_FAILED && error.empty()) {
                    view = PluginStringView::fromCstr("plugin offload failed");
                }
                done(ud, result->value, &view);
            }
        });
        /// 旧 ABI 的 cancel_flag 仍由调用者维护，不能在这里再增加跨线程写入。
        /// R3 将移除该 volatile 参数，改为宿主 opaque CancelToken。
        core->accept();
    } catch (...) {
        core->reject();
        throw;
    }

    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->threadPool) {
        auto notify = core->notify();
        auto error = PluginStringView::fromCstr("plugin offload: no thread pool");
        notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &error);
        return;
    }
    try {
        asio::post(*ctx->threadPool, [core, result, work, cancel_flag, ud] {
            AgentxxPluginString error{};
            int32_t status = AGENTXX_PLUGIN_OPERATOR_OK;
            std::string message;
            try {
                result->value = work(ud, cancel_flag, &error);
                if (error.data) {
                    message.assign(error.data, static_cast<size_t>(error.size));
                    status = AGENTXX_PLUGIN_OPERATOR_FAILED;
                }
            } catch (const std::exception& e) {
                status = AGENTXX_PLUGIN_OPERATOR_FAILED;
                try { message = e.what(); } catch (...) {}
            } catch (...) {
                status = AGENTXX_PLUGIN_OPERATOR_FAILED;
            }
            hostMemoryFree(error.data);
            auto notify = core->notify();
            auto view = PluginStringView::from(message);
            /// work 已经返回，之后仅有宿主代码；done 先复制错误串再投递到 IO。
            notify.done(notify.host_ud, status, &view);
        });
    } catch (const std::exception& e) {
        auto notify = core->notify();
        auto view = PluginStringView::fromCstr(e.what());
        notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &view);
    } catch (...) {
        auto notify = core->notify();
        notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, nullptr);
    }
}

} // namespace agentxx::plugin
