#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/plugin/op_driver.h"
#include "agentxx/util/container_util.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"

#include <algorithm>
#include <cstring>

namespace agentxx {
namespace plugin {

static std::atomic<size_t> g_pluginCallSeq{0};

static void setErrOut(PluginInstance* caller, char** error_out, const std::string& msg) {
    if (!error_out || *error_out) {
        return;
    }
    if (caller && caller->host.vtable && caller->host.vtable->strdup) {
        *error_out = caller->host.vtable->strdup(msg.c_str());
        return;
    }
    auto* p = static_cast<char*>(::malloc(msg.size() + 1));
    if (p) {
        std::memcpy(p, msg.c_str(), msg.size() + 1);
    }
    *error_out = p;
}

// =====================================================================
// CapabilityRegistry
// =====================================================================

bool CapabilityRegistry::registerCapability(
    std::string_view  name,
    std::string_view  provider,
    AgentxxCapStartFn start,
    AgentxxOpCancelFn cancel,
    void*             ctx
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

int PluginManager::registerCapability(PluginInstance* inst, const char* capability) {
    if (!inst || !capability || !*capability) {
        return -1;
    }
    if (!capabilities_->registerCapability(capability, inst->name)) {
        return -1;
    }
    inst->capabilityRegistrations.erase(
        std::remove_if(
            inst->capabilityRegistrations.begin(),
            inst->capabilityRegistrations.end(),
            [capability](const PluginInstance::CapabilityRegistration& c) {
                return c.name == capability;
            }
        ),
        inst->capabilityRegistrations.end()
    );
    inst->capabilityRegistrations.push_back(
        PluginInstance::CapabilityRegistration{capability, nullptr, nullptr, nullptr}
    );
    return 0;
}

int PluginManager::unregisterCapability(PluginInstance* inst, const char* capability) {
    if (!inst || !capability) {
        return -1;
    }
    auto it = std::find_if(
        inst->capabilityRegistrations.begin(),
        inst->capabilityRegistrations.end(),
        [capability](const PluginInstance::CapabilityRegistration& c) {
            return c.name == capability;
        }
    );
    if (it == inst->capabilityRegistrations.end()) {
        return -1;
    }
    inst->capabilityRegistrations.erase(it);
    capabilities_->unregisterCapability(capability, inst->name);
    return 0;
}

int PluginManager::hasCapability(const char* capability) const {
    return capability && capabilities_->has(capability) ? 1 : 0;
}

int PluginManager::registerCapabilityEx(
    PluginInstance*   inst,
    const char*       capability,
    AgentxxCapStartFn start,
    AgentxxOpCancelFn cancel,
    void*             ctx
) {
    if (!inst || !capability || !*capability || !start) {
        return -1;
    }
    if (!capabilities_->registerCapability(capability, inst->name, start, cancel, ctx)) {
        return -1;
    }
    inst->capabilityRegistrations.erase(
        std::remove_if(
            inst->capabilityRegistrations.begin(),
            inst->capabilityRegistrations.end(),
            [capability](const PluginInstance::CapabilityRegistration& c) {
                return c.name == capability;
            }
        ),
        inst->capabilityRegistrations.end()
    );
    inst->capabilityRegistrations.push_back(
        PluginInstance::CapabilityRegistration{capability, start, cancel, ctx}
    );
    return 0;
}

static bool buildCapabilityDrive(
    PluginManager&          mgr,
    PluginInstance*         caller,
    const char*             capability,
    const char*             method,
    const char*             args_json,
    std::string&            providerName,
    plugin::OpDrive&        drive,
    std::string&            err
) {
    CapabilityRegistry::Entry entry;
    bool                      found = false;
    if (mgr.isIoThread()) {
        if (const auto* e = mgr.capabilities()->get(capability)) {
            entry = *e;
            found = true;
        }
    } else {
        found = ioCallSync<bool>(&mgr, [&mgr, capability, &entry]() {
            const auto* e = mgr.capabilities()->get(capability);
            if (!e) {
                return false;
            }
            entry = *e;
            return true;
        });
    }
    if (!found) {
        err = fmt::format("invoke_capability: capability `{}` not registered", capability);
        return false;
    }
    if (!entry.start) {
        err = fmt::format("invoke_capability: capability `{}` has no method handler", capability);
        return false;
    }
    providerName   = entry.provider;
    auto capStr    = std::string{method};
    auto argStr    = (args_json && *args_json) ? std::string{args_json} : std::string{"{}"};
    auto weakCaller = caller ? caller->self : std::weak_ptr<PluginInstance>{};
    drive.start
        = [entry, capStr, argStr, weakCaller](const AgentxxOpNotify* notify, char** e) -> void* {
        const AgentxxHost* callerHost = nullptr;
        if (auto c = weakCaller.lock()) {
            callerHost = &c->host;
        }
        return entry.start(
            entry.ctx,
            callerHost,
            agentxx_plugin_sv_cstr(capStr.c_str()),
            agentxx_plugin_sv_cstr(argStr.c_str()),
            notify,
            e
        );
    };
    drive.cancel = [entry](void* op) {
        if (entry.cancel) {
            entry.cancel(entry.ctx, op);
        }
    };
    return true;
}

AgentxxOpHandle* PluginManager::callToolAsync(
    PluginInstance* caller,
    const char*     name,
    const char*     args_json,
    const char*     thread_id,
    AgentxxOpCb     cb,
    void*           ud,
    char**          error_out
) {
    auto setErr = [&](const std::string& msg) {
        setErrOut(caller, error_out, msg);
    };
    if (!ioExecutor_) {
        setErr("call_tool_async: io executor not ready");
        return nullptr;
    }

    std::string toolName = name ? name : "";
    std::shared_ptr<agentxx::tools::XXToolBase> tool;
    bool found = false;
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

    const auto& spec   = pluginTool->spec();
    neograph::json parsed = neograph::json::object();
    if (args_json && *args_json) {
        try {
            auto j = neograph::json::parse(args_json);
            if (j.is_object()) {
                parsed = std::move(j);
            }
        } catch (const std::exception& e) {
            setErr(fmt::format("plugin call_tool: invalid args_json: {}", e.what()));
            return nullptr;
        }
    }
    parsed["sessionId"]    = thread_id ? thread_id : "";
    parsed["tool_call_id"] = fmt::format("plugin_call_{}", ++g_pluginCallSeq);
    auto argsStr           = parsed.dump();
    std::string sessionId  = thread_id ? thread_id : "";

    auto handle    = std::make_shared<AgentxxOpHandle>();
    handle->caller = caller;
    if (caller) {
        caller->outstandingOps.push_back(handle);
    }

    auto guard = std::make_shared<PluginInstance::InflightGuard>(targetInst.get());
    auto core  = std::make_shared<OpCore>(ioExecutor_, guard);
    core->cb   = cb;
    core->cbUd = ud;

    plugin::OpDrive drive;
    drive.start = [spec, argsStr, sessionId](const AgentxxOpNotify* notify, char** err) -> void* {
        return spec.execute_start(
            spec.user_data,
            agentxx_plugin_sv(argsStr.data(), argsStr.size()),
            agentxx_plugin_sv(sessionId.data(), sessionId.size()),
            agentxx_plugin_sv("", 0),
            notify,
            err
        );
    };
    drive.cancel = [spec](void* op) {
        if (spec.execute_cancel) {
            spec.execute_cancel(spec.user_data, op);
        }
    };

    char* startErr = nullptr;
    void* op       = nullptr;
    auto  ntf      = core->notify();
    try {
        op = drive.start(&ntf, &startErr);
    } catch (...) {
        startErr = ::strdup("start threw");
    }

    if (startErr || (!op && !core->notified.load(std::memory_order_acquire))) {
        guard.reset();
        char* errMsg = startErr ? startErr : ::strdup("protocol violation");
        setErr(errMsg);
        if (cb) {
            cb(ud, AGENTXX_OP_FAILED, errMsg);
        } else if (startErr) {
            ::free(startErr);
        }
        return nullptr;
    }

    handle->cancelFn = [core, drive, op]() {
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
                    auto [ec] = co_await core->chan.async_receive(asio::as_tuple(asio::use_awaitable));
                    (void)ec;
                }
            },
            asio::detached
        );
    }

