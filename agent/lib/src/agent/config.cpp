#include "agentxx/agent/config.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/config_validation.h"
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

std::string AgentConfig::resolvedWorkDir() const noexcept {
    // workDir 非空时原样返回 (client/FFI 装配侧已把相对路径按进程 cwd 解析为绝对路径);
    // 为空时回退进程当前工作目录, 与历史行为完全一致
    if (!workDir.empty()) {
        return workDir;
    }
    return AgentConfigStatic::getCurrentWorkPath();
}

std::expected<void, std::string> AgentConfig::validate() const {
    // 校验规则只有一份实现 (validateAgentConfig, 见 config_validation.h);
    // 本函数返回其中的致命问题 (旧调用方语义: 只关心能否启动)
    const auto report = agentxx::agent::validateAgentConfig(*this);
    for (const auto& issue : report.issues) {
        if (issue.level == agentxx::agent::ConfigIssueLevel::Fatal) {
            return std::unexpected{
                fmt::format("AgentConfig: {}: {}", issue.keyPath, issue.message)
            };
        }
    }
    return {};
}

} // namespace agent
} // namespace agentxx
