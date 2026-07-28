#pragma once

#include "agentxx/agent/config.h"
#include "neograph/provider.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace agentxx {
namespace agent {

/// 模型 Provider 注册表 (共享)
/// - 管理多个命名模型配置及其 Provider 实例缓存
/// - 仅保存可用模型与默认模型; "当前选择" 由各 Session 独立记录
/// - 仅在 agent io_context 线程访问, 无需锁保护
/// - UI 线程通过 Wire 消息 (WireGetModel/WireSelectModel) 间接操作
class ModelProviderRegistry {
public:

    /// 注册一个命名模型配置; 同名覆盖
    /// - 首次注册时自动设为默认模型
    void registerModel(std::string_view name, const ModelConfig& config);

    /// 设置默认模型名; 模型不存在时返回 false
    bool setDefaultModel(std::string_view name);

    /// 默认模型名 (无注册模型时为空)
    std::string getDefaultModelName() const;

    /// 解析有效模型名: name 为空或不存在时回退到默认模型
    std::string resolveModelName(std::string_view name) const;

    /// 指定模型的配置; name 为空/不存在时取默认模型
    ModelConfig getModelConfig(std::string_view name) const;

    /// 根据 ModelConfig::type 创建对应 Provider
    static std::shared_ptr<neograph::Provider> createProvider(const ModelConfig& mc);

    /// 指定模型的 Provider, 按需创建并缓存; name 为空/不存在时取默认模型
    /// - 无可用模型时返回 nullptr
    std::shared_ptr<neograph::Provider> getProvider(std::string_view name);

    /// 所有已注册模型的显示名称
    std::vector<std::string> listModelNames() const;

    bool hasModel(std::string_view name) const;

    size_t size() const;

private:

    std::map<std::string, ModelConfig, std::less<>>                         models_;
    std::map<std::string, std::shared_ptr<neograph::Provider>, std::less<>> providerCache_;
    std::string                                                             defaultName_;
};

} // namespace agent
} // namespace agentxx
