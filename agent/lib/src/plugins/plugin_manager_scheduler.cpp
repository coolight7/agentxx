#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/util/log.h"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"

#include <algorithm>
#include <chrono>

namespace agentxx {
namespace plugin {

void* PluginManager::sleep(PluginInstance* inst, long ms, void (*cb)(void* ud), void* ud) {
    if (!inst || !cb || ms < 0 || !ioExecutor_) {
        return nullptr;
    }
    auto timer  = std::make_shared<asio::steady_timer>(ioExecutor_);
    auto item   = std::make_shared<PluginSleepTimer>();
    item->inst  = inst ? inst->self : std::weak_ptr<PluginInstance>{};
    item->timer = timer;
    item->cb    = cb;
    item->ud    = ud;
    inst->sleepTimers.emplace(item.get(), item);

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
    auto it = inst->sleepTimers.find(timerPtr);
    if (it != inst->sleepTimers.end()) {
        auto item = it->second;
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
                        } catch (...) {
                        }
                    }
                });
            }
        }
    }
}

void PluginManager::offload(
    PluginInstance* inst,
    volatile int*   cancel_flag,
    void* (*work)(void* ud, volatile int* cancel_flag, AgentxxPluginString* error_out),
    void (*done)(void* ud, void* result, AgentxxPluginStringView error),
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
        std::shared_ptr<PluginInstance> instKeep;
        volatile int*                   cancel_flag;
        void* (*work)(void* ud, volatile int* cancel_flag, AgentxxPluginString* error_out);
        void (*done)(void* ud, void* result, AgentxxPluginStringView error);
        void*                 ud;
        void*                 result = nullptr;
        AgentxxPluginString   error{nullptr, 0};
        asio::any_io_executor ex;
    };

    auto instKeep = inst->self.lock();
    if (!instKeep) {
        return;
    }
    auto task = std::make_shared<OffloadTask>(
        OffloadTask{instKeep, cancel_flag, work, done, ud, nullptr, {nullptr, 0}, ioExecutor_}
    );

    inst->inflight.fetch_add(1, std::memory_order_acq_rel);

    asio::post(*ctx->threadPool, [task]() {
        auto* instPtr = task->instKeep.get();
        try {
            task->result = task->work(task->ud, task->cancel_flag, &task->error);
        } catch (const std::exception& e) {
            task->error = agentxx_plugin_string_from_cstr(instPtr ? &instPtr->host : nullptr, e.what());
        } catch (...) {
            task->error = agentxx_plugin_string_from_cstr(
                instPtr ? &instPtr->host : nullptr,
                "unknown error in plugin offload"
            );
        }

        if (task->ex) {
            asio::post(task->ex, [task]() {
                auto* instPtr2 = task->instKeep.get();
                if (task->done) {
                    try {
                        task->done(
                            task->ud,
                            task->result,
                            task->error.data ? agentxx_plugin_string_to_sv(task->error)
                                             : agentxx_plugin_sv(nullptr, 0)
                        );
                    } catch (const std::exception& e) {
                        if (instPtr2) {
                            XX_LOGW("Plugin `{}` offload done threw: {}", instPtr2->name, e.what());
                        }
                    } catch (...) {
                        if (instPtr2) {
                            XX_LOGW(
                                "Plugin `{}` offload done threw unknown exception",
                                instPtr2->name
                            );
                        }
                    }
                }
                if (task->error.data) {
                    if (instPtr2) {
                        agentxx_plugin_string_free(&instPtr2->host, &task->error);
                    } else {
                        ::free(task->error.data);
                        task->error = {nullptr, 0};
                    }
                }
                if (instPtr2) {
                    instPtr2->inflight.fetch_sub(1, std::memory_order_acq_rel);
                }
            });
        } else {
            if (instPtr) {
                instPtr->inflight.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
    });
}

} // namespace plugin
} // namespace agentxx
