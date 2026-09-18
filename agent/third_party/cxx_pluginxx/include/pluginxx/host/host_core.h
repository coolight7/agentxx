/// pluginxx 宿主核心 (通用表的状态与方法实现, 与宿主领域无关)
///
/// 定位: 把**通用表** (log / json / config / plugins / events / capabilities /
/// scheduler / coroutine_runtime / tasks / cancel) 的实现从各宿主的管理器里收敛到
/// 一处, 宿主只需:
/// 1. 管理器继承本类 (`class MyManager : public pluginxx::PluginHostCore<MyInstance>`);
/// 2. 实现 [DomainHooks] 并调用 [PluginHostCore::setDomainHooks];
/// 3. 用 `pluginxx/host/tables_impl.h` 的 `queryGenericPluginIface` 装配 vtable 的
///    `query_interface` (通用表由内核提供, 领域表由宿主自己提供)。
///
/// 归属判据: 通用 = 与"会话/模型/工具/提示词/图"无关的表; 需要宿主语义的数据一律
/// 经 [DomainHooks] 取, 因此本头不包含任何宿主类型。
///
/// 线程约定: 除明确标注的方法外, 全部方法**只在宿主 IO 线程调用** (ABI 入口由
/// `tables_impl.h` 的 trampoline 先投递到 IO 线程); 实例的登记表 (订阅/能力/睡眠
/// 句柄) 因此无需额外加锁。
#pragma once

#include "pluginxx/host/abi_util.h"
#include "pluginxx/host/capability_registry.h"
#include "pluginxx/host/domain_hooks.h"
#include "pluginxx/host/event_bus.h"
#include "pluginxx/runtime/manager_base.h"
#include "pluginxx/runtime/op_driver.h"
#include "utilxx_base/log.h"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pluginxx {
namespace detail {

/// C ABI 字符串视图是否为空 (指针与 (指针,长度) 两种形态)
inline bool abiViewEmpty(const AgentxxPluginStringView* sv) noexcept {
    return !sv || !sv->data || sv->size == 0;
}

inline bool abiViewEmpty(const AgentxxPluginStringView& sv) noexcept {
    return !sv.data || sv.size == 0;
}

/// std::string_view → C ABI 视图 (**借用**; 调用方保证被引用内存长命)
inline AgentxxPluginStringView abiView(std::string_view sv) noexcept {
    return AgentxxPluginStringView{sv.data(), static_cast<uint64_t>(sv.size())};
}

/// 写 C ABI 出参错误串 (宿主堆内存; 插件经 host->free 释放)
inline void setErrorString(AgentxxPluginString* out, std::string_view msg) noexcept {
    if (!out) {
        return;
    }
    hostMemorySetString(out, msg);
}

/// 把 (key, 值) 追加到字符串映射 (能力/订阅等登记表的去重写入)
template<typename Entry, typename Key>
inline void upsertEntry(std::vector<Entry>& entries, const Key& key, Entry&& entry) {
    entries.erase(
        std::remove_if(entries.begin(), entries.end(), [&key](const Entry& existing) {
            return existing.name == key;
        }),
        entries.end()
    );
    entries.push_back(std::forward<Entry>(entry));
}

} // namespace detail

/// 宿主核心: 通用表的状态与实现
///
/// - `InstanceT` 须继承 [pluginxx::PluginInstanceBase] (通用表的实例登记表在基类上);
/// - 派生宿主负责领域部分: 领域表的 vtable 入口、加载/卸载/启停事务、领域注册表。
template<typename InstanceT>
class PluginHostCore : public PluginManagerBase<InstanceT> {
public:

    using Base        = PluginManagerBase<InstanceT>;
    using InstancePtr = std::shared_ptr<InstanceT>;

    explicit PluginHostCore(asio::any_io_executor ex = {}) :
        Base(std::move(ex)) {}

    /// 注入领域钩子
    /// - 宿主管理器实现 [DomainHooks] 后在构造函数体内调用
    ///   (`this->setDomainHooks(this)`), 必须在任何通用表入口被调用之前完成
    void setDomainHooks(DomainHooks* hooks) noexcept {
        hooks_ = hooks;
    }

    DomainHooks* domainHooks() const noexcept {
        return hooks_;
    }

    /// 能力注册表 (宿主其它代码可直接查询)
    std::shared_ptr<CapabilityRegistry> capabilities() const {
        return capabilities_;
    }

    // =====================================================================
    // capabilities 表 (能力注册 / 撤销 / 调用)
    // =====================================================================

