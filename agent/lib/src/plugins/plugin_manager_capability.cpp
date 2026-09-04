#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/plugin/op_driver.h"
#include "agentxx/util/container_util.h"
#include "agentxx/util/log.h"
#include "asio/as_tuple.hpp"
#include "asio/bind_cancellation_slot.hpp"
#include "asio/co_spawn.hpp"
#include "asio/deferred.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"

#include <algorithm>
#include <cstring>

namespace agentxx {
namespace plugin {

static std::atomic<size_t> g_pluginCallSeq{0};

static void
    setErrOut(PluginInstance* caller, AgentxxPluginString* error_out, const std::string& msg) {
    if (!error_out || error_out->data) {
        return;
    }
    const AgentxxPluginHost* host = caller ? &caller->host : nullptr;
    *error_out                    = agentxx::plugin::PluginString::from(host, strToSv(msg));
    if (!error_out->data) {
        auto* p = static_cast<char*>(hostMemoryAlloc(msg.size() + 1));
        if (p) {
            std::memcpy(p, msg.c_str(), msg.size() + 1);
            error_out->data = p;
            error_out->size = msg.size();
        }
    }
}

// =====================================================================
// CapabilityRegistry
// =====================================================================

bool CapabilityRegistry::registerCapability(
    std::string_view                     name,
    std::string_view                     provider,
    AgentxxPluginCapabilityStartFunction start,
    AgentxxPluginOperatorCancelFunction  cancel,
    void*                                ctx
) {
    if (name.empty()) {
        return false;
    }
    auto it = caps_.find(name);
    if (it != caps_.end()) {
        XX_LOGW(
            "CapabilityRegistry: capability `{}` already registered by `{}`",
            name,
            it->second.provider
        );
        return false;
    }
    util::insertHeterogeneous(
        caps_,
        std::string{name},
        Entry{std::string{provider}, start, cancel, ctx}
    );
    XX_LOGI("CapabilityRegistry: `{}` registered by plugin `{}`", name, provider);
    return true;
}

bool CapabilityRegistry::unregisterCapability(std::string_view name, std::string_view provider) {
    auto it = caps_.find(name);
    if (it == caps_.end()) {
        return false;
    }
    if (it->second.provider != provider) {
        XX_LOGW(
            "CapabilityRegistry: capability `{}` owned by `{}`, cannot unregister by `{}`",
            name,
            it->second.provider,
            provider
        );
        return false;
    }
    caps_.erase(it);
    return true;
}

bool CapabilityRegistry::has(std::string_view name) const {
    return caps_.contains(name);
}

const CapabilityRegistry::Entry* CapabilityRegistry::get(std::string_view name) const {
    auto it = caps_.find(name);
    if (it == caps_.end()) {
        return nullptr;
    }
    return &it->second;
}

std::string CapabilityRegistry::providerOf(std::string_view name) const {
    auto it = caps_.find(name);
    if (it == caps_.end()) {
        return {};
    }
    return it->second.provider;
}

std::vector<std::string> CapabilityRegistry::names() const {
    std::vector<std::string> out;
    out.reserve(caps_.size());
    for (const auto& [name, entry] : caps_) {
        (void)entry;
        out.push_back(name);
    }
    return out;
}

// =====================================================================
// Capability 注册与调用
// =====================================================================

int PluginManager::registerCapability(PluginInstance* inst, AgentxxPluginStringView capability) {
    if (!inst || agentxx::plugin::PluginStringView::empty(capability)) {
        return -1;
    }
    std::string capStr{capability.data, capability.size};
    if (!capabilities_->registerCapability(capStr, inst->name)) {
        return -1;
    }
    inst->capabilityRegistrations.erase(
        std::remove_if(
            inst->capabilityRegistrations.begin(),
            inst->capabilityRegistrations.end(),
            [&capStr](const PluginInstance::CapabilityRegistration& c) {
                return c.name == capStr;
            }
        ),
        inst->capabilityRegistrations.end()
    );
    inst->capabilityRegistrations.push_back(
        PluginInstance::CapabilityRegistration{capStr, nullptr, nullptr, nullptr}
    );
    return 0;
}

int PluginManager::unregisterCapability(PluginInstance* inst, AgentxxPluginStringView capability) {
    if (!inst || agentxx::plugin::PluginStringView::empty(capability)) {
        return -1;
    }
    std::string capStr{capability.data, capability.size};
    auto        it = std::find_if(
        inst->capabilityRegistrations.begin(),
        inst->capabilityRegistrations.end(),
        [&capStr](const PluginInstance::CapabilityRegistration& c) {
            return c.name == capStr;
        }
    );
    if (it == inst->capabilityRegistrations.end()) {
        return -1;
    }
    inst->capabilityRegistrations.erase(it);
    capabilities_->unregisterCapability(capStr, inst->name);
    return 0;
}

int PluginManager::hasCapability(AgentxxPluginStringView capability) const {
    if (agentxx::plugin::PluginStringView::empty(capability)) {
        return 0;
    }
    return capabilities_->has(std::string_view{capability.data, capability.size}) ? 1 : 0;
}

int PluginManager::registerCapabilityEx(
    PluginInstance*                      inst,
    AgentxxPluginStringView              capability,
    AgentxxPluginCapabilityStartFunction start,
    AgentxxPluginOperatorCancelFunction  cancel,
    void*                                ctx
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(capability) || !start) {
        return -1;
    }
    std::string capStr{capability.data, capability.size};
    if (!capabilities_->registerCapability(capStr, inst->name, start, cancel, ctx)) {
        return -1;
    }
    inst->capabilityRegistrations.erase(
        std::remove_if(
            inst->capabilityRegistrations.begin(),
            inst->capabilityRegistrations.end(),
            [&capStr](const PluginInstance::CapabilityRegistration& c) {
                return c.name == capStr;
            }
        ),
        inst->capabilityRegistrations.end()
    );
    inst->capabilityRegistrations.push_back(
        PluginInstance::CapabilityRegistration{capStr, start, cancel, ctx}
    );
    return 0;
}

static bool buildCapabilityDrive(
    PluginManager&          mgr,
    PluginInstance*         caller,
    AgentxxPluginStringView capability,
    AgentxxPluginStringView method,
    AgentxxPluginStringView args_json,
    std::string&            providerName,
    plugin::OpDrive&        drive,
    std::string&            err
) {
    std::string capName = svToStr(capability);
    std::string methStr = svToStr(method);
    std::string argStr  = svToStr(args_json);
    if (argStr.empty()) {
        argStr = "{}";
    }

    CapabilityRegistry::Entry entry;
    bool                      found = false;
    if (mgr.isIoThread()) {
        if (const auto* e = mgr.capabilities()->get(capName)) {
            entry = *e;
            found = true;
        }
    } else {
        found = ioCallSync<bool>(&mgr, [&mgr, &capName, &entry]() {
            const auto* e = mgr.capabilities()->get(capName);
            if (!e) {
                return false;
            }
            entry = *e;
            return true;
        });
    }
    if (!found) {
        err = fmt::format("invoke_capability: capability `{}` not registered", capName);
        return false;
    }
    if (!entry.start) {
        err = fmt::format("invoke_capability: capability `{}` has no method handler", capName);
        return false;
    }
    providerName    = entry.provider;
    auto weakCaller = caller ? caller->self : std::weak_ptr<PluginInstance>{};
    drive.start     = [entry, methStr, argStr, weakCaller](
                      const AgentxxPluginOperatorNotify* notify,
                      AgentxxPluginString*               e
                  ) -> void* {
        const AgentxxPluginHost* callerHost = nullptr;
        if (auto c = weakCaller.lock()) {
            callerHost = &c->host;
        }
        auto methSv = agentxx::plugin::PluginStringView::from(methStr.data(), methStr.size());
        auto argSv  = agentxx::plugin::PluginStringView::from(argStr.data(), argStr.size());
        return entry.start(entry.ctx, callerHost, &methSv, &argSv, notify, e);
    };
    drive.cancel = [entry](void* op) {
        if (entry.cancel) {
            entry.cancel(entry.ctx, op);
        }
    };
    return true;
}

AgentxxPluginOperatorHandle* PluginManager::callToolAsync(
    PluginInstance*               caller,
    AgentxxPluginStringView       name,
    AgentxxPluginStringView       args_json,
    AgentxxPluginStringView       thread_id,
    AgentxxPluginOperatorCallback cb,
    void*                         ud,
    AgentxxPluginString*          error_out
) {
    auto setErr = [&](const std::string& msg) {
        setErrOut(caller, error_out, msg);
    };
    if (!ioExecutor_) {
        setErr("call_tool_async: io executor not ready");
        return nullptr;
    }

    std::string                                 toolName = svToStr(name);
    std::shared_ptr<agentxx::tools::XXToolBase> tool;
    bool                                        found = false;
    if (isIoThread()) {
        tool  = registry_->find(toolName);
        found = tool != nullptr;
    } else {
        found = ioCallSync<bool>(this, [this, &toolName, &tool]() {
            tool = registry_->find(toolName);
            return tool != nullptr;
        });
    }
    if (!found) {
        setErr(fmt::format("plugin call_tool: tool `{}` not found", toolName));
        return nullptr;
    }
    auto pluginTool = std::dynamic_pointer_cast<PluginTool>(tool);
    if (!pluginTool) {
        setErr(fmt::format("plugin call_tool: tool `{}` is not a plugin tool", toolName));
        return nullptr;
    }
    auto targetInst = pluginTool->instance();
    if (!targetInst || !targetInst->enabled) {
        setErr(fmt::format("plugin call_tool: tool `{}` plugin disabled/released", toolName));
        return nullptr;
    }

    const auto&    spec   = pluginTool->spec();
    neograph::json parsed = neograph::json::object();
    if (!agentxx::plugin::PluginStringView::empty(args_json)) {
        try {
            auto j = neograph::json::parse(std::string_view{args_json.data, args_json.size});
            if (j.is_object()) {
                parsed = std::move(j);
            }
        } catch (const std::exception& e) {
            setErr(fmt::format("plugin call_tool: invalid args_json: {}", e.what()));
            return nullptr;
        }
    }
    std::string sessionId  = svToStr(thread_id);
    parsed["sessionId"]    = sessionId;
    parsed["tool_call_id"] = fmt::format("plugin_call_{}", ++g_pluginCallSeq);
    auto argsStr           = parsed.dump();

    auto handle    = std::make_shared<AgentxxPluginOperatorHandle>();
    handle->caller = caller;
    if (caller) {
        caller->outstandingOps.push_back(handle);
    }

    auto guard = std::make_shared<PluginInstance::InflightGuard>(targetInst.get());
    auto core  = std::make_shared<OpCore>(ioExecutor_, guard);
    core->cb   = cb;
    core->cbUd = ud;

    plugin::OpDrive drive;
    drive.start =
        [spec,
         argsStr,
         sessionId](const AgentxxPluginOperatorNotify* notify, AgentxxPluginString* err) -> void* {
        auto argsSv = agentxx::plugin::PluginStringView::from(argsStr.data(), argsStr.size());
        auto sidSv  = agentxx::plugin::PluginStringView::from(sessionId.data(), sessionId.size());
        auto tcidSv = agentxx::plugin::PluginStringView::from("", 0);
        return spec.execute_start(spec.user_data, &argsSv, &sidSv, &tcidSv, notify, err);
    };
    drive.cancel = [spec](void* op) {
        if (spec.execute_cancel) {
            spec.execute_cancel(spec.user_data, op);
        }
    };

    AgentxxPluginString startErr{nullptr, 0};
    void*               op  = nullptr;
    auto                ntf = core->notify();
    try {
        op = drive.start(&ntf, &startErr);
    } catch (...) {
        startErr = agentxx::plugin::PluginString::fromCstr(
            caller ? &caller->host : nullptr,
            "start threw"
        );
    }

    if (startErr.data || (!op && !core->notified.load(std::memory_order_acquire))) {
        guard.reset();
        std::string errMsg
            = startErr.data ? std::string(startErr.data, startErr.size) : "protocol violation";
        if (startErr.data && caller) {
            agentxx::plugin::PluginString::free(&caller->host, &startErr);
        }
        setErr(errMsg);
        if (cb) {
            cb(ud,
               AGENTXX_PLUGIN_OPERATOR_FAILED,
               agentxx::plugin::PluginStringView::from(errMsg.data(), errMsg.size()));
        }
        return nullptr;
    }

    handle->cancelFn = [core, drive, op]() {
        if (core->notified.load(std::memory_order_acquire)) {
            return;
        }
        core->cancelRequested.store(true, std::memory_order_release);
        detail::safeCancelOnce(*core, drive, op);
    };

    if (handle->cancelled.load(std::memory_order_acquire)) {
        handle->cancelFn();
    }

    if (!core->notified.load(std::memory_order_acquire)) {
        asio::co_spawn(
            ioExecutor_,
            [core, drive, op]() -> asio::awaitable<void> {
                while (!core->notified.load(std::memory_order_acquire)) {
                    auto [ec]
                        = co_await core->chan.async_receive(asio::as_tuple(asio::use_awaitable));
                    (void)ec;
                }
            },
            asio::detached
        );
    }
    // 自动回收 outstandingOps：操作终态后从调用方列表移除，避免悬垂 handle 在后续 unload 时触发 UAF
    // 零轮询：等待 doneSignal 事件（避免与上方的 sentinel 协程竞争同一 chan 消息）
    detail::spawnHandleReaper(
        ioExecutor_,
        core,
        caller ? caller->self : std::weak_ptr<PluginInstance>{},
        handle
    );

    return handle.get();
}

AgentxxPluginOperatorHandle* PluginManager::invokeCapabilityAsync(
    PluginInstance*               caller,
    AgentxxPluginStringView       capability,
    AgentxxPluginStringView       method,
    AgentxxPluginStringView       args_json,
    AgentxxPluginOperatorCallback cb,
    void*                         ud,
    AgentxxPluginString*          error_out
) {
    auto setErr = [&](const std::string& msg) {
        setErrOut(caller, error_out, msg);
    };
    if (!ioExecutor_) {
        setErr("invoke_capability_async: io executor not ready");
        return nullptr;
    }

    plugin::OpDrive drive;
    std::string     provider;
    std::string     err;
    if (!buildCapabilityDrive(*this, caller, capability, method, args_json, provider, drive, err)) {
        setErr(err);
        return nullptr;
    }

    auto providerInst = find(provider);
    auto handle       = std::make_shared<AgentxxPluginOperatorHandle>();
    handle->caller    = caller;
    if (caller) {
        caller->outstandingOps.push_back(handle);
    }

    auto guard = providerInst ? std::make_shared<PluginInstance::InflightGuard>(providerInst.get())
                              : nullptr;
    auto core  = std::make_shared<OpCore>(ioExecutor_, guard);
    core->cb   = cb;
    core->cbUd = ud;

    AgentxxPluginString startErr{nullptr, 0};
    void*               op  = nullptr;
    auto                ntf = core->notify();
    try {
        op = drive.start(&ntf, &startErr);
    } catch (...) {
        startErr = agentxx::plugin::PluginString::fromCstr(
            caller ? &caller->host : nullptr,
            "start threw"
        );
    }

    if (startErr.data || (!op && !core->notified.load(std::memory_order_acquire))) {
        guard.reset();
        std::string errMsg
            = startErr.data ? std::string(startErr.data, startErr.size) : "protocol violation";
        if (startErr.data && caller) {
            agentxx::plugin::PluginString::free(&caller->host, &startErr);
        }
        setErr(errMsg);
        if (cb) {
            cb(ud,
               AGENTXX_PLUGIN_OPERATOR_FAILED,
               agentxx::plugin::PluginStringView::from(errMsg.data(), errMsg.size()));
        }
        return nullptr;
    }

    handle->cancelFn = [core, drive, op]() {
        if (core->notified.load(std::memory_order_acquire)) {
            return;
        }
        core->cancelRequested.store(true, std::memory_order_release);
        detail::safeCancelOnce(*core, drive, op);
    };

    if (handle->cancelled.load(std::memory_order_acquire)) {
        handle->cancelFn();
    }

    if (!core->notified.load(std::memory_order_acquire)) {
        asio::co_spawn(
            ioExecutor_,
            [core, drive, op]() -> asio::awaitable<void> {
                while (!core->notified.load(std::memory_order_acquire)) {
                    auto [ec]
                        = co_await core->chan.async_receive(asio::as_tuple(asio::use_awaitable));
                    (void)ec;
                }
            },
            asio::detached
        );
    }
    // 自动回收 outstandingOps：操作终态后从调用方列表移除，避免悬垂 handle 在后续 unload 时触发 UAF
    // 零轮询：等待 doneSignal 事件（避免与上方的 sentinel 协程竞争同一 chan 消息）
    detail::spawnHandleReaper(
        ioExecutor_,
        core,
        caller ? caller->self : std::weak_ptr<PluginInstance>{},
        handle
    );

    return handle.get();
}

} // namespace plugin
} // namespace agentxx
