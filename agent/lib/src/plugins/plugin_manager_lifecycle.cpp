#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/op_driver.h"
#include "agentxx/plugin/plugin_graph_node.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/resource_applier.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/plugin/plugin_common.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "asio/as_tuple.hpp"
#include "asio/steady_timer.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

#if XX_IS_WIN_D
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace agentxx {
namespace plugin {

const void* AGENTXX_PLUGIN_CALL
    xx_query_interface(const AgentxxPluginHost*, const AgentxxPluginStringView* iid);

// =====================================================================
// NativeLoader
// =====================================================================

void* NativeLoader::open(const std::string& path, std::string& err) {
#if XX_IS_WIN_D
    std::wstring wpath;
    {
        int len = ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (len > 0) {
            wpath.resize(static_cast<size_t>(len) - 1);
            ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), len);
        }
    }
    HMODULE h = ::LoadLibraryW(wpath.c_str());
    if (!h) {
        err = fmt::format("LoadLibrary failed: error {}", ::GetLastError());
        return nullptr;
    }
    return reinterpret_cast<void*>(h);
#else
    ::dlerror();
    void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* d = ::dlerror();
        err           = d ? d : "dlopen failed";
        return nullptr;
    }
    return h;
#endif
}

void* NativeLoader::sym(void* handle, const char* name, std::string& err) {
#if XX_IS_WIN_D
    FARPROC p = ::GetProcAddress(reinterpret_cast<HMODULE>(handle), name);
    if (!p) {
        err = fmt::format("GetProcAddress({}) failed: error {}", name, ::GetLastError());
        return nullptr;
    }
    return reinterpret_cast<void*>(p);
#else
    ::dlerror();
    void*       p = ::dlsym(handle, name);
    const char* d = ::dlerror();
    if (d) {
        err = fmt::format("dlsym({}) failed: {}", name, d);
        return nullptr;
    }
    return p;
#endif
}

void NativeLoader::close(void* handle) {
    if (!handle) {
        return;
    }
#if XX_IS_WIN_D
    ::FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
}

void NativeLoader::addSearchPath(std::string_view dir) {
    (void)dir;
}

// =====================================================================
// PluginInstance
// =====================================================================

/// 未终结 Operation 摘要（见 [PluginRuntime::pendingOperationSummary]）。
/// 完成包在 executor 停止期间保留在待重放队列，Operation 因此仍未终结；关闭
/// 超时把它作为可观察线索输出。
std::string PluginRuntime::pendingOperationSummary() const {
    std::vector<std::string> items;
    {
        std::lock_guard lock(operationsMutex);
        for (const auto& entry : operations) {
            const auto& operation = entry.second;
            if (!operation || operation->completed()) {
                continue;
            }
            std::string item = operation->label();
            item += '#';
            item += std::to_string(entry.first);
            items.push_back(std::move(item));
        }
    }
    std::string out;
    for (const auto& item : items) {
        if (!out.empty()) {
            out += ", ";
        }
        out += item;
    }
    return out;
}

PluginInstance::~PluginInstance() {
    if (lifecycleStopPending()) {
        // stop 从未执行：此时 destroy 会看到不完整的插件状态。析构无法“保留”
        // 对象本身，因此宁可泄漏上下文与 DSO，也不能调用插件 destroy/dlclose。
        XX_LOGE(
            "Plugin `{}` destroyed with lifecycle stop pending; keeping plugin context "
            "and DSO loaded (owner must call shutdownAsync before destruction)",
            name
        );
        return;
    }
    if (!destroyPlugin()) {
        // 析构不能绕过仍在运行的插件代码。此处宁可保留动态库句柄，
        // 也不能在 worker/callback 仍可能执行时 dlclose。
        XX_LOGE(
            "Plugin `{}` destroyed while {} lease(s) remain; refusing to dlclose",
            name,
            lifetime ? lifetime->leaseCount() : inflight.load(std::memory_order_acquire)
        );
        return;
    }
    if (dlHandle) {
        NativeLoader::close(dlHandle);
        dlHandle = nullptr;
    }
}

bool PluginInstance::destroyPlugin() noexcept {
    if (pluginDestroyed) {
        return true;
    }
    if (lifetime && lifetime->leaseCount() != 0) {
        destroyDeferred = true;
        XX_LOGE(
            "Plugin `{}` destroy deferred while {} lease(s) are still active",
            name,
            lifetime->leaseCount()
        );
        return false;
    }
    if (!pluginCreated) {
        pluginDestroyed = true;
        destroyDeferred = false;
        retireHostControl();
        return true;
    }

    AgentxxPluginDestroyFn destroy = builtinUnload;
    if (dlHandle) {
        std::string err;
        destroy = reinterpret_cast<AgentxxPluginDestroyFn>(
            NativeLoader::sym(dlHandle, AGENTXX_PLUGIN_AGENT_SYMBOL_DESTROY, err)
        );
        if (!destroy && !err.empty()) {
            XX_LOGW("Plugin `{}` has no destroy entry: {}", name, err);
        }
    }
    if (destroy) {
        try {
            destroy(pluginCtx);
        } catch (const std::exception& e) {
            XX_LOGW("Plugin `{}` destroy threw: {}", name, e.what());
        } catch (...) {
            XX_LOGW("Plugin `{}` destroy threw unknown exception", name);
        }
    }
    pluginCtx       = nullptr;
    pluginDestroyed = true;
    destroyDeferred = false;
    // 插件上下文已销毁：之后插件持有的旧 host 指针只能安全失败。
    retireHostControl();
    return true;
}

// =====================================================================
// PluginManager 核心生命周期
// =====================================================================

PluginManager::PluginManager(std::weak_ptr<agentxx::agent::AgentContext> agentContext) :
    agentContext_(std::move(agentContext)),
    capabilities_(std::make_shared<CapabilityRegistry>()) {
    if (auto ctx = agentContext_.lock()) {
        registry_ = ctx->toolRegistry ? ctx->toolRegistry : std::make_shared<ToolRegistry>();
    } else {
        registry_ = std::make_shared<ToolRegistry>();
    }
}

PluginManager::~PluginManager() {
    shutdownAll();
    if (hasPendingClose()) {
        XX_LOGW(
            "PluginManager destroyed with pending plugin shutdown; owner should await "
            "shutdownAsync() before stopping its IO executor"
        );
    }
}

void PluginManager::shutdownAll() {
    std::vector<std::string> names;
    names.reserve(plugins_.size());
    for (const auto& [name, inst] : plugins_) {
        (void)inst;
        names.push_back(name);
    }
    for (const auto& name : names) {
        auto inst = find(name);
        if (inst) {
            shutdownPlugin(inst);
        }
    }
    // shutdownPlugin 对仍有 lease 的实例会保留上下文和动态库；不能无条件
    // 清空实例表，否则最后一个 lease 释放后将失去可重试的关闭入口。
}