    /// 声明能力 (**IO 线程**; 名称重复或实例正在关闭时失败)
    int registerCapability(InstanceT* inst, std::string_view capability) {
        if (!inst || capability.empty()) {
            return -1;
        }
        if (!this->acceptsRegistration(inst)) {
            XX_LOGW(
                "Plugin `{}` registerCapability rejected: instance is closing or disabled",
                inst->name
            );
            return -1;
        }
        const std::string capStr{capability};
        if (!capabilities_->registerCapability(capStr, inst->name)) {
            return -1;
        }
        detail::upsertEntry(
            inst->capabilityRegistrations,
            capStr,
            PluginCapabilityRegistration{capStr, nullptr, nullptr, nullptr}
        );
        return 0;
    }

    /// 声明带启动回调的能力 (**IO 线程**)
    int registerCapabilityEx(
        InstanceT*                           inst,
        std::string_view                     capability,
        AgentxxPluginCapabilityStartFunction start,
        AgentxxPluginOperatorCancelFunction  cancel,
        void*                                ctx
    ) {
        if (!inst || capability.empty() || !start) {
            return -1;
        }
        if (!this->acceptsRegistration(inst)) {
            XX_LOGW(
                "Plugin `{}` registerCapabilityEx rejected: instance is closing or disabled",
                inst->name
            );
            return -1;
        }
        const std::string capStr{capability};
        if (!capabilities_->registerCapability(capStr, inst->name, start, cancel, ctx)) {
            return -1;
        }
        detail::upsertEntry(
            inst->capabilityRegistrations,
            capStr,
            PluginCapabilityRegistration{capStr, start, cancel, ctx}
        );
        return 0;
    }

    /// 撤销本插件声明的能力 (**IO 线程**; 非本人声明返回 -1)
    int unregisterCapability(InstanceT* inst, std::string_view capability) {
        if (!inst || capability.empty()) {
            return -1;
        }
        const std::string capStr{capability};
        auto              it = std::find_if(
            inst->capabilityRegistrations.begin(),
            inst->capabilityRegistrations.end(),
            [&capStr](const PluginCapabilityRegistration& entry) {
                return entry.name == capStr;
            }
        );
        if (it == inst->capabilityRegistrations.end()) {
            return -1;
        }
        inst->capabilityRegistrations.erase(it);
        capabilities_->unregisterCapability(capStr, inst->name);
        return 0;
    }

    /// 能力是否可用 (1 = 可用)
    int hasCapability(std::string_view capability) const {
        if (capability.empty()) {
            return 0;
        }
        return capabilities_->has(capability) ? 1 : 0;
    }

    /// 撤销实例声明的全部能力 (**IO 线程**; 保留实例内的登记记录, 供启用事务重新声明)
    /// - 卸载 / 禁用路径调用; 登记记录由宿主在 stop 成功后一并清空
    void unregisterInstanceCapabilities(InstanceT* inst) {
        if (!inst) {
            return;
        }
        for (const auto& cap : inst->capabilityRegistrations) {
            capabilities_->unregisterCapability(cap.name, inst->name);
        }
    }

    /// 撤销实例声明的全部能力并清空登记记录 (**IO 线程**; 实例即将销毁/停用收尾)
    void clearInstanceCapabilities(InstanceT* inst) {
        if (!inst) {
            return;
        }
        unregisterInstanceCapabilities(inst);
        inst->capabilityRegistrations.clear();
    }

    /// 调用能力 (**IO 线程**; ABI 入口已把跨线程调用投递过来)
    /// - `caller` 为发起调用的插件实例 (持有其执行 lease, 卸载等待必然覆盖本次调用);
    /// - 完成通知经 [OpCore] 恒在 IO 线程发布 (即使插件同步 done);
    /// - 真正拒绝 (能力不存在/提供者不可用) 返回 NULL 并写 `error_out`, 此时不调用 `cb`。
    AgentxxPluginOperatorHandle* invokeCapabilityAsync(
        InstanceT*                     caller,
        std::string_view               capability,
        std::string_view               method,
        std::string_view               argsJson,
        AgentxxPluginOperatorCallback  cb,
        void*                          ud,
        AgentxxPluginString*           error_out
    ) {
        std::shared_ptr<OpCore> core;
        try {
            auto owner = caller ? caller->template sharedSelf<InstanceT>() : nullptr;
            if (!owner || !this->ioExecutor()) {
                throw std::runtime_error("invoke_capability_async: missing caller or IO executor");
            }
            const std::string cap      = std::string{capability};
            const auto*       entry    = capabilities_->get(cap);
            auto              provider = entry ? this->find(entry->provider) : nullptr;
            if (!entry || !entry->start || !provider || !provider->enabled
                || (provider->lifetime && !provider->lifetime->acceptsOperations())) {
                throw std::runtime_error("invoke_capability_async: capability not available: " + cap);
            }
            const auto binding = *entry;
            OpDrive    drive;
            drive.start = [binding,
                           owner,
                           meth = std::string{method},
                           args = std::string{argsJson}](const auto* notify, auto* error) -> void* {
                const auto m = detail::abiView(meth);
                const auto a = detail::abiView(args);
                return binding.start(binding.ctx, owner->hostView(), &m, &a, notify, error);
            };
            drive.cancel = [binding](void* op) {
                if (binding.cancel) {
                    binding.cancel(binding.ctx, op);
                }
            };
            core = OpCore::create(this->runtime(), provider, owner, cap);
            std::string error;
            if (!core->start(std::move(drive), error)) {
                detail::setErrorString(error_out, error);
                return nullptr;
            }
            core->setCallback(cb, ud);
            return core->handle();
        } catch (const std::exception& e) {
            if (core && !core->submitted()) {
                core->reject();
            }
            detail::setErrorString(error_out, e.what());
            return nullptr;
        }
    }

