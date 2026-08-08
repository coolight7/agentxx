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
    std::vector<std::string>                  skillDirPaths;
    std::vector<std::string>                  memoryFilePaths;
    std::string                               useModelDefault;
    std::string                               useModelSubagent;
    std::string                               useModelWebSearch;
    std::string                               useModelAcp;
    std::string                               useModelTrain;
    std::string                               useModelTrainScorer;
    std::string                               useModelTrainOptimizer;
    /// 是否启用 CodeGraph 代码分析 (默认关闭)
    /// - 索引项目根目录固定为当前程序工作目录
    /// - 索引数据库: ~/.agentxx/sqlite/codegraph/<折叠路径>/index.db
    ///   (深层路径折叠 + 单段截断控制长度, 子目录可前缀复用最近父级索引)
    bool enableCodeGraph = true;
};

std::map<std::string, std::string> loadDotEnv(std::string_view path);
std::map<std::string, std::string> loadDotEnv(const std::vector<std::string>& paths);
std::map<std::string, std::string> loadOverrideEnv(std::string_view path);

std::string resolveEnvVars(
    std::string_view                          input,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
);

YamlAppConfig loadYamlConfig(
    std::string_view                          path,
    const std::map<std::string, std::string>& dotEnvVars,
    const std::map<std::string, std::string>& overrideEnvVars
);

agent::ModelConfig resolveModelConfig(
    const std::map<std::string, agent::ModelConfig>& models,
    std::string_view                                 modelName
);

void applyModelToConfig(
    std::shared_ptr<agent::AgentConfig>              agentConfig,
    const std::map<std::string, agent::ModelConfig>& models,
    std::string_view                                 modelName
);

void applySubagentModelToConfig(
    std::shared_ptr<agent::AgentConfig>              agentConfig,
    const std::map<std::string, agent::ModelConfig>& models,
    std::string_view                                 modelName
);

void applyWebSearchModelToConfig(
    std::shared_ptr<agent::AgentConfig>              agentConfig,
    const std::map<std::string, agent::ModelConfig>& models,
    std::string_view                                 modelName
);

void applyAvailableModelsToConfig(
    std::shared_ptr<agent::AgentConfig>              agentConfig,
    const std::map<std::string, agent::ModelConfig>& models,
    std::string_view                                 currentModelName
);

} // namespace client
} // namespace agentxx
