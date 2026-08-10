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
    /// 统一数据根目录 (yaml `data_dir`, 支持 `~`/环境变量展开)
    /// - 为空表示不持久化: 设置/会话/codegraph 数据仅存内存 (BaseAgent 输出警告)
    /// - 特殊关键字 `default` (仅 tui/cli 模式): 使用当前系统数据目录
    ///   (Linux/macOS: ~/.agentxx/, Windows: %APPDATA%/agentxx/)
    /// - 非空时数据子路径: {dataDir}/sqlite/global.db (全局设置),
    ///   {dataDir}/sqlite/sessions/{threadId}/ (会话数据),
    ///   {dataDir}/sqlite/codegraph/... (CodeGraph 索引)
    std::string dataDir;
    /// 是否启用 CodeGraph 代码分析 (默认关闭)
    /// - 索引项目根目录固定为当前程序工作目录
    /// - 索引数据库: {dataDir}/sqlite/codegraph/<折叠路径>/index.db
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