    // =====================================================================
    // events 表 (订阅 / 撤销 / 发布)
    // =====================================================================

    /// 订阅事件 (**IO 线程**)
    /// - `topic` 经 [DomainHooks::qualifyEventTopic] 补齐命名空间;
    /// - 事件回调在宿主事件线程执行, 每次回调都在实例执行 lease 保护下调用插件
    ///   (因此卸载的 idle 等待覆盖它);
    /// - 实例正在关闭/已禁用, 或宿主无事件后端时返回 nullptr。
    AgentxxPluginSubscription* subscribe(
        InstanceT*               inst,
        std::string_view         topic,
        void(AGENTXX_PLUGIN_CALL* handler)(const AgentxxPluginStringView* event_json, void* ud),
        void* ud
    ) {
        if (!inst || topic.empty() || !handler) {
            return nullptr;
        }
        if (!this->acceptsRegistration(inst)) {
            XX_LOGW("Plugin `{}` subscribe rejected: instance is closing or disabled", inst->name);
            return nullptr;
        }
        auto source = hooks_ ? hooks_->eventSource() : nullptr;
        if (!source) {
            XX_LOGW("Plugin `{}` subscribe rejected: host has no event source", inst->name);
            return nullptr;
        }

        auto sub     = std::make_shared<AgentxxPluginSubscription>();
        sub->source  = source;
        sub->topic   = hooks_ ? hooks_->qualifyEventTopic(topic) : std::string{topic};
        sub->inst    = inst->ownerSelf;
        sub->runtime = this->runtime();
        sub->handler = handler;
        sub->ud      = ud;

        const size_t subId = source->subscribe(
            sub->topic,
            [sub](std::string_view data) {
                if (!sub->alive.load(std::memory_order_acquire) || !sub->handler) {
                    return;
                }
                auto inst = sub->inst.lock();
                if (!inst || !inst->enabled || !inst->lifetime
                    || !inst->lifetime->acceptsOperations()) {
                    return;
                }
                PluginInstanceBase::InflightGuard guard(inst);
                if (!guard || !sub->alive.load(std::memory_order_acquire)) {
                    return;
                }
                try {
                    const auto view = detail::abiView(data);
                    sub->handler(&view, sub->ud);
                } catch (const std::exception& e) {
                    XX_LOGW("Plugin `{}` event handler threw: {}", inst->name, e.what());
                } catch (...) {
                    XX_LOGW("Plugin `{}` event handler threw unknown exception", inst->name);
                }
            }
        );
        if (subId == 0) {
            XX_LOGW("Plugin `{}` subscribe failed: event source rejected topic `{}`", inst->name, sub->topic);
            return nullptr;
        }
        sub->subscriptionId = subId;
        inst->subscriptionHandles.push_back(sub);
        inst->subscriptions.push_back(sub);
        return sub.get();
    }

    /// 撤销事件订阅 (**幂等**; 可在任意线程调用, 簿记自动回到 IO 线程)
    void unsubscribe(AgentxxPluginSubscription* sub) {
        unsubscribePluginSubscription(sub);
    }

