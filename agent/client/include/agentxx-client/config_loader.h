#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/util/log.h"
#include "neograph/json.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace agentxx {
namespace client {

struct YamlAppConfig {
    std::map<std::string, agent::ModelConfig> models;
    std::map<std::string, std::string>        mcpServers;
    std::string                               useModelDefault;
    std::string                               useModelSubagent;
    std::string                               useModelWebSearch;
    std::string                               useModelAcp;
    std::string                               useModelTrain;
    std::string                               useModelTrainScorer;
    std::string                               useModelTrainOptimizer;
};

std::map<std::string, std::string> loadDotEnv(const std::string& path);
std::map<std::string, std::string> loadDotEnv(const std::vector<std::string>& paths);
std::map<std::string, std::string> loadOverrideEnv(const std::string& path);

std::string resolveEnvVars(
    const std::string&                        input,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
);

YamlAppConfig loadYamlConfig(
    const std::string&                        path,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
);

agent::ModelConfig resolveModelConfig(
    const std::map<std::string, agent::ModelConfig>& models,
    const std::string&                               modelName
);

void applyModelToConfig(
    std::shared_ptr<agent::AgentConfig>              agentConfig,
    const std::map<std::string, agent::ModelConfig>& models,
    const std::string&                               modelName
);

void applySubagentModelToConfig(
    std::shared_ptr<agent::AgentConfig>              agentConfig,
    const std::map<std::string, agent::ModelConfig>& models,
    const std::string&                               modelName
);

void applyWebSearchModelToConfig(
    std::shared_ptr<agent::AgentConfig>              agentConfig,
    const std::map<std::string, agent::ModelConfig>& models,
    const std::string&                               modelName
);

void applyAvailableModelsToConfig(
    std::shared_ptr<agent::AgentConfig>              agentConfig,
    const std::map<std::string, agent::ModelConfig>& models,
    const std::string&                               currentModelName
);

} // namespace client
} // namespace agentxx