namespace {

/// 同步 owner 即将析构时，保留实例和 DSO 到最后一个 lease 释放。
/// cleanup 在 lifetime 所属 IO 线程执行；若 manager 已析构，weak 引用为空，
/// 仍会完成 plugin destroy 并由最后一个 shared_ptr 安全关闭 DSO。
void deferPluginShutdown(const std::shared_ptr<PluginInstance>& inst) {
    if (!inst || !inst->lifetime) {
        return;
    }
    if (!inst->lifetime->setIdleCleanup([inst] {
            if (inst->lifecycleStopPending()) {
                // stop 事务只能在 IO 线程上协作完成；idle 回调是同步上下文，
                // 不能在这里 begin/await。保留实例与动态库并保持 CloseFailed，
                // 交给仍存活的 owner 经 shutdownAsync 收尾。
                inst->lifetime->setState(PluginInstanceState::CloseFailed);
                XX_LOGE(
                    "Plugin `{}` idle cleanup: lifecycle stop still pending; keeping context and DSO",
                    inst->name
                );
                return;
            }
            if (!inst->destroyPlugin()) {
                XX_LOGE("Plugin `{}` idle cleanup still has active leases", inst->name);
                return;
            }
            inst->lifetime->setState(PluginInstanceState::Closed);
            // 管理器仍存活时释放同一实例的名称预占，使后续加载可以重试。
            // manager 已析构时 weak_ptr 为空，实例会在 cleanup 返回后自然释放。
            if (auto manager = inst->manager.lock()) {
                auto it = manager->plugins_.find(inst->name);
                if (it != manager->plugins_.end() && it->second == inst) {
                    manager->plugins_.erase(it);
                }
            }
        })) {
        XX_LOGW("Plugin `{}` already has an idle cleanup", inst->name);
    }
}

} // namespace

void PluginManager::shutdownPlugin(const std::shared_ptr<PluginInstance>& inst) {
    if (!inst || inst->unloadRequested) {
        return;
    }
    inst->unloadRequested = true;
    if (inst->lifetime) {
        inst->lifetime->requestClose();
    }
    for (const auto& dep :
         collectReverseRequiredDeps(plugins_, inst->name, /*onlyEnabled=*/false)) {
        auto depInst = find(dep);
        if (depInst && !depInst->unloadRequested) {
            shutdownPlugin(depInst);
        }
    }
    detachAll(inst.get());
    inst->tools.clear();
    eraseMiddleware(inst->middleware.get());
    inst->middleware = nullptr;
    if (auto c = agentContext_.lock()) {
        if (c->resourceApplier) {
            c->resourceApplier->removeAllOwned(inst->name);
        }
    }
    // stop 事务尚未完成时不能 destroy/dlclose：同步路径无法等待该事务，
    // 只能保留实例并标记 CloseFailed，等待 shutdownAsync/unloadAsync 收尾。
    if (inst->lifecycleStopPending()) {
        if (inst->lifetime) {
            inst->lifetime->setState(PluginInstanceState::CloseFailed);
        }
        XX_LOGE(
            "Plugin `{}` shutdown deferred: lifecycle stop pending; call shutdownAsync "
            "before destroying the owner",
            inst->name
        );
        return;
    }

    // 同步析构路径不能绕过运行中的 lease。注册已撤销，但插件上下文和动态库
    // 必须保留到所有已接受的 Operation/回调返回；完成回调随后可再次调用
    // unloadAsync 继续收尾。这里不强行 destroy，也不清空实例记录。
    if (inst->lifetime && inst->lifetime->leaseCount() != 0) {
        XX_LOGW(
            "Plugin shutdown deferred: `{}` still has {} active lease(s)",
            inst->name,
            inst->lifetime->leaseCount()
        );
        deferPluginShutdown(inst);
        return;
    }

    inst->destroyPlugin();
    if (!inst->pluginDestroyed) {
        XX_LOGW("Plugin shutdown deferred after destroy attempt: `{}`", inst->name);
        return;
    }
    if (inst->lifetime) {
        inst->lifetime->setState(PluginInstanceState::Closed);
    }
    plugins_.erase(inst->name);
    XX_LOGI("Plugin shutdown: {}", inst->name);
}

void PluginManager::detachAll(PluginInstance* inst) {
    if (!inst) {
        return;
    }

    /// 只请求取消，不删除活跃记录；终态提交负责唯一一次清理。
    /// 回调可能登记其他操作，因此遍历当前快照，避免 vector 迭代器失效。
    const auto operations = inst->outstandingOps;
    for (const auto& op : operations) {
        if (op && !op->completed.load(std::memory_order_acquire) && op->cancelFn) {
            op->cancelFn();
        }
    }

    for (const auto& name : inst->toolNames) {
        registry_->unregisterTool(name);
    }
    for (const auto& sub : inst->subscriptions) {
        if (sub && sub->bus && sub->subscriptionId != 0) {
            sub->bus->get<std::string>(sub->topic).unsubscribe(sub->subscriptionId);
            sub->subscriptionId = 0;
            sub->alive.store(false, std::memory_order_release);
            sub->inst.reset();
        }
    }
    inst->subscriptions.clear();

    for (const auto& cap : inst->capabilityRegistrations) {
        capabilities_->unregisterCapability(cap.name, inst->name);
    }

    for (const auto& graph : inst->graphNodeTypes) {
        if (graph.slot) {
            graph.slot->invalidate(inst);
        }
    }

    restorePromptBackup(inst);

    if (inst->middleware) {
        inst->middleware->disabled = true;
    }
}

void PluginManager::eraseMiddleware(PluginMiddlewareHandle* mw) {
    if (!mw) {
        return;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return;
    }
    auto& handles = ctx->middlewareHandleContext->handles;
    handles.erase(
        std::remove_if(
            handles.begin(),
            handles.end(),
            [mw](const std::shared_ptr<agentxx::middleware::BaseMiddlewareHandleInterface>& h) {
                return h.get() == mw;
            }
        ),
        handles.end()
    );
}

void PluginManager::disable(std::string_view name) {
    disableImpl(name, /*userInitiated=*/true);
}

void PluginManager::enable(std::string_view name) {
    enableImpl(name, /*userInitiated=*/true);
}

/// 摘除宿主侧注册（保留注册记录）；禁用与卸载共用。
void PluginManager::detachInstanceRegistrations(PluginInstance* inst) {
    if (!inst) {
        return;
    }
    detachAll(inst);
    eraseMiddleware(inst->middleware.get());
    inst->middleware = nullptr;
    if (auto c = agentContext_.lock()) {
        if (c->resourceApplier) {
            c->resourceApplier->setOwnerEnabled(inst->name, false);
        }
    }
}