    /// 撤销实例的全部事件订阅 (**IO 线程**; 不做总线往返, 直接令句柄失效)
    /// - 卸载 / 禁用路径调用; 句柄本体仍由实例保活, 插件持有的裸指针继续安全
    void revokeInstanceSubscriptions(InstanceT* inst) {
        if (!inst) {
            return;
        }
        // 先快照再逐个撤销: [detail::revokeSubscription] 会从实例的订阅表里移除句柄,
        // 不能边遍历同一个 vector 边删除。
        auto subs = inst->subscriptions;
        for (const auto& sub : subs) {
            if (!sub) {
                continue;
            }
            sub->alive.store(false, std::memory_order_release);
            detail::revokeSubscription(sub.get());
        }
        inst->subscriptions.clear();
    }

    /// 发布事件 (**IO 线程**; 0 成功)
    /// - `eventJson` 为空时按 `{}` 发布; 主题经 [DomainHooks::qualifyEventTopic] 补齐
    int publish(std::string_view topic, std::string_view eventJson) {
        if (topic.empty()) {
            return -1;
        }
        auto source = hooks_ ? hooks_->eventSource() : nullptr;
        if (!source) {
            return -1;
        }
        const std::string fullTopic = hooks_ ? hooks_->qualifyEventTopic(topic) : std::string{topic};
        const std::string payload   = eventJson.empty() ? std::string{"{}"} : std::string{eventJson};
        return source->publish(fullTopic, payload);
    }

    // =====================================================================
    // scheduler 表 (IO 投递 / 睡眠 / 阻塞工作委托)
    // =====================================================================

    /// 投递一次回调到 IO 线程 (**IO 线程**)
    /// - 恒异步: 回调经 OpCore 完成协议在 IO 线程发布, 因此不会在调用栈内重入
    AgentxxPluginOperatorHandle*
        postCallback(InstanceT* inst, void(AGENTXX_PLUGIN_CALL* fn)(void*), void* ud) {
        if (!inst || !fn || !this->isIoThread()) {
            return nullptr;
        }
        auto core = OpCore::create(
            this->runtime(),
            inst->template sharedSelf<InstanceT>(),
            nullptr,
            "scheduler post"
        );
        try {
            core->setCompletionHandler([fn, ud](int32_t, std::string_view) {
                fn(ud);
            });
            core->accept();
            auto notify = core->notify();
            notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
            return core->handle();
        } catch (...) {
            core->reject();
            throw;
        }
    }

