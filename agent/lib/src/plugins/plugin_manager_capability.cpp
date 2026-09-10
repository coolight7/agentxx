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

static void
    setErrOut(PluginInstance* caller, AgentxxPluginString* error_out, const std::string& msg) {
    if (!error_out || error_out->data) {
        return;
    }
    const AgentxxPluginHost* host = caller ? caller->hostView() : nullptr;
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
    if (!acceptsRegistration(inst)) {
        XX_LOGW(
            "Plugin `{}` registerCapability rejected: instance is closing or disabled",
            inst->name
        );
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
    if (!acceptsRegistration(inst)) {
        XX_LOGW(
            "Plugin `{}` registerCapabilityEx rejected: instance is closing or disabled",
            inst->name
        );
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

AgentxxPluginOperatorHandle* PluginManager::callToolAsync(
    PluginInstance* caller, AgentxxPluginStringView name, AgentxxPluginStringView args_json,
    AgentxxPluginStringView thread_id, AgentxxPluginOperatorCallback cb, void* ud,
    AgentxxPluginString* error_out
) {
    // 入参在当前调用内复制；查询、登记和完整 start 都在所属 IO 线程执行。
    if (!isIoThread()) {
        auto self = shared_from_this();
        auto owner = caller ? caller->self.lock() : nullptr;
        /// 排队阶段也保护 caller；只有持有实例对象不能阻止 ctx/dlclose。
        auto admission = std::make_shared<PluginInstance::InflightGuard>(owner);
        if (!*admission) {
            hostMemorySetString(error_out, "plugin caller is closing");
            return nullptr;
        }
        const std::string toolName = svToStr(name), args = svToStr(args_json), sid = svToStr(thread_id);
        return ioCallSync<AgentxxPluginOperatorHandle*>(this, [self, owner, admission, toolName, args, sid, cb, ud, error_out] {
            return self->callToolAsync(owner.get(), toolName, args, sid, cb, ud, error_out);
        });
    }
    std::shared_ptr<OpCore> core;
    try {
        auto owner = caller ? caller->self.lock() : nullptr;
        if (!owner || !ioExecutor_) {
            throw std::runtime_error("call_tool_async: missing caller or IO executor");
        }
        const auto toolName = svToStr(name);
        auto tool = std::dynamic_pointer_cast<PluginTool>(registry_->find(toolName));
        auto provider = tool ? tool->instance() : nullptr;
        if (!provider || !provider->enabled || (provider->lifetime && !provider->lifetime->acceptsOperations())) {
            throw std::runtime_error("call_tool_async: plugin tool not available: " + toolName);
        }
        const auto spec = tool->spec();
        if (!spec.execute_start) {
            throw std::runtime_error("call_tool_async: tool has no start callback");
        }
        auto parsed = PluginStringView::empty(args_json) ? util::Json::object()
                                                       : util::Json::parse(PluginStringView::str(args_json));
        if (!parsed.is_object()) {
            throw std::runtime_error("call_tool_async: arguments must be a JSON object");
        }
        const auto session = svToStr(thread_id);
        core = OpCore::create(runtime(), provider, owner, toolName);
        const auto callId = fmt::format("plugin_call_{}", core->id());
        parsed["sessionId"] = session;
        parsed["tool_call_id"] = callId;
        OpDrive drive;
        drive.start = [spec, args = parsed.dump(), session, callId](const auto* notify, auto* error) -> void* {
            const auto a = PluginStringView::from(args), s = PluginStringView::from(session), id = PluginStringView::from(callId);
            return spec.execute_start(spec.user_data, &a, &s, &id, notify, error);
        };
        drive.cancel = [spec](void* op) {
            if (spec.execute_cancel) {
                spec.execute_cancel(spec.user_data, op);
            }
        };
        std::string error;
        if (!core->start(std::move(drive), error)) {
            setErrOut(caller, error_out, error);
            return nullptr; // 真正拒绝：不调用 cb。
        }
        core->setCallback(cb, ud);
        return core->handle(); // 同步 done 也返回受管句柄，callback 恒经 IO 发布。
    } catch (const std::exception& e) {
        if (core && !core->submitted()) {
            core->reject();
        }
        setErrOut(caller, error_out, e.what());
        return nullptr;
    }
}

AgentxxPluginOperatorHandle* PluginManager::invokeCapabilityAsync(
    PluginInstance* caller, AgentxxPluginStringView capability, AgentxxPluginStringView method,
    AgentxxPluginStringView args_json, AgentxxPluginOperatorCallback cb, void* ud,
    AgentxxPluginString* error_out
) {
    if (!isIoThread()) {
        auto self = shared_from_this();
        auto owner = caller ? caller->self.lock() : nullptr;
        /// 排队阶段也保护 caller；只有持有实例对象不能阻止 ctx/dlclose。
        auto admission = std::make_shared<PluginInstance::InflightGuard>(owner);
        if (!*admission) {
            hostMemorySetString(error_out, "plugin caller is closing");
            return nullptr;
        }
        const std::string cap = svToStr(capability), meth = svToStr(method), args = svToStr(args_json);
        return ioCallSync<AgentxxPluginOperatorHandle*>(this, [self, owner, admission, cap, meth, args, cb, ud, error_out] {
            return self->invokeCapabilityAsync(owner.get(), cap, meth, args, cb, ud, error_out);
        });
    }
    std::shared_ptr<OpCore> core;
    try {
        auto owner = caller ? caller->self.lock() : nullptr;
        if (!owner || !ioExecutor_) {
            throw std::runtime_error("invoke_capability_async: missing caller or IO executor");
        }
        const auto cap = svToStr(capability);
        const auto* entry = capabilities_->get(cap);
        auto provider = entry ? find(entry->provider) : nullptr;
        if (!entry || !entry->start || !provider || !provider->enabled
            || (provider->lifetime && !provider->lifetime->acceptsOperations())) {
            throw std::runtime_error("invoke_capability_async: capability not available: " + cap);
        }
        const auto binding = *entry;
        OpDrive drive;
        drive.start = [binding, owner, meth = svToStr(method), args = svToStr(args_json)](const auto* notify, auto* error) -> void* {
            const auto m = PluginStringView::from(meth), a = PluginStringView::from(args);
            return binding.start(binding.ctx, owner->hostView(), &m, &a, notify, error);
        };
        drive.cancel = [binding](void* op) {
            if (binding.cancel) {
                binding.cancel(binding.ctx, op);
            }
        };
        core = OpCore::create(runtime(), provider, owner, cap);
        std::string error;
        if (!core->start(std::move(drive), error)) {
            setErrOut(caller, error_out, error);
            return nullptr;
        }
        core->setCallback(cb, ud);
        return core->handle();
    } catch (const std::exception& e) {
        if (core && !core->submitted()) {
            core->reject();
        }
        setErrOut(caller, error_out, e.what());
        return nullptr;
    }
}

} // namespace plugin
} // namespace agentxx