/// 清空由插件 start 事务重新声明的注册记录（stop 成功后调用）。
void PluginManager::clearPluginOwnedRegistrations(PluginInstance* inst) {
    if (!inst) {
        return;
    }
    inst->toolNames.clear();
    inst->tools.clear();
    inst->hookRegistrations.clear();
    inst->capabilityRegistrations.clear();
    inst->graphNodeTypes.clear();
    inst->subscriptions.clear();
    inst->subscriptionHandles.clear();
}

void PluginManager::disableImpl(std::string_view name, bool userInitiated) {
    auto inst = find(name);
    if (!inst || !inst->enabled) {
        return;
    }
    if (inst->lifetime && inst->lifetime->closeRequested()) {
        // 已进入关闭流程: 不再接受启用状态变化，避免与 stop/destroy 交错。
        return;
    }
    if (userInitiated) {
        inst->userDisabled = true;
        inst->blockedByDependencies = false;
    } else {
        // 级联禁用: 只记录原因，不改写用户显式禁用标记。
        inst->blockedByDependencies = true;
    }
    inst->enabled = false;
    if (inst->lifetime) {
        inst->lifetime->setState(PluginInstanceState::Disabled);
    }
    detachInstanceRegistrations(inst.get());
    XX_LOGI("Plugin `{}` disabled ({})", inst->name, userInitiated ? "user" : "dependency");

    // 级联禁用依赖者: 只收集直接依赖者，再逐层递归，覆盖三级/菱形依赖。
    for (const auto& child : collectReverseRequiredDeps(plugins_, inst->name, /*onlyEnabled=*/true)) {
        disableImpl(child, /*userInitiated=*/false);
    }

    // start/stop 事务（R5）：导出 stop 的插件必须收到 stop 才能撤销自管资源
    // （订阅/线程/定时器）；同步入口把事务投递到本管理器 IO executor，
    // 完成后状态保持 Disabled。
    requestStopForDisable(inst);
}

void PluginManager::enableImpl(std::string_view name, bool userInitiated) {
    auto inst = find(name);
    if (!inst || inst->enabled) {
        return;
    }
    if (inst->lifetime && inst->lifetime->closeRequested()) {
        return;
    }
    if (!userInitiated && inst->userDisabled) {
        // 用户显式禁用的插件不被级联恢复（F09 / M8）。
        return;
    }
    // 先置位再递归: 既让注册复查通过，也让循环依赖不会无限递归。
    inst->enabled = true;
    if (userInitiated) {
        inst->userDisabled = false;
    }
    inst->blockedByDependencies = false;
    if (inst->lifetime) {
        inst->lifetime->setState(PluginInstanceState::Ready);
    }

    // 先启用必选依赖: 子插件的 start 需要父插件的能力/工具已经可用。
    for (const auto& dep : inst->depends) {
        enableImpl(dep, /*userInitiated=*/false);
    }

    if (inst->lifecycleStart) {
        // 托管插件: 注册由插件 start 事务重新声明，宿主只负责投递与结果处理。
        requestStartForEnable(inst);
    } else {
        // legacy 插件: 按宿主侧保存的记录恢复注册。
        restoreHostSideRegistrations(inst.get());
    }
    XX_LOGI("Plugin `{}` enabled ({})", inst->name, userInitiated ? "user" : "dependency");

    // 级联恢复因本插件被禁用的依赖者（不覆盖用户显式禁用，F09）。
    for (const auto& child : collectReverseRequiredDeps(plugins_, inst->name, /*onlyEnabled=*/false)) {
        enableImpl(child, /*userInitiated=*/false);
    }
}

/// 按需投递禁用事务：只有"已 start 且尚未 stop"的实例需要 stop。
void PluginManager::requestStopForDisable(const std::shared_ptr<PluginInstance>& inst) {
    if (!inst || !inst->lifecycleStopPending()) {
        return;
    }
    auto self = shared_from_this();
    try {
        asio::co_spawn(
            ioExecutor(),
            [self, inst]() -> asio::awaitable<void> {
                co_await self->stopForDisable(inst);
            },
            [inst](std::exception_ptr e) {
                if (!e) {
                    return;
                }
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    XX_LOGE("Plugin `{}` disable stop threw: {}", inst->name, ex.what());
                } catch (...) {
                    XX_LOGE("Plugin `{}` disable stop threw unknown", inst->name);
                }
            }
        );
    } catch (const std::exception& e) {
        XX_LOGW("Plugin `{}` disable stop could not be scheduled: {}", inst->name, e.what());
    }
}

/// 按需投递启用事务（导出 start 的插件）。
void PluginManager::requestStartForEnable(const std::shared_ptr<PluginInstance>& inst) {
    if (!inst || !inst->lifecycleStart) {
        return;
    }
    auto self = shared_from_this();
    try {
        asio::co_spawn(
            ioExecutor(),
            [self, inst]() -> asio::awaitable<void> {
                co_await self->startForEnable(inst);
            },
            [inst](std::exception_ptr e) {
                if (!e) {
                    return;
                }
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    XX_LOGE("Plugin `{}` enable start threw: {}", inst->name, ex.what());
                } catch (...) {
                    XX_LOGE("Plugin `{}` enable start threw unknown", inst->name);
                }
            }
        );
    } catch (const std::exception& e) {
        XX_LOGW("Plugin `{}` enable start could not be scheduled: {}", inst->name, e.what());
    }
}

/// 禁用事务的异步部分（IO 线程）：调用插件 stop 撤销自管资源。
/// - 期间被重新启用时本次事务作废，由 [startForEnable] 先 stop 再 start；
/// - stop 失败只记录日志并保持 Disabled，实例仍保留（可重试或直接卸载）。
asio::awaitable<void> PluginManager::stopForDisable(std::shared_ptr<PluginInstance> inst) {
    if (!inst || !inst->lifecycleStopPending()) {
        co_return;
    }
    // 实例已经开始关闭时, stop 由卸载路径负责补齐；这里再发一次会与
    // unload 的 stop/destroy 交错。
    if (inst->lifetime && inst->lifetime->closeRequested()) {
        co_return;
    }
    if (inst->enabled) {
        // 等待期间用户又启用了该插件：本次 stop 作废。
        co_return;
    }
    std::string error;
    if (!co_await awaitPluginLifecycle(
            runtime(), inst, inst->pluginCtx, inst->lifecycleStop, "plugin stop", error
        )) {
        XX_LOGE("Plugin `{}` stop failed while disabling: {}", inst->name, error);
        co_return;
    }
    inst->lifecycleStopped = true;
    // stop 成功: 插件侧注册已撤销，宿主侧记录同步清空，使下次 start 从干净状态
    // 重新声明，避免同一工具/能力在多次 enable/disable 后重复累积。
    clearPluginOwnedRegistrations(inst.get());
    XX_LOGI("Plugin `{}` stopped for disable", inst->name);
    co_return;
}