    /// 定时器睡眠 (**IO 线程**); 完成/取消/失败都经完成回调恰好一次上报
    AgentxxPluginOperatorHandle* sleep(
        InstanceT*                    inst,
        int64_t                       ms,
        AgentxxPluginOperatorCallback cb,
        void*                         ud,
        AgentxxPluginString*          error_out
    ) {
        if (!inst || !cb || !this->isIoThread()) {
            detail::setErrorString(error_out, "scheduler sleep: invalid instance, callback, or thread");
            return nullptr;
        }
        if (ms < 0) {
            detail::setErrorString(error_out, "scheduler sleep: negative duration");
            return nullptr;
        }

        auto owner = inst->template sharedSelf<InstanceT>();
        if (!owner) {
            detail::setErrorString(error_out, "scheduler sleep: instance is unavailable");
            return nullptr;
        }

        std::shared_ptr<OpCore> core;
        try {
            core         = OpCore::create(this->runtime(), owner, nullptr, "scheduler sleep");
            auto  timer  = std::make_shared<asio::steady_timer>(this->ioExecutor());
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
            timer->async_wait([core, timer](const utilxx_base::AsioErrorCode& ec) {
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
                owner->sleepTimers.erase(core->handle());
                core->reject();
            }
            detail::setErrorString(error_out, e.what());
            return nullptr;
        } catch (...) {
            if (core) {
                owner->sleepTimers.erase(core->handle());
                core->reject();
            }
            detail::setErrorString(error_out, "scheduler sleep: failed to create timer");
            return nullptr;
        }
    }

    /// 阻塞工作委托到宿主工作线程池 (**IO 线程**)
    /// - 工作体本身在工作线程执行 (显式例外), 完成通知经 OpCore 回到 IO 线程;
    /// - `work` 收到的取消令牌只在本次工作调用期间有效 (插件不得保存);
    /// - 宿主无工作线程时立即以失败终结 (不静默丢工作)。
    AgentxxPluginOperatorHandle* offload(
        InstanceT* inst,
        void*(AGENTXX_PLUGIN_CALL* work)(
            void*                           ud,
            const AgentxxPluginCancelToken* token,
            AgentxxPluginString*            error_out
        ),
        void(AGENTXX_PLUGIN_CALL* done)(
            void*                          ud,
            int32_t                        status,
            void*                          result,
            const AgentxxPluginStringView* error
        ),
        void*                ud,
        AgentxxPluginString* error_out
    ) {
        if (!inst || !work || !this->isIoThread()) {
            detail::setErrorString(
                error_out,
                "scheduler offload: invalid instance, work, or thread"
            );
            return nullptr;
        }

        auto owner = inst->template sharedSelf<InstanceT>();
        if (!owner) {
            detail::setErrorString(error_out, "scheduler offload: instance is unavailable");
            return nullptr;
        }

        struct WorkResult {
            void* value = nullptr;
        };

        std::shared_ptr<OpCore> core;
        try {
            core             = OpCore::create(this->runtime(), owner, nullptr, "scheduler offload");
            auto result      = std::make_shared<WorkResult>();
            auto cancelState = std::make_shared<OffloadCancelState>();
            core->setCompletionHandler([result, done, ud](int32_t status, std::string_view error) {
                if (!done) {
                    return;
                }
                auto view = detail::abiView(error);
                if (status == AGENTXX_PLUGIN_OPERATOR_FAILED && error.empty()) {
                    view = detail::abiView("plugin offload failed");
                }
                done(ud, status, result->value, &view);
            });
            core->accept([cancelState] {
                cancelState->requested.store(true, std::memory_order_release);
            });

            const bool queued = [&]() {
                if (!hooks_ || !hooks_->postToWorkerThread([core, result, cancelState, work, ud]() {
                        AgentxxPluginCancelToken token{&isOffloadCancelled, cancelState.get()};
                        AgentxxPluginString    workError{};
                        int32_t                status = AGENTXX_PLUGIN_OPERATOR_OK;
                        std::string            error;
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
                        auto view   = detail::abiView(error);
                        notify.done(notify.host_ud, status, &view);
                    })) {
                    return false;
                }
                return true;
            }();

            if (!queued) {
                auto notify = core->notify();
                auto view   = detail::abiView("plugin offload: no thread pool");
                notify.done(notify.host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, &view);
            }
            return core->handle();
        } catch (const std::exception& e) {
            if (core) {
                core->reject();
            }
            detail::setErrorString(error_out, e.what());
            return nullptr;
        } catch (...) {
            if (core) {
                core->reject();
            }
            detail::setErrorString(error_out, "scheduler offload: failed to create operation");
            return nullptr;
        }
    }

    // =====================================================================
    // tasks 表 (后台任务托管)
    // =====================================================================

    /// 托管一个后台任务 (**IO 线程**)
    /// - `cancel_fn`/`cancel_ud`: 卸载取消时宿主回调 (IO 线程, 协作式);
    /// - `notify`: 【出参】插件协程结束 (帧销毁后) 经 `notify.done` 恰好一次上报;
    /// - 返回宿主托管句柄 (失败 NULL + error_out); 句柄仅用于 `cancel_task`,
    ///   宿主在任务 done 后自动回收。
    AgentxxPluginOperatorHandle* registerTask(
        InstanceT*                          inst,
        AgentxxPluginOperatorCancelFunction cancel_fn,
        void*                               cancel_ud,
        AgentxxPluginOperatorNotify*        notify,
        AgentxxPluginString*                error_out
    ) {
        if (notify) {
            *notify = {};
        }
        std::shared_ptr<OpCore> core;
        try {
            if (!inst || !notify || !this->isIoThread() || !this->acceptsRegistration(inst)) {
                throw std::runtime_error(
                    "register_task: missing instance/notify or wrong IO thread"
                );
            }
            core = OpCore::create(
                this->runtime(),
                inst->template sharedSelf<InstanceT>(),
                nullptr,
                "background task"
            );
            core->accept([cancel_fn, cancel_ud] {
                if (cancel_fn) {
                    cancel_fn(cancel_ud, nullptr);
                }
            });
            *notify = core->notify();
            return core->handle();
        } catch (const std::exception& e) {
            if (core) {
                core->reject();
            }
            detail::setErrorString(error_out, e.what());
            return nullptr;
        }
    }

protected:

    /// offload 取消状态: 令牌只在工作调用期间有效, 插件不得保存 token 或 host_ud
    struct OffloadCancelState {
        std::atomic<bool> requested{false};
    };

    static int32_t AGENTXX_PLUGIN_CALL isOffloadCancelled(const AgentxxPluginCancelToken* token) {
        auto* state = token ? static_cast<const OffloadCancelState*>(token->host_ud) : nullptr;
        return state && state->requested.load(std::memory_order_acquire) ? 1 : 0;
    }

    DomainHooks*                        hooks_ = nullptr;
    std::shared_ptr<CapabilityRegistry> capabilities_ = std::make_shared<CapabilityRegistry>();
};

} // namespace pluginxx
