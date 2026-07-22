#pragma once
#include "agentxx/agent/training.h"
#include <memory>
#include <string>
#include <vector>

// ======================== 训练模式 ========================

/// 递归加载目录中所有 JSON 测试用例（含子目录）
std::vector<agentxx::agent::TrainingTestCase>
loadTestCasesRecursive(const std::string &dirPath);

/// 获取项目根目录（agentxx 源码根目录）
std::string findProjectRoot();

/// 替换输入中的 {agentxx_root} 占位符
void replacePlaceholders(std::vector<agentxx::agent::TrainingTestCase> &cases,
                         const std::string &projectRoot);

void runTrainingMode(
    std::shared_ptr<agentxx::agent::AgentConfig> baseConfig,
    std::shared_ptr<agentxx::agent::AgentConfig> scorerConfig,
    std::shared_ptr<agentxx::agent::AgentConfig> optimizerConfig);
