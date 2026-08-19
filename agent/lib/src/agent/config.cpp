#include "agentxx/agent/config.h"

#include <expected>
#include <filesystem>
#include <fmt/format.h>

namespace agentxx {
namespace agent {

const ModelConfig ModelConfig::defaultModelConfig{};

bool ModelConfig::isValid() const {
    return !baseUrl.empty() || apiKey != "EMPTY";
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
    if (enableSessionPersistence && dataDir.empty() && sessionPersistenceRoot.empty()) {
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
