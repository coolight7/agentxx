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

/// 模型 Provider 注册表 (共享)
/// - 管理多个命名模型配置及其 Provider 实例缓存
/// - 仅保存可用模型与默认模型; "当前选择" 由各 Session 独立记录
/// - 线程安全: UI 线程读取/切换, graph 运行线程按名解析 Provider
class ModelProviderRegistry {
public:
  /// 注册一个命名模型配置; 同名覆盖
  /// - 首次注册时自动设为默认模型
  void registerModel(const std::string &name, const ModelConfig &config) {
    std::lock_guard<std::mutex> lock(mutex_);
    models_[name] = config;
    if (defaultName_.empty()) {
      defaultName_ = name;
    }
  }

  /// 设置默认模型名; 模型不存在时返回 false
  bool setDefaultModel(const std::string &name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (false == models_.contains(name)) {
      return false;
    }
    defaultName_ = name;
    return true;
  }

  /// 默认模型名 (无注册模型时为空)
  std::string getDefaultModelName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return defaultName_;
  }

  /// 解析有效模型名: name 为空或不存在时回退到默认模型
  std::string resolveModelName(const std::string &name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (false == name.empty() && models_.contains(name)) {
      return name;
    }
    return defaultName_;
  }

  /// 指定模型的配置; name 为空/不存在时取默认模型
  ModelConfig getModelConfig(const std::string &name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto effective = (false == name.empty() && models_.contains(name))
                         ? name
                         : defaultName_;
    auto it = models_.find(effective);
    if (it == models_.end()) {
      return ModelConfig{};
    }
    return it->second;
  }

  /// 指定模型的 Provider, 按需创建并缓存; name 为空/不存在时取默认模型
  /// - 无可用模型时返回 nullptr
  std::shared_ptr<neograph::Provider> getProvider(const std::string &name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto effective = (false == name.empty() && models_.contains(name))
                         ? name
                         : defaultName_;
    auto cfgIt = models_.find(effective);
    if (cfgIt == models_.end()) {
      return nullptr;
    }
    auto cacheIt = providerCache_.find(effective);
    if (cacheIt != providerCache_.end()) {
      return cacheIt->second;
    }
    auto provider = agentxx::server::OpenAIProvider::create_shared(
        cfgIt->second);
    providerCache_[effective] = provider;
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

private:
  mutable std::mutex mutex_;
  std::map<std::string, ModelConfig> models_;
  std::map<std::string, std::shared_ptr<neograph::Provider>> providerCache_;
  std::string defaultName_;
};

} // namespace agent
} // namespace agentxx
