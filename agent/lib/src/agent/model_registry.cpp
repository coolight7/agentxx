#include "agentxx/agent/model_registry.h"

#include "agentxx/protocol/anthropic_provider.h"
#include "agentxx/protocol/openai_provider.h"

namespace agentxx {
namespace agent {

void ModelProviderRegistry::registerModel(const std::string& name, const ModelConfig& config) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    models_[name] = config;
    // 同名覆盖注册时使旧 provider 缓存失效, 否则 getProvider 返回基于旧配置的 provider
    providerCache_.erase(name);
    if (defaultName_.empty()) {
        defaultName_ = name;
    }
}

bool ModelProviderRegistry::setDefaultModel(const std::string& name) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (false == models_.contains(name)) {
        return false;
    }
    defaultName_ = name;
    return true;
}

std::string ModelProviderRegistry::getDefaultModelName() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return defaultName_;
}

std::string ModelProviderRegistry::resolveModelName(const std::string& name) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (false == name.empty() && models_.contains(name)) {
        return name;
    }
    return defaultName_;
}

ModelConfig ModelProviderRegistry::getModelConfig(const std::string& name) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto effective = (false == name.empty() && models_.contains(name)) ? name : defaultName_;
    auto it        = models_.find(effective);
    if (it == models_.end()) {
        return ModelConfig{};
    }
    return it->second;
}

std::shared_ptr<neograph::Provider> ModelProviderRegistry::createProvider(const ModelConfig& mc) {
    if (mc.type == "anthropic") {
        return agentxx::server::AnthropicProvider::create_shared(mc);
    }
    return agentxx::server::OpenAIProvider::create_shared(mc);
}

std::shared_ptr<neograph::Provider> ModelProviderRegistry::getProvider(const std::string& name) {
    std::string effective;
    ModelConfig cfg;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        effective  = (false == name.empty() && models_.contains(name)) ? name : defaultName_;
        auto cfgIt = models_.find(effective);
        if (cfgIt == models_.end()) {
            return nullptr;
        }
        auto cacheIt = providerCache_.find(effective);
        if (cacheIt != providerCache_.end()) {
            return cacheIt->second;
        }
        // 复制配置, 避免解锁后再使用迭代器 (可能被并发 registerModel 失效)
        cfg = cfgIt->second;
    }
    auto provider = createProvider(cfg);
    {
        std::unique_lock<std::shared_mutex> ulock(mutex_);
        // 双重检查: 另一线程可能已创建并缓存, 复用以避免重复构造
        auto cacheIt = providerCache_.find(effective);
        if (cacheIt != providerCache_.end()) {
            return cacheIt->second;
        }
        providerCache_[effective] = provider;
    }
    return provider;
}

std::vector<std::string> ModelProviderRegistry::listModelNames() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::string>            names;
    names.reserve(models_.size());
    for (const auto& kv : models_) {
        names.push_back(kv.first);
    }
    return names;
}

bool ModelProviderRegistry::hasModel(const std::string& name) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return models_.contains(name);
}

size_t ModelProviderRegistry::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return models_.size();
}

} // namespace agent
} // namespace agentxx