    return handle.get();
}

AgentxxOpHandle* PluginManager::invokeCapabilityAsync(
    PluginInstance* caller,
    const char*     capability,
    const char*     method,
    const char*     args_json,
    AgentxxOpCb     cb,
    void*           ud,
    char**          error_out
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
    auto handle       = std::make_shared<AgentxxOpHandle>();
    handle->caller    = caller;
    if (caller) {
        caller->outstandingOps.push_back(handle);
    }

    auto guard = providerInst ? std::make_shared<PluginInstance::InflightGuard>(providerInst.get()) : nullptr;
    auto core  = std::make_shared<OpCore>(ioExecutor_, guard);
    core->cb   = cb;
    core->cbUd = ud;

    char* startErr = nullptr;
    void* op       = nullptr;
    auto  ntf      = core->notify();
    try {
        op = drive.start(&ntf, &startErr);
    } catch (...) {
        startErr = ::strdup("start threw");
    }

    if (startErr || (!op && !core->notified.load(std::memory_order_acquire))) {
        guard.reset();
        char* errMsg = startErr ? startErr : ::strdup("protocol violation");
        setErr(errMsg);
        if (cb) {
            cb(ud, AGENTXX_OP_FAILED, errMsg);
        } else if (startErr) {
            ::free(startErr);
        }
        return nullptr;
    }

    handle->cancelFn = [core, drive, op]() {
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
                    auto [ec] = co_await core->chan.async_receive(asio::as_tuple(asio::use_awaitable));
                    (void)ec;
                }
            },
            asio::detached
        );
    }

    return handle.get();
}

} // namespace plugin
} // namespace agentxx
