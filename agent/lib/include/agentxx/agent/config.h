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

    static const ModelConfig defaultModelConfig;

    std::string name;            ///< 模型标识名称（来自配置文件 key）
    std::string type = "openai"; ///< 模型类型："openai" / "openai-responses" / "anthropic"
    std::string baseUrl;         ///< API 地址，为空时使用 provider 默认官方地址
    std::string apiKey                  = "EMPTY";
    std::string modelName               = "Agentxx"; ///< 发送请求时的 model 字段值
    int         connectTimeoutSeconds   = 16;
    int         readChunkTimeoutSeconds = 60;
    /// 是否在发送 LLM 请求时携带 thinking 内容
    bool                sendThinking = false;
    std::optional<bool> sslVerify    = std::nullopt;
    /// Anthropic API version（仅 Anthropic 使用）
    std::string anthropicVersion = "2023-06-01";
    /// 自定义 API 路径（如 "/v1/chat/completions"）
    /// - 为空时使用 provider 默认路径: openai 为 "/chat/completions", codex/responses 为
    /// "/responses"
    /// - 用于适配各种 OpenAI 兼容服务 (DeepSeek/Moonshot/Ollama/Azure 等) 的不同端点路径
    std::string apiPath;
    /// 额外 HTTP 请求头 (如自定义鉴权头/网关透传头)
    std::map<std::string, std::string> extraHeaders;
    /// 模型支持的最大上下文 token 数
    /// - 0 表示未指定, 此时上下文压缩中间件使用其默认值
    ///   [agentxx::middleware::SummarizationMiddlewareHandle::defaultModelSupportMaxToken]
    size_t modelContenxtMaxToken = 0;
    /// 扩展 JSON 配置，合并到请求 body
    neograph::json extra_config;

    bool isValid() const;

    bool isOpenaiResponseApi() const;
};

class AgentConfig {
public:

    std::string agentName     = "Agentxx";
    std::string agentNameView = "Agentxx";

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
    const ModelConfig& getSubagentModel() const;

    std::string currentSystemName;
    bool        isSystemWSL = false;

    agentxx::agent::AgentPrompt prompt;
    std::vector<std::string>    skillDirPaths{};
    /// 上下文文件路径列表
    /// - 支持绝对路径或相对路径（相对路径按程序工作目录解析）
    /// - 文件内容会在每次模型调用时注入系统提示词
    std::vector<std::string> memoryFilePaths{};
    /// MCP 服务器配置
    /// - key: MCP 命名空间 (每个 MCP 的命名空间应当唯一，作为该服务所有 tool 的名称前缀)
    /// - value: MCP 服务器 URL
    std::map<std::string, std::string> mcpServerUrls{};
    std::vector<std::string>           ragDocsPaths{};

    /// 是否启用 CodeGraph 代码分析 (需编译时启用 AGENTXX_ENABLE_CODEGRAPH)
    /// - 配置启用且编译启用时, CodeAgent 才会注册 codegraph 系列 tool
    /// - 索引项目根目录固定为当前程序工作目录
    /// - 索引数据库: ~/.agentxx/sqlite/codegraph/<折叠路径>/index.db
    ///   (深层路径折叠 + 单段截断控制长度, 子目录可前缀复用最近父级索引)
    bool enableCodeGraph = true;

    /// LLM 节点最大重试次数
    /// - 最多执行 1 + 5(retry) = 6 次
    size_t llmMaxRetry = 5;
    /// - 当 toolcall 启用了 [agentxx::tools::XXToolBase::autoSummaryOutput]
    /// 且输出超过限制值 [toolcallSummaryLimitOutputLength] 时进行压缩
    /// - 功能实现见 [agentxx::node::ToolcallWrapNode::execTool]
    size_t toolcallSummaryLimitOutputLength = 2 * 1024;

    /// TODO: 更换api
    /// - [duckduckgo] `https://duckduckgo.com/html/?q={}` 国内连接不稳定
    std::string websearchApiUrl               = "";
    bool        websearchConvertHtml2markdown = true;
    /// 使用模型进行网络搜索的配置
    /// - 指定后将使用模型搜索替代传统 websearchApiUrl 方式
    /// - 未指定时按 websearchApiUrl 判断是否启用传统搜索
    std::optional<ModelConfig> websearchModel;

    /// 在向 llm api 发起请求之前，检查 [messages] 是否符合 utf-8 编码
    bool checkMessagesBeforeLLM = true;

    /// 日志输出控制
    bool logPrintToolcall                       = false;
    bool logPrintMessagesBeforeLLM              = false;
    bool logPrintMessagesBeforeLLMWithSystemMsg = false;
    bool logPrintSummarizationResultTokenCount  = false;
};

} // namespace agent
} // namespace agentxx