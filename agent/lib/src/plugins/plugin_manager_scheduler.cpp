#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/util/log.h"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"

#include <algorithm>
#include <chrono>

namespace agentxx {
namespace plugin {

void* PluginManager::sleep(
    PluginInstance* inst,
    long            ms,
    void (*cb)(void* ud),
    void* ud
) {
    if (!inst || !cb || ms < 0 || !ioExecutor_) {
        return nullptr;
    }
    auto timer = std::make_shared<asio::steady_timer>(ioExecutor_);
    auto item  = std::make_shared<PluginSleepTimer>();
    item->inst = inst ? inst->self : std::weak_ptr<PluginInstance>{};
    item->timer = timer;
    item->cb    = cb;
    item->ud    = ud;
    inst->sleepTimers.push_back(item);

    timer->expires_after(std::chrono::milliseconds(ms));
    timer->async_wait([item, ex = ioExecutor_](const neograph_asio_error_code& ec) {
        (void)ec;
        bool exp = false;
        if (!item->triggered.compare_exchange_strong(exp, true, std::memory_order_acq_rel)) {
            return;
        }
        asio::post(ex, [item]() {
            auto inst = item->inst.lock();
            if (inst && item->cb) {
                PluginInstance::InflightGuard guard(inst);
                try {
                    item->cb(item->ud);
                } catch (const std::exception& e) {
                    XX_LOGW("Plugin `{}` sleep callback threw: {}", inst->name, e.what());
                } catch (...) {
                    XX_LOGW("Plugin `{}` sleep callback threw unknown exception", inst->name);
                }
            }
        });
    });
    return item.get();
}

void PluginManager::cancelSleep(PluginInstance* inst, void* timerPtr) {
    if (!inst || !timerPtr) {
        return;
    }
    auto it = std::find_if(
        inst->sleepTimers.begin(),
        inst->sleepTimers.end(),
        [timerPtr](const std::shared_ptr<PluginSleepTimer>& t) {
            return t.get() == timerPtr;
        }
    );
    if (it != inst->sleepTimers.end()) {
        auto item = *it;
        inst->sleepTimers.erase(it);
        if (item->timer) {
            item->timer->cancel();
        }
        bool exp = false;
        if (item->triggered.compare_exchange_strong(exp, true, std::memory_order_acq_rel)) {
            if (ioExecutor_) {
                asio::post(ioExecutor_, [item]() {
                    auto inst = item->inst.lock();
                    if (inst && item->cb) {
                        PluginInstance::InflightGuard guard(inst);
                        try {
                            item->cb(item->ud);
                        } catch (...) {}
                    }
                });
            }
        }
    }
}

void PluginManager::offload(
    PluginInstance* inst,
    volatile int*   cancel_flag,
    void* (*work)(void* ud, volatile int* cancel_flag, char** error_out),
    void (*done)(void* ud, void* result, char* error),
    void* ud
) {
    if (!inst || !work) {
        return;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->threadPool) {
        XX_LOGW("Plugin `{}` offload: no thread pool in AgentContext", inst->name);
        return;
    }

    struct OffloadTask {
        PluginInstance* inst;
        volatile int*   cancel_flag;
        void* (*work)(void* ud, volatile int* cancel_flag, char** error_out);
        void (*done)(void* ud, void* result, char* error);
        void*                 ud;
        void*                 result = nullptr;
        char*                 error  = nullptr;
        asio::any_io_executor ex;
    };

    auto task = std::make_shared<OffloadTask>(OffloadTask{
        inst,
        cancel_flag,
        work,
        done,
        ud,
        nullptr,
        nullptr,
        ioExecutor_
    });

    inst->inflight.fetch_add(1, std::memory_order_acq_rel);

    asio::post(
        *ctx->threadPool,
        [task, this]() {
            try {
                task->result = task->work(task->ud, task->cancel_flag, &task->error);
            } catch (const std::exception& e) {
                task->error = task->inst->host.vtable->strdup(e.what());
            } catch (...) {
                task->error = task->inst->host.vtable->strdup("unknown error in plugin offload");
            }

            if (task->ex) {
                asio::post(
                    task->ex,
                    [task]() {
                        if (task->done) {
                            try {
                                task->done(task->ud, task->result, task->error);
                            } catch (const std::exception& e) {
                                XX_LOGW("Plugin `{}` offload done threw: {}", task->inst->name, e.what());
                            } catch (...) {
                                XX_LOGW("Plugin `{}` offload done threw unknown exception", task->inst->name);
                            }
                        }
                        task->inst->inflight.fetch_sub(1, std::memory_order_acq_rel);
                    }
                );
            } else {
                task->inst->inflight.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
    );
}

} // namespace plugin
} // namespace agentxx
