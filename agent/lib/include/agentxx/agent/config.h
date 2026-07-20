#pragma once

#include "prompt.h"
#include "neograph/api.h"
#include "neograph/json.h"
#include <optional>
#include <string>
#include <vector>

namespace agentxx {
namespace agent {

/// 模型连接配置
/// - 用于主模型和 subagent 模型的统一描述
class ModelConfig {
public:
  std::string baseUrl = "";
  std::string apiKey = "EMPTY";
  std::string modelName = "Agentxx";
  neograph::json extra_config;

  bool isValid() const { return !baseUrl.empty(); }
};

class AgentConfig {
public:
  std::string agentName = "Agentxx";
  std::string agentNameView = "萝卜";

  /// 主模型配置
  ModelConfig model;
  /// subagent 模型配置
  /// - 未指定时默认使用主模型 [model]
  std::optional<ModelConfig> subagentModel;

  /// 获取 subagent 实际使用的模型配置
  /// - 如果指定了 subagentModel 则返回它，否则返回主模型
  const ModelConfig &getSubagentModel() const {
    return subagentModel.has_value() ? subagentModel.value() : model;
  }

  std::string currentSystemName;
  bool isSystemWSL = false;

  agentxx::agent::AgentPrompt prompt;
  std::vector<std::string> skillDirPaths{};
  std::vector<std::string> mcpServerUrls{};
  std::vector<std::string> ragDocsPaths{};

  /// LLM 节点最大重试次数
  /// - 最多执行 1 + 3(retry) = 4 次
  size_t llmMaxRetry = 3;
  /// - 当 toolcall 启用了 [agentxx::tools::XXToolBase::autoSummaryOutput]
  /// 且输出超过限制值 [toolcallSummaryLimitOutputLength] 时进行压缩
  /// - 功能实现见 [agentxx::node::ToolcallWrapNode::execTool]
  size_t toolcallSummaryLimitOutputLength = 2 * 1024;

  /// TODO: 更换api
  /// - [duckduckgo] `https://duckduckgo.com/html/?q={}` 国内连接不稳定
  std::string websearchApiUrl = "";
  bool websearchConvertHtml2markdown = true;
  /// 使用模型进行网络搜索的配置
  /// - 指定后将使用模型搜索替代传统 websearchApiUrl 方式
  /// - 未指定时按 websearchApiUrl 判断是否启用传统搜索
  std::optional<ModelConfig> websearchModel;

  bool checkMessagesUtf8BeforeLLM = true;

  bool logPringToolcall = false;
  bool logPrintMessagesBeforeLLM = false;
  bool logPrintMessagesBeforeLLMWithSystemMsg = false;
  bool logPrintSummarizationResultTokenCount = false;
};

} // namespace agent
} // namespace agentxx