/// 启用事务的异步部分（IO 线程）：先补齐 stop（若仍欠着），再调用插件 start
/// 重新提交注册；start 成功后才把状态置回 Ready。
asio::awaitable<void> PluginManager::startForEnable(std::shared_ptr<PluginInstance> inst) {
    if (!inst || !inst->lifecycleStart) {
        co_return;
    }
    // enable 事务可能落后于 unload: 关闭/已关闭的实例不得被重新置为 Ready
    // (InstanceLifetime::setState 明确禁止 Closed 重新打开)。
    if (!inst->lifetime || inst->lifetime->closeRequested()
        || inst->lifetime->state() == PluginInstanceState::Closed) {
        co_return;
    }
    if (!inst->enabled) {
        co_return;
    }
    // disable 的 stop 事务可能还在排队或正在执行：必须先停干净再 start，
    // 否则新注册会叠加在未撤销的旧状态之上。
    if (inst->lifecycleStopPending()) {
        std::string stopError;
        if (!co_await awaitPluginLifecycle(
                runtime(), inst, inst->pluginCtx, inst->lifecycleStop, "plugin stop", stopError
            )) {
            if (inst->lifetime) {
                inst->lifetime->setState(PluginInstanceState::Disabled);
            }
            XX_LOGE("Plugin `{}` enable aborted, stop failed: {}", inst->name, stopError);
            co_return;
        }
        inst->lifecycleStopped = true;
        clearPluginOwnedRegistrations(inst.get());
    }
    if (!inst->enabled) {
        // 等待 stop 期间用户又禁用了该插件。
        co_return;
    }
    if (inst->lifetime->closeRequested()
        || inst->lifetime->state() == PluginInstanceState::Closed) {
        // 等待 stop 期间实例开始关闭: 不再 start。
        co_return;
    }

    std::string error;
    if (!co_await awaitPluginLifecycle(
            runtime(), inst, inst->pluginCtx, inst->lifecycleStart, "plugin start", error
        )) {
        // start 失败: 回到 Disabled 且不留部分注册；实例保持"stop 仍欠着"，
        // 卸载或下次启用时先 stop 清理，符合 plugin.md 第 7.2 节回滚顺序。
        inst->enabled = false;
        inst->blockedByDependencies = false;
        if (inst->lifetime && !inst->lifetime->closeRequested()) {
            inst->lifetime->setState(PluginInstanceState::Disabled);
        }
        detachInstanceRegistrations(inst.get());
        clearPluginOwnedRegistrations(inst.get());
        XX_LOGE("Plugin `{}` start failed while enabling: {}", inst->name, error);
        co_return;
    }
    inst->lifecycleStopped = false;
    if (auto c = agentContext_.lock(); c && c->resourceApplier) {
        c->resourceApplier->setOwnerEnabled(inst->name, true);
    }
    if (inst->lifetime && !inst->lifetime->closeRequested()) {
        inst->lifetime->setState(PluginInstanceState::Ready);
    }
    XX_LOGI("Plugin `{}` restarted after enable", inst->name);
    co_return;
}

/// 恢复宿主侧已保存的注册记录（legacy 插件路径；也用于 start 事务前的基础恢复）。
void PluginManager::restoreHostSideRegistrations(PluginInstance* inst) {
    if (!inst) {
        return;
    }
    for (const auto& tool : inst->tools) {
        registry_->registerTool(tool->get_definition().name, tool);
    }
    if (!inst->hookRegistrations.empty()) {
        auto ctx = agentContext_.lock();
        auto self = inst->self.lock();
        if (ctx && ctx->middlewareHandleContext && self) {
            inst->middleware = std::make_shared<PluginMiddlewareHandle>(
                fmt::format("{}_middleware", inst->name),
                agentContext_,
                std::move(self)
            );
            ctx->middlewareHandleContext->handles.push_back(inst->middleware);
            for (const auto& hook : inst->hookRegistrations) {
                AgentxxPluginHookSpec spec{};
                spec.point       = hook.point;
                spec.hook_start  = hook.start;
                spec.hook_cancel = hook.cancel;
                spec.user_data   = hook.ud;
                inst->middleware->setHook(spec);
            }
        }
    }
    for (const auto& cap : inst->capabilityRegistrations) {
        if (cap.start) {
            capabilities_->registerCapability(cap.name, inst->name, cap.start, cap.cancel, cap.ctx);
        } else {
            capabilities_->registerCapability(cap.name, inst->name);
        }
    }
    for (const auto& graph : inst->graphNodeTypes) {
        if (!graph.slot) {
            continue;
        }
        AgentxxPluginGraphNodeTypeSpec spec{};
        spec.type = strToSv(graph.type);
        spec.run_start = graph.run_start;
        spec.run_cancel = graph.run_cancel;
        spec.user_data = graph.user_data;
        spec.config_schema_json = strToSv(graph.config_schema_json);
        graph.slot->activate(inst->self.lock(), spec, inst->lifetime ? inst->lifetime->generation() : 0);
    }
    if (auto c = agentContext_.lock()) {
        if (c->resourceApplier) {
            c->resourceApplier->setOwnerEnabled(inst->name, true);
        }
    }
}

void PluginManager::flushPendingCleanup() {
    for (auto& item : pendingCleanups_) {
        if (auto mw = item.mw.lock()) {
            eraseMiddleware(mw.get());
        }
    }
    pendingCleanups_.clear();
}

asio::awaitable<bool> PluginManager::unloadAsync(
    std::string_view name, std::chrono::milliseconds timeout
) {
    const auto deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds::zero());
    co_return co_await unloadAsyncUntil(std::string{name}, deadline);
}

