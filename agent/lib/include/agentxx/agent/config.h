#pragma once

#include "neograph/api.h"
#include "neograph/json.h"
#include "prompt.h"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace agentxx {
namespace agent {

/// 模型连接配置
/// - 用于主模型和 subagent 模型的统一描述
/// - 同时作为 OpenAIProvider / AnthropicProvider 的配置
class ModelConfig {
public:
  std::string name; ///< 模型标识名称（来自配置文件 key）
  std::string type = "openai"; ///< 模型类型："openai" 或 "anthropic"
  std::string baseUrl; ///< API 地址，为空时使用 provider 默认官方地址
  std::string apiKey = "EMPTY";
  std::string modelName = "Agentxx"; ///< 发送请求时的 model 字段值
  int connectTimeoutSeconds = 16;
  int readTimeoutSeconds = 24;
  /// 是否在发送 LLM 请求时携带 thinking 内容
  bool sendThinking = false;
  /// Anthropic API version（仅 Anthropic 使用）
  std::string anthropicVersion = "2023-06-01";
  /// max_tokens
  int maxTokens = 8096;
  /// 扩展 JSON 配置，合并到请求 body
  neograph::json extra_config;

  bool isValid() const { return !baseUrl.empty() || apiKey != "EMPTY"; }
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

  /// 可用模型列表 (供运行时切换)
  /// - key: 模型显示名称, value: 模型配置
  /// - 由客户端从配置文件加载填充
  std::map<std::string, ModelConfig> availableModels;
  /// 当前选中的模型显示名称
  /// - 应为 [availableModels] 的 key; 为空时使用 [model]
  std::string currentModelName;

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
  /// - 最多执行 1 + 5(retry) = 6 次
  size_t llmMaxRetry = 5;
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

  /// 在向 llm api 发起请求之前，检查 [messages] 是否符合 utf-8 编码
  bool checkMessagesUtf8BeforeLLM = true;

  /// 日志输出控制
  bool logPringToolcall = false;
  bool logPrintMessagesBeforeLLM = false;
  bool logPrintMessagesBeforeLLMWithSystemMsg = false;
  bool logPrintSummarizationResultTokenCount = false;
};

} // namespace agent
} // namespace agentxx