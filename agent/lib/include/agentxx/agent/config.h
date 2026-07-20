#pragma once

#include "prompt.h"
#include <string>
#include <vector>

namespace agentxx {
namespace agent {

class AgentConfig {
public:
  std::string agentName = "Agentxx";
  std::string agentNameView = "萝卜";

  std::string modelOpenAIBaseUrl = "";
  std::string modelOpenAIApiKey = "EMPTY";
  std::string modelOpenAIModelName = "Agentxx";

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

  bool checkMessagesUtf8BeforeLLM = true;

  bool logPringToolcall = false;
  bool logPrintMessagesBeforeLLM = false;
  bool logPrintMessagesBeforeLLMWithSystemMsg = false;
  bool logPrintSummarizationResultTokenCount = false;
};

} // namespace agent
} // namespace agentxx