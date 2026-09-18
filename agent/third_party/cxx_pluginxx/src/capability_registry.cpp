#include "pluginxx/host/capability_registry.h"

namespace pluginxx {

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
    utilxx_base::insertHeterogeneous(
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

} // namespace pluginxx
