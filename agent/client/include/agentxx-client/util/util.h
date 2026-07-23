#pragma once
#include "agentxx/agent/config.h"
#include <memory>
#include <string>

// ======================== 共享配置构建 ========================
std::shared_ptr<agentxx::agent::AgentConfig> buildDefaultConfig();

/// 从主配置克隆一个子 agent 配置，仅替换 system prompt
std::shared_ptr<agentxx::agent::AgentConfig> makeSubAgentConfig(
    std::shared_ptr<agentxx::agent::AgentConfig> base,
    const std::string&                           systemPrompt
);
