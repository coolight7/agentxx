#pragma once

#include "agentxx/agent/config.h"
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
    void registerModel(const std::string& name, const ModelConfig& config);

    /// 设置默认模型名; 模型不存在时返回 false
    bool setDefaultModel(const std::string& name);

    /// 默认模型名 (无注册模型时为空)
    std::string getDefaultModelName() const;

    /// 解析有效模型名: name 为空或不存在时回退到默认模型
    std::string resolveModelName(const std::string& name) const;

    /// 指定模型的配置; name 为空/不存在时取默认模型
    ModelConfig getModelConfig(const std::string& name) const;

    /// 根据 ModelConfig::type 创建对应 Provider
    static std::shared_ptr<neograph::Provider> createProvider(const ModelConfig& mc);

    /// 指定模型的 Provider, 按需创建并缓存; name 为空/不存在时取默认模型
    /// - 无可用模型时返回 nullptr
    std::shared_ptr<neograph::Provider> getProvider(const std::string& name);

    /// 所有已注册模型的显示名称
    std::vector<std::string> listModelNames() const;

    bool hasModel(const std::string& name) const;

    size_t size() const;

private:

    mutable std::mutex                                         mutex_;
    std::map<std::string, ModelConfig>                         models_;
    std::map<std::string, std::shared_ptr<neograph::Provider>> providerCache_;
    std::string                                                defaultName_;
};

} // namespace agent
} // namespace agentxx