asio::awaitable<bool> PluginManager::unloadAsyncUntil(
    std::string name, std::chrono::steady_clock::time_point deadline
) {
    auto inst = find(name);
    if (!inst) {
        XX_LOGW("Plugin unload: `{}` not loaded", name);
        co_return false;
    }
    if (inst->unloadRequested) {
        // 同步 shutdownAll 可能已经摘除注册并登记了 idle cleanup；
        // 在 executor 仍运行时允许 shutdownAsync 接管这次关闭。
        if (!inst->lifetime || !inst->lifetime->closeRequested()) {
            co_return false;
        }
        inst->unloadRequested = false;
    }
    inst->unloadRequested = true;
    if (inst->lifetime) {
        inst->lifetime->requestClose();
    }

    for (const auto& dep :
         collectReverseRequiredDeps(plugins_, inst->name, /*onlyEnabled=*/false)) {
        auto depInst = find(dep);
        if (depInst) {
            if (!co_await unloadAsyncUntil(depInst->name, deadline)) {
                co_return false;
            }
        }
    }

    detachAll(inst.get());
    inst->tools.clear();
    eraseMiddleware(inst->middleware.get());
    inst->middleware = nullptr;
    if (auto c = agentContext_.lock()) {
        if (c->resourceApplier) {
            c->resourceApplier->removeAllOwned(inst->name);
        }
    }

    if (inst->lifecycleStopPending()) {
        std::string stopError;
        if (!co_await awaitPluginLifecycle(
                runtime(), inst, inst->pluginCtx, inst->lifecycleStop, "plugin stop", stopError
            )) {
            if (inst->lifetime) {
                inst->lifetime->setState(PluginInstanceState::CloseFailed);
            }
            inst->unloadRequested = false;
            XX_LOGE("Plugin `{}` stop failed: {}", inst->name, stopError);
            co_return false;
        }
        inst->lifecycleStopped = true;
    }

    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::max(deadline - std::chrono::steady_clock::now(), std::chrono::steady_clock::duration::zero())
    );
    bool ok = co_await waitInflightZero(inst, remaining);
    if (!ok) {
        if (inst->lifetime) {
            inst->lifetime->setState(PluginInstanceState::CloseFailed);
        }
        inst->unloadRequested = false;
        // 未终结 Operation 摘要：完成包可能已产生但没有投递到 IO 线程（executor
        // 停止时保留在待重放队列），这是 CloseFailed 的唯一可观察线索。
        const auto pending = runtime() ? runtime()->pendingOperationSummary() : std::string{};
        XX_LOGE(
            "Plugin `{}` unload timed out waiting for inflight callbacks (pending operations: {})",
            inst->name,
            pending.empty() ? "none" : pending
        );
        co_return false;
    }

    inst->destroyPlugin();
    if (!inst->pluginDestroyed) {
        XX_LOGW("Plugin `{}` unload deferred after destroy attempt", inst->name);
        co_return false;
    }
    if (inst->lifetime) {
        inst->lifetime->setState(PluginInstanceState::Closed);
    }
    plugins_.erase(name);
    XX_LOGI("Plugin `{}` unloaded", name);
    co_return true;
}

