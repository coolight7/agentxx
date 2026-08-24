#include "agentxx/agent/config.h"

#include "agentxx/agent/config_static.h"
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
    // workDir: 非空时必须为绝对路径 (相对路径的解析归属装配侧, 按进程 cwd 展开;
    // lib 内不隐式解析, 避免"配置相对路径在不同启动目录下语义漂移")
    if (!workDir.empty() && !std::filesystem::path(workDir).is_absolute()) {
        return std::unexpected{fmt::format(
            "AgentConfig: workDir must be an absolute path (got '{}'); "
            "resolve relative paths against the process cwd at assembly time",
            workDir
        )};
    }
    return {};
}

} // namespace agent
} // namespace agentxx
