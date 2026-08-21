#pragma once
#include "agentxx/agent/training.h"
#include <memory>
#include <string>
#include <vector>

// ======================== 训练模式 ========================
// 测试用例加载复用 agentxx::agent::loadTestCasesFromDirectory(dir, true)
// 的递归实现, 不再在客户端重复维护一份目录遍历/JSON 解析代码。

/// 获取项目根目录（agentxx 源码根目录）
std::string findProjectRoot();

/// 替换输入中的 {agentxx_root} 占位符
void replacePlaceholders(
    std::vector<agentxx::agent::TrainingTestCase>& cases,
    std::string_view                               projectRoot
);

void runTrainingMode(
    std::shared_ptr<agentxx::agent::AgentConfig> baseConfig,
    std::shared_ptr<agentxx::agent::AgentConfig> scorerConfig,
    std::shared_ptr<agentxx::agent::AgentConfig> optimizerConfig
);
