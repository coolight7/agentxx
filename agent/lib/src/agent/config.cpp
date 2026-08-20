#include "agentxx/agent/config.h"

#include <expected>
#include <filesystem>
#include <fmt/format.h>

namespace agentxx {
namespace agent {

const ModelConfig ModelConfig::defaultModelConfig{};

bool ModelConfig::isValid() const {
    // - 指定了自定义 api（可能不需要验证 api key），或是指定了 apk key （baseUrl 取官方 api）
    // 都可以使用
    return !baseUrl.empty() || apiKey != "EMPTY";
}

bool ModelConfig::isOpenaiApi() const {
    return type == "openai";
}

bool ModelConfig::isOpenaiResponseApi() const {
    return type == "openai-responses";
}

const ModelConfig& AgentConfig::getSubagentModel() const {
    return subagentModel.has_value() ? subagentModel.value() : model;
}

std::expected<void, std::string> AgentConfig::validate() const {
    if (!model.isValid() && availableModels.empty()) {
        return std::unexpected{
            "AgentConfig: no valid model (model.baseUrl/apiKey empty and availableModels empty)"
        };
    }
    if (!availableModels.empty() && !currentModelName.empty()
        && !availableModels.contains(currentModelName)) {
        return std::unexpected{fmt::format(
            "AgentConfig: currentModelName '{}' not in availableModels",
            currentModelName
        )};
    }
    if (enableSessionStore && dataDir.empty() && sessionStoreDirectory.empty()) {
        // 仅警告, 不阻断: 会话将仅内存 (BaseAgent 构造时已处理)
    }
    // dataDir 相对路径规范化在 ConfigStatic 层, 此处仅校验非空时可解析
    if (!dataDir.empty()) {
        std::error_code ec;
        auto            p = std::filesystem::path(dataDir);
        if (p.empty()) {
            return std::unexpected{"AgentConfig: dataDir is empty path"};
        }
        (void)ec;
    }
    return {};
}

} // namespace agent
} // namespace agentxx
