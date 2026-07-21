#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/protocol/openai_provider.h"
#include "neograph/provider.h"
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agentxx {
namespace agent {

/// 模型 Provider 注册表
/// - 管理多个命名模型配置, 支持运行时切换当前使用的模型
/// - 按需创建并缓存各模型对应的 Provider 实例
/// - 线程安全: UI 线程切换模型, graph 运行线程读取当前模型
class ModelProviderRegistry {
public:
  /// 注册一个命名模型配置; 同名覆盖
  /// - 首次注册时自动设为当前模型
  void registerModel(const std::string &name, const ModelConfig &config) {
    std::lock_guard<std::mutex> lock(mutex_);
    models_[name] = config;
    if (currentName_.empty()) {
      currentName_ = name;
    }
  }

  /// 切换当前使用的模型
  /// - 模型不存在时返回 false, 不改变当前选择
  bool setCurrentModel(const std::string &name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (false == models_.contains(name)) {
      return false;
    }
    currentName_ = name;
    return true;
  }

  /// 当前模型显示名称 (无注册模型时为空)
  std::string getCurrentModelName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentName_;
  }

  /// 当前模型配置; 无注册模型时返回默认 ModelConfig
  ModelConfig getCurrentModelConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = models_.find(currentName_);
    if (it == models_.end()) {
      return ModelConfig{};
    }
    return it->second;
  }

  /// 当前模型的 Provider, 按需创建并缓存
  /// - 无注册模型时返回 nullptr
  std::shared_ptr<neograph::Provider> getCurrentProvider() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto cfgIt = models_.find(currentName_);
    if (cfgIt == models_.end()) {
      return nullptr;
    }
    auto cacheIt = providerCache_.find(currentName_);
    if (cacheIt != providerCache_.end()) {
      return cacheIt->second;
    }
    auto provider = agentxx::server::OpenAIProvider::create_shared(
        toProviderConfig(cfgIt->second));
    providerCache_[currentName_] = provider;
    return provider;
  }

  /// 所有已注册模型的显示名称
  std::vector<std::string> listModelNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(models_.size());
    for (const auto &kv : models_) {
      names.push_back(kv.first);
    }
    return names;
  }

  bool hasModel(const std::string &name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return models_.contains(name);
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return models_.size();
  }

  /// ModelConfig -> OpenAIProvider::Config
  static agentxx::server::OpenAIProvider::Config
  toProviderConfig(const ModelConfig &mc) {
    agentxx::server::OpenAIProvider::Config cfg{
        .api_key = mc.apiKey,
        .base_url = mc.baseUrl,
        .default_model = mc.modelName,
    };
    if (mc.extra_config.is_object()) {
      cfg.extra_body = mc.extra_config;
    }
    return cfg;
  }

private:
  mutable std::mutex mutex_;
  std::map<std::string, ModelConfig> models_;
  std::map<std::string, std::shared_ptr<neograph::Provider>> providerCache_;
  std::string currentName_;
};

} // namespace agent
} // namespace agentxx
