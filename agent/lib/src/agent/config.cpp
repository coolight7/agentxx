#include "agentxx/agent/config.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/config_validation.h"
#include <algorithm>
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

size_t AgentConfig::eventContentLimitLength() const {
    // 两项取小 (0 视为该项不限制): 字符数 <= 字节数, 把字节上限当字符阈值用
    // 只会更早压缩, 不会放过超出字节上限的内容; 两者都为 0 时不压缩
    if (eventValueMaxBytes == 0) {
        return toolcallSummaryLimitOutputLength;
    }
    if (toolcallSummaryLimitOutputLength == 0) {
        return eventValueMaxBytes;
    }
    return std::min(eventValueMaxBytes, toolcallSummaryLimitOutputLength);
}

std::string AgentConfig::defaultModelName() const {
    if (!availableModels.empty()) {
        // `model.use.default` 指定且存在
        if (!currentModelName.empty() && availableModels.contains(currentModelName)) {
            return currentModelName;
        }
        // 没指定 `model.use` 时, 上游名恰好也是条目名
        if (!model.modelName.empty() && availableModels.contains(model.modelName)) {
            return model.modelName;
        }
        // 都没有: 建表时第一个注册的条目成为默认 (`availableModels` 是 std::map)
        return availableModels.begin()->first;
    }
    // 没有可用模型列表: 主模型按上游名入表, 默认就是它
    return model.modelName;
}

std::string AgentConfig::resolvedWorkDir() const noexcept {
    // workDir 非空时原样返回 (client/FFI 加载侧已把相对路径按进程 cwd 解析为绝对路径);
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
            return std::unexpected{fmt::format("AgentConfig: {}: {}", issue.keyPath, issue.message)
            };
        }
    }
    return {};
}

} // namespace agent
} // namespace agentxx