asio::awaitable<bool>
    PluginManager::shutdownAsync(std::chrono::milliseconds timeout) {
    std::vector<std::string> names;
    names.reserve(plugins_.size());
    for (const auto& [name, inst] : plugins_) {
        (void)inst;
        names.push_back(name);
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool       allClosed = true;
    for (const auto& name : names) {
        if (!find(name)) {
            continue;
        }
        const bool closed = co_await unloadAsyncUntil(name, deadline);
        allClosed         = closed && allClosed;
    }
    co_return allClosed && plugins_.empty();
}

std::vector<PluginManager::PluginListView> PluginManager::list() const {
    std::vector<PluginListView> out;
    out.reserve(plugins_.size());
    for (const auto& [name, inst] : plugins_) {
        PluginListView view;
        view.name               = inst->name;
        view.version            = inst->version;
        view.description        = inst->description;
        view.path               = inst->path;
        view.configPath         = inst->configPath;
        view.enabled            = inst->enabled;
        view.inflight           = inst->inflight.load(std::memory_order_acquire);
        view.tools              = inst->toolNames;
        view.depends            = inst->depends;
        view.optionalDepends    = inst->optionalDepends;
        view.requiredInterfaces = inst->interfaces.require;
        view.optionalInterfaces = inst->interfaces.optional;
        for (const auto& cap : inst->capabilityRegistrations) {
            view.capabilities.push_back(cap.name);
        }
        out.push_back(std::move(view));
    }
    return out;
}

std::string PluginManager::listPluginsJson() {
    auto                views = list();
    agentxx::util::Json arr   = agentxx::util::Json::array();
    for (const auto& v : views) {
        agentxx::util::Json item;
        item["name"]                = v.name;
        item["version"]             = v.version;
        item["description"]         = v.description;
        item["path"]                = v.path;
        item["config"]              = v.configPath;
        item["enabled"]             = v.enabled;
        item["tools"]               = v.tools;
        item["capabilities"]        = v.capabilities;
        item["depends"]             = v.depends;
        item["optional_depends"]    = v.optionalDepends;
        item["required_interfaces"] = v.requiredInterfaces;
        item["optional_interfaces"] = v.optionalInterfaces;
        arr.push_back(std::move(item));
    }
    return arr.dump();
}

std::string PluginManager::getPluginJson(const std::string& name) {
    auto inst = find(name);
    if (!inst) {
        return {};
    }
    agentxx::util::Json item;
    item["name"]                = inst->name;
    item["version"]             = inst->version;
    item["description"]         = inst->description;
    item["path"]                = inst->path;
    item["config"]              = inst->configPath;
    item["enabled"]             = inst->enabled;
    item["tools"]               = inst->toolNames;
    item["depends"]             = inst->depends;
    item["optional_depends"]    = inst->optionalDepends;
    item["required_interfaces"] = inst->interfaces.require;
    item["optional_interfaces"] = inst->interfaces.optional;
    agentxx::util::Json caps    = agentxx::util::Json::array();
    for (const auto& c : inst->capabilityRegistrations) {
        caps.push_back(c.name);
    }
    item["capabilities"] = std::move(caps);
    return item.dump();
}

// ==================== 加载分支 (Native / Builtin / Configured) ====================

// ---------------------------------------------------------------------------
// 内置插件路径 helper (yaml `builtin://<name>` 简写)
// ---------------------------------------------------------------------------
static inline bool isBuiltinScheme(std::string_view p) noexcept {
    return p.size() > 10 && p.substr(0, 10) == "builtin://";
}

static inline std::string parseBuiltinName(std::string_view p) {
    return std::string(p.substr(10));
}

asio::awaitable<std::shared_ptr<PluginInstance>> PluginManager::loadNativeAsync(
    std::string                             path,
    const agentxx::agent::PluginConfig*     cfg,
    bool                                    allowClientOnlySkip,
    const plugin::PluginManifestResources&  resources,
    const plugin::PluginManifestInterfaces& interfaces
) {
    (void)allowClientOnlySkip;
    std::string err;
    void*       dl = NativeLoader::open(path, err);
    if (!dl) {
        XX_LOGE("Plugin load failed: {}: {}", path, err);
        co_return nullptr;
    }

    auto getInfoFn = reinterpret_cast<AgentxxPluginGetInfoFn>(
        NativeLoader::sym(dl, AGENTXX_PLUGIN_AGENT_SYMBOL_GET_INFO, err)
    );
    std::string createErr;
    auto createFn = reinterpret_cast<AgentxxPluginCreateFn>(
        NativeLoader::sym(dl, AGENTXX_PLUGIN_AGENT_SYMBOL_CREATE, createErr)
    );
    std::string lifecycleErr;
    auto lifecycleStart = reinterpret_cast<AgentxxPluginStartFn>(
        NativeLoader::sym(dl, AGENTXX_PLUGIN_AGENT_SYMBOL_START, lifecycleErr)
    );
    lifecycleErr.clear();
    auto lifecycleStop = reinterpret_cast<AgentxxPluginStopFn>(
        NativeLoader::sym(dl, AGENTXX_PLUGIN_AGENT_SYMBOL_STOP, lifecycleErr)
    );

    if (!createFn) {
        NativeLoader::close(dl);
        if (allowClientOnlySkip) {
            XX_LOGW("Plugin `{}` skipped: no agent entry (client only)", path);
            co_return nullptr;
        }
        XX_LOGE("Plugin `{}` missing {}", path, AGENTXX_PLUGIN_AGENT_SYMBOL_CREATE);
        co_return nullptr;
    }

    const AgentxxPluginInfo* info = nullptr;
    try {
        info = getInfoFn ? getInfoFn() : nullptr;
    } catch (const std::exception& e) {
        XX_LOGW("Plugin `{}` get_info threw: {}", path, e.what());
    } catch (...) {
        XX_LOGW("Plugin `{}` get_info threw unknown exception", path);
    }
    if (info && info->api_version != AGENTXX_PLUGIN_API_VERSION) {
        NativeLoader::close(dl);
        XX_LOGE(
            "Plugin `{}` API version mismatch (got {}, host requires {})",
            path,
            info->api_version,
            AGENTXX_PLUGIN_API_VERSION
        );
        co_return nullptr;
    }

    std::string name = info && info->name.data ? std::string(info->name.data, info->name.size)
                                               : std::filesystem::path(path).stem().string();
    if (name.starts_with("lib")) {
        name = name.substr(3);
    }

    if (!reservePluginName(name)) {
        XX_LOGE("Plugin load rejected: duplicate or currently loading name `{}`", name);
        NativeLoader::close(dl);
        co_return nullptr;
    }

    auto inst      = std::make_shared<PluginInstance>(name);
    inst->lifetime = makeLifetime(name);
    inst->version = info && info->version.data ? std::string(info->version.data, info->version.size)
                                               : "1.0.0";
    inst->description = info && info->description.data
                            ? std::string(info->description.data, info->description.size)
                            : "";
    inst->path        = path;
    inst->dlHandle    = dl;
    inst->lifecycleStart = lifecycleStart;
    inst->lifecycleStop  = lifecycleStop;
    inst->interfaces  = interfaces;
    inst->self        = inst;
    inst->ownerSelf   = inst;
    inst->manager     = shared_from_this();
    auto vtableSv     = agentxx::plugin::PluginStringView::fromCstr("__vtable");
    // 交给插件的 host 视图必须放在进程级稳定的控制块里：插件可能在卸载后继续
    // 使用旧 host 指针，控制块 tombstone 保证这类迟到调用安全失败。
    inst->hostControl = PluginHostControl::create(
        inst, (const AgentxxHostVtable*)xx_query_interface(nullptr, &vtableSv)
    );
    if (cfg) {
        inst->args       = cfg->args;
        inst->configPath = cfg->configPath;
    }

    plugins_[name] = inst;
    int rc         = -1;
    try {
        rc = createFn(inst->hostView(), &inst->pluginCtx);
        // 即使 create 返回失败，只要交付了上下文，destroy 仍是宿主的责任。
        inst->pluginCreated = (inst->pluginCtx != nullptr);
    } catch (const std::exception& e) {
        XX_LOGE("Plugin `{}` create threw: {}", name, e.what());
        rc = -1;
    } catch (...) {
        XX_LOGE("Plugin `{}` create threw unknown exception", name);
        rc = -1;
    }

    if (rc != 0) {
        XX_LOGE("Plugin `{}` create failed (code={}), performing rollback", name, rc);
        detachAll(inst.get());
        eraseMiddleware(inst->middleware.get());
        inst->middleware = nullptr;
        if (auto c = agentContext_.lock()) {
            if (c->resourceApplier) {
                c->resourceApplier->removeAllOwned(inst->name);
            }
        }
        inst->destroyPlugin();
        plugins_.erase(name);
        releasePluginName(name);
        if (inst->dlHandle) {
            NativeLoader::close(inst->dlHandle);
            inst->dlHandle = nullptr;
        }
        co_return nullptr;
    }

    // New Reset-v1 plugins may split construction from registration/activation.
    // Legacy plugins have no lifecycle symbol and are already fully active after
    // create, so they continue directly to Ready.
    if (inst->lifecycleStart) {
        std::string startError;
        if (!co_await awaitPluginLifecycle(
                runtime(), inst, inst->pluginCtx, inst->lifecycleStart, "plugin start", startError
            )) {
            XX_LOGE("Plugin `{}` start failed: {}", name, startError);
            detachAll(inst.get());
            eraseMiddleware(inst->middleware.get());
            inst->middleware = nullptr;
            if (auto c = agentContext_.lock(); c && c->resourceApplier) {
                c->resourceApplier->removeAllOwned(inst->name);
            }
            inst->destroyPlugin();
            plugins_.erase(name);
            releasePluginName(name);
            if (inst->dlHandle) {
                NativeLoader::close(inst->dlHandle);
                inst->dlHandle = nullptr;
            }
            co_return nullptr;
        }
    }
    // 无论是否有 start 导出，走到这里实例都已激活：此后 destroy 前必须先
    // 完成 stop (若插件导出了 stop)。
    inst->lifecycleStarted = true;

    applyDeclaredResources(*inst, resources);
    inst->resourcesFrozen = true;
    inst->lifetime->setState(PluginInstanceState::Ready);
    releasePluginName(name);
    XX_LOGI("Plugin `{}` loaded successfully", name);
    co_return inst;
}

asio::awaitable<std::shared_ptr<PluginInstance>> PluginManager::loadBuiltinAsync(
    std::string                             name,
    std::string                             path,
    std::vector<std::string>                depends,
    std::vector<std::string>                optionalDepends,
    const agentxx::agent::PluginConfig*     cfg,
    const plugin::PluginManifestResources&  resources,
    const plugin::PluginManifestInterfaces& interfaces
) {
    auto entry = agentxx::plugin::findBuiltinPlugin(name);
    if (!entry) {
        XX_LOGE("Built-in plugin `{}` not found in registry", name);
        co_return nullptr;
    }

    const AgentxxPluginInfo* info = entry->get_info ? entry->get_info() : nullptr;
    if (info && info->api_version != AGENTXX_PLUGIN_API_VERSION) {
        XX_LOGE(
            "Builtin plugin `{}` API version mismatch (got {}, host requires {})",
            name,
            info->api_version,
            AGENTXX_PLUGIN_API_VERSION
        );
        co_return nullptr;
    }

    if (!reservePluginName(name)) {
        XX_LOGE("Builtin plugin load rejected: duplicate or currently loading name `{}`", name);
        co_return nullptr;
    }

    auto inst      = std::make_shared<PluginInstance>(name);
    inst->lifetime = makeLifetime(name);
    inst->version = info && info->version.data ? std::string(info->version.data, info->version.size)
                                               : "1.0.0";
    inst->description     = info && info->description.data
                                ? std::string(info->description.data, info->description.size)
                                : "";
    inst->path            = path;
    inst->depends         = std::move(depends);
    inst->optionalDepends = std::move(optionalDepends);
    inst->interfaces      = interfaces;
    inst->self            = inst;
    inst->ownerSelf       = inst;
    inst->manager         = shared_from_this();
    auto vtableSv2        = agentxx::plugin::PluginStringView::fromCstr("__vtable");
    inst->hostControl     = PluginHostControl::create(
        inst, (const AgentxxHostVtable*)xx_query_interface(nullptr, &vtableSv2)
    );
    inst->builtinUnload   = entry->destroy;
    inst->lifecycleStart  = entry->start;
    inst->lifecycleStop   = entry->stop;
    if (cfg) {
        inst->args       = cfg->args;
        inst->configPath = cfg->configPath;
    }

    plugins_[name] = inst;
    int rc         = -1;
    try {
        rc = entry->create(inst->hostView(), &inst->pluginCtx);
        inst->pluginCreated = (inst->pluginCtx != nullptr);
    } catch (const std::exception& e) {
        XX_LOGE("Builtin plugin `{}` create threw: {}", name, e.what());
        rc = -1;
    } catch (...) {
        XX_LOGE("Builtin plugin `{}` create threw unknown exception", name);
        rc = -1;
    }

    if (rc != 0) {
        XX_LOGE("Builtin plugin `{}` create failed (code={}), performing rollback", name, rc);
        detachAll(inst.get());
        eraseMiddleware(inst->middleware.get());
        inst->middleware = nullptr;
        if (auto c = agentContext_.lock()) {
            if (c->resourceApplier) {
                c->resourceApplier->removeAllOwned(inst->name);
            }
        }
        inst->destroyPlugin();
        plugins_.erase(name);
        releasePluginName(name);
        co_return nullptr;
    }

    if (inst->lifecycleStart) {
        std::string startError;
        if (!co_await awaitPluginLifecycle(
                runtime(), inst, inst->pluginCtx, inst->lifecycleStart, "plugin start", startError
            )) {
            XX_LOGE("Builtin plugin `{}` start failed: {}", name, startError);
            detachAll(inst.get());
            eraseMiddleware(inst->middleware.get());
            inst->middleware = nullptr;
            if (auto c = agentContext_.lock(); c && c->resourceApplier) {
                c->resourceApplier->removeAllOwned(inst->name);
            }
            inst->destroyPlugin();
            plugins_.erase(name);
            releasePluginName(name);
            co_return nullptr;
        }
    }
    inst->lifecycleStarted = true;

    applyDeclaredResources(*inst, resources);
    inst->resourcesFrozen = true;
    inst->lifetime->setState(PluginInstanceState::Ready);
    releasePluginName(name);
    XX_LOGI("Builtin plugin `{}` loaded successfully", name);
    co_return inst;
}

asio::awaitable<std::shared_ptr<PluginInstance>> PluginManager::loadPluginAsync(
    std::string                         path,
    const agentxx::agent::PluginConfig* cfg,
    bool                                allowClientOnlySkip
) {
    // 内置简写: builtin://<name> 直接经内置注册表加载 (无需外部目录/文件)
    if (isBuiltinScheme(path)) {
        auto btName = parseBuiltinName(path);
        if (btName.empty()) {
            XX_LOGE("Plugin load failed: invalid builtin path `{}`", path);
            co_return nullptr;
        }
        // 尝试从默认插件目录解析 manifest 以获取 depends/interfaces/resources
        // (可选: 失败则按无依赖/无资源处理, 不影响内置核心加载)
        std::vector<std::string> depends, optionalDepends;
        PluginManifestResources  resources;
        PluginManifestInterfaces interfaces;
        // 按可执行目录与当前工作目录探测 manifest (与内置合并模式资源拷贝布局一致)
        bool        manifestFound = false;
        std::string dummyName, dummyEntry;
        // 优先内嵌清单 (单文件分发, 无需外部 plugin.yaml)
        if (parseBuiltinManifest(
                btName,
                dummyName,
                dummyEntry,
                depends,
                optionalDepends,
                &resources,
                &interfaces
            )) {
            manifestFound = true;
        } else {
            // 跨平台探测: 优先 exe 目录 (安装布局) 其次 cwd (开发布局)
            std::vector<std::filesystem::path> bases;
            {
                auto exeDir = getExecutableDirPath();
                if (!exeDir.empty()) {
                    bases.push_back(exeDir);
                }
            }
            bases.push_back(std::filesystem::current_path());
            for (auto& base : bases) {
                auto probe = base / "plugins" / btName / "plugin.yaml";
                if (std::filesystem::exists(probe)) {
                    if (parsePluginManifest(
                            probe.parent_path(),
                            dummyName,
                            dummyEntry,
                            depends,
                            optionalDepends,
                            &resources,
                            &interfaces
                        )) {
                        manifestFound = true;
                        break;
                    }
                }
            }
            if (!manifestFound) {
                (void)resources;
                (void)interfaces;
            }
        }
        // 若内置注册表中不存在, 回退为普通目录插件加载 (非合并编译时
        // builtin:// 仍可指向外部目录插件, 保持兼容)
        if (!agentxx::plugin::findBuiltinPlugin(btName)) {
            // 按目录插件路径重新进入常规加载分支 (跨平台: exe 目录优先)
            std::filesystem::path fallback;
            {
                auto exeDir = getExecutableDirPath();
                if (!exeDir.empty()) {
                    auto cand = exeDir / "plugins" / btName;
                    if (std::filesystem::is_directory(cand)) {
                        fallback = cand;
                    }
                }
            }
            if (fallback.empty()) {
                auto cand = std::filesystem::current_path() / "plugins" / btName;
                if (std::filesystem::is_directory(cand)) {
                    fallback = cand;
                }
            }
            if (!fallback.empty()) {
                XX_LOGI(
                    "Builtin plugin `{}` not in registry, fallback to directory `{}`",
                    btName,
                    fallback.string()
                );
                co_return co_await loadPluginAsync(fallback.string(), cfg, allowClientOnlySkip);
            }
        }
        for (const auto& dep : depends) {
            if (!find(dep)) {
                XX_LOGE(
                    "Builtin plugin `{}` load failed: required dependency `{}` not installed",
                    btName,
                    dep
                );
                co_return nullptr;
            }
        }
        for (const auto& dep : optionalDepends) {
            if (!find(dep)) {
                XX_LOGW("Builtin plugin `{}` optional dependency `{}` not installed", btName, dep);
            }
        }
        co_return co_await loadBuiltinAsync(
            btName,
            path,
            depends,
            optionalDepends,
            cfg,
            resources,
            interfaces
        );
    }
    namespace fs = std::filesystem;
    fs::path p(path);
    if (fs::is_directory(p)) {
        auto manifestPath = p / "plugin.yaml";
        if (fs::exists(manifestPath)) {
            std::string              manifestName, manifestEntry;
            std::vector<std::string> depends, optionalDepends;
            PluginManifestResources  resources;
            PluginManifestInterfaces interfaces;
            if (!parsePluginManifest(
                    p,
                    manifestName,
                    manifestEntry,
                    depends,
                    optionalDepends,
                    &resources,
                    &interfaces
                )) {
                XX_LOGE("Parse manifest `{}` failed", manifestPath.string());
                co_return nullptr;
            }

            for (const auto& dep : depends) {
                if (!find(dep)) {
                    XX_LOGE(
                        "Plugin `{}` load failed: required dependency `{}` not installed (load it first)",
                        manifestName,
                        dep
                    );
                    co_return nullptr;
                }
            }

            for (const auto& dep : optionalDepends) {
                if (!find(dep)) {
                    XX_LOGW(
                        "Plugin `{}` optional dependency `{}` not installed",
                        manifestName,
                        dep
                    );
                }
            }

            if (agentxx::plugin::findBuiltinPlugin(manifestName) != nullptr) {
                co_return co_await loadBuiltinAsync(
                    manifestName,
                    manifestPath.string(),
                    depends,
                    optionalDepends,
                    cfg,
                    resources,
                    interfaces
                );
            }

            std::string binPath = resolvePluginEntryPath(p, manifestEntry);
            if (!fs::exists(binPath)) {
#if XX_IS_WIN_D
                binPath = (p / (manifestName + ".dll")).string();
                if (!fs::exists(binPath)) {
                    binPath = (p / ("lib" + manifestName + ".dll")).string();
                }
#elif defined(__APPLE__)
                binPath = (p / ("lib" + manifestName + ".dylib")).string();
                if (!fs::exists(binPath)) {
                    binPath = (p / (manifestName + ".dylib")).string();
                }
#else
                binPath = (p / ("lib" + manifestName + ".so")).string();
                if (!fs::exists(binPath)) {
                    binPath = (p / (manifestName + ".so")).string();
                }
#endif
            }

            auto inst = co_await loadNativeAsync(
                binPath,
                cfg,
                allowClientOnlySkip,
                resources,
                interfaces
            );
            if (inst) {
                inst->depends         = depends;
                inst->optionalDepends = optionalDepends;
            }
            co_return inst;
        }
    }

    co_return co_await loadNativeAsync(path, cfg, allowClientOnlySkip);
}

asio::awaitable<void>
    PluginManager::loadConfiguredPlugins(const std::vector<agentxx::agent::PluginConfig>& plugins) {
    namespace fs = std::filesystem;

    struct SortItem {
        std::string                         name;
        std::vector<std::string>            depends;
        const agentxx::agent::PluginConfig* cfg = nullptr;
        std::string                         path;
    };

    std::vector<SortItem> items;
    items.reserve(plugins.size());

    for (const auto& pc : plugins) {
        if (!pc.enabled) {
            continue;
        }
        if (pc.sides == agentxx::agent::PluginSide::Client) {
            continue;
        }
        SortItem it;
        it.path = pc.path;
        it.cfg  = &pc;
        // 内置简写: builtin://<name> 直接以 name 作为标识
        // 优先内嵌清单取 depends, 回退文件系统
        if (isBuiltinScheme(pc.path)) {
            it.name = parseBuiltinName(pc.path);
            std::string              dummyName, dummyEntry;
            std::vector<std::string> optionalDepends;
            PluginManifestResources  resources;
            PluginManifestInterfaces interfaces;
            if (parseBuiltinManifest(
                    it.name,
                    dummyName,
                    dummyEntry,
                    it.depends,
                    optionalDepends,
                    &resources,
                    &interfaces
                )) {
                // 内嵌清单命中, 依赖已填入 it.depends
            } else {
                // 跨平台探测: exe 目录优先, 回退 cwd
                std::vector<std::filesystem::path> bases;
                {
                    auto exeDir = getExecutableDirPath();
                    if (!exeDir.empty()) {
                        bases.push_back(exeDir);
                    }
                }
                bases.push_back(std::filesystem::current_path());
                for (auto& base : bases) {
                    auto probe = base / "plugins" / it.name / "plugin.yaml";
                    if (std::filesystem::exists(probe)) {
                        if (parsePluginManifest(
                                probe.parent_path(),
                                dummyName,
                                dummyEntry,
                                it.depends,
                                optionalDepends,
                                &resources,
                                &interfaces
                            )) {
                            break;
                        }
                    }
                }
            }
        } else {
            fs::path        p(pc.path);
            std::error_code ec;
            if (fs::is_directory(p, ec)) {
                std::string              manifestName, manifestEntry;
                std::vector<std::string> optionalDepends;
                PluginManifestResources  resources;
                PluginManifestInterfaces interfaces;
                if (parsePluginManifest(
                        p,
                        manifestName,
                        manifestEntry,
                        it.depends,
                        optionalDepends,
                        &resources,
                        &interfaces
                    )) {
                    it.name = manifestName;
                } else {
                    it.name = pluginNameFromPath(pc.path);
                }
            } else {
                it.name = pluginNameFromPath(pc.path);
            }
        }
        items.push_back(std::move(it));
    }

    auto ordered = topoSortPlugins(std::move(items));

    for (const auto& it : ordered) {
        if (it.name.empty()) {
            continue;
        }
        if (plugins_.count(it.name) > 0) {
            continue;
        }
        co_await loadPluginAsync(it.path, it.cfg, /*allowClientOnlySkip=*/true);
    }
}

} // namespace plugin
} // namespace agentxx
