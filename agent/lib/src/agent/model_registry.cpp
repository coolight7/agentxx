#include "agentxx/agent/model_registry.h"

#include "agentxx/protocol/anthropic_provider.h"
#include "agentxx/protocol/openai_provider.h"

namespace agentxx {
namespace agent {

void ModelProviderRegistry::registerModel(std::string_view name, const ModelConfig& config) {
    models_[std::string{name}] = config;
    providerCache_.erase(name);
    if (defaultName_.empty()) {
        defaultName_ = name;
    }
}

bool ModelProviderRegistry::setDefaultModel(std::string_view name) {
    if (false == models_.contains(name)) {
        return false;
    }
    defaultName_ = std::string{name};
    return true;
}

std::string ModelProviderRegistry::getDefaultModelName() const {
    return defaultName_;
}

std::string ModelProviderRegistry::resolveModelName(std::string_view name) const {
    if (false == name.empty() && models_.contains(name)) {
        return std::string{name};
    }
    return defaultName_;
}

ModelConfig ModelProviderRegistry::getModelConfig(std::string_view name) const {
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
    if (mc.type == "codex") {
        // Codex 使用 OpenAI Responses API: 复用 OpenAIProvider, 开启 responses 模式
        ModelConfig codex = mc;
        return agentxx::server::OpenAIProvider::create_shared(codex);
    }
    return agentxx::server::OpenAIProvider::create_shared(mc);
}

std::shared_ptr<neograph::Provider> ModelProviderRegistry::getProvider(std::string_view name) {
    std::string_view effective
        = (false == name.empty() && models_.contains(name)) ? name : defaultName_;
    auto cfgIt = models_.find(effective);
    if (cfgIt == models_.end()) {
        return nullptr;
    }
    auto cacheIt = providerCache_.find(effective);
    if (cacheIt != providerCache_.end()) {
        return cacheIt->second;
    }
    auto provider = createProvider(cfgIt->second);
    providerCache_.emplace(std::string{effective}, provider);
    return provider;
}

std::vector<std::string> ModelProviderRegistry::listModelNames() const {
    std::vector<std::string> names;
    names.reserve(models_.size());
    for (const auto& kv : models_) {
        names.push_back(kv.first);
    }
    return names;
}

bool ModelProviderRegistry::hasModel(std::string_view name) const {
    return models_.contains(name);
}

size_t ModelProviderRegistry::size() const {
    return models_.size();
}

} // namespace agent
} // namespace agentxx
