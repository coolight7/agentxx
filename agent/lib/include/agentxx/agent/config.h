#pragma once

#include "neograph/api.h"
#include "neograph/json.h"
#include "prompt.h"
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace agentxx {
namespace middleware {
class MiddlewareContext;
} // namespace middleware

namespace agent {

/// 权限询问处理模式 (yaml `permission.mode` 指定; 默认 Ask)
///
/// 服务端 CodeAgent 按模式注册文件系统读写默认规则 (见 code_agent.cpp
/// setupMiddleware), 白名单/黑名单路径始终优先于模式默认规则;
/// 客户端 (TUI/CLI) 仅对仍到达的权限 INTERRUPT 询问作兜底处理。
///
/// - Ask:    当前工作目录内允许读写, 其他路径询问用户
/// - AllAsk: 所有路径读写均询问用户
/// - Pass:   全部放行, 不询问
/// - Deny:   全部拒绝, 不询问
enum class PermissionMode : int {
    Ask    = 0,
    AllAsk = 1,
    Pass   = 2,
    Deny   = 3,
};

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
    int         readChunkTimeoutSeconds = 100;
    /// 是否在发送 LLM 请求时携带 thinking 内容
    bool                sendThinking = false;
    std::optional<bool> sslVerify    = std::nullopt;
    /// LLM API 连接池: 该模型端点 (baseUrl) 的最大并发连接数 (yaml `max_concurrent_connections`)
    /// - 默认 5; 0 = 不限制 (仍复用空闲连接)
    /// - LLM 请求启用 HTTP keep-alive 连接池, 复用空闲连接并限制并发建连数,
    ///   超过上限的并发请求排队等待空闲连接 (见 HttpClient::RequestConfig)
    size_t maxConcurrentConnections = 5;
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

/// MCP 服务器配置 (yaml `mcp` 列表项; key 为命名空间)
struct McpServerConfig {
    /// MCP 服务器 URL
    std::string url;
    /// MCP 工具调用超时限制 (毫秒)
    /// - 0 表示不限制
    /// - 默认 120 秒; yaml 中按秒配置 (timeout 字段, 0=不限制)
    std::chrono::milliseconds toolTimeout{std::chrono::seconds{120}};
};

/// 插件运行侧 (yaml `plugins` 条目 sides)
/// - auto: 按导出符号自动决定 (client 侧: 有 agentxx_client_entry 才加载)
/// - agent: 仅 agent 侧加载 (client 侧跳过)
/// - client: 仅 client 侧加载 (agent 侧跳过)
enum class PluginSide : uint8_t {
    Auto = 0,
    Agent,
    Client,
};

/// 插件配置 (yaml `plugins` 列表项)
struct PluginConfig {
    /// 插件动态库路径 或 插件目录 (目录含 plugin.yaml 时按清单解析)
    /// - 必填: 所有插件统一经 path 外置指定, 不再区分内置/外置插件
    ///   (相对路径按程序工作目录解析为绝对路径)
    std::string path;
    /// 是否启用 (默认 true)
    bool enabled = true;
    /// 插件运行侧 (默认 auto = 按导出符号自动决定)
    PluginSide sides = PluginSide::Auto;
    /// 插件参数 (yaml `args`; 宿主原样保存并整体传递给插件,
    /// 不解析具体字段 —— 参数语义由插件自行定义)
    neograph::json args;
};

class AgentConfig {
public:

    std::string agentName     = "Agentxx";
    std::string agentNameView = "Agentxx";

    /// share store 桥接 (运行时注入, 非配置项):
    /// - 非空时, 本 agent 的 `agentxx_share_store` 工具读写该 MiddlewareContext
    ///   的 share store (而非本 agent 自己的), 保证 id 空间一致
    /// - 用途: 同上下文模式子代理 (如上下文压缩) 把长内容写入父会话的
    ///   share store, 摘要中的 id 父会话可直接读取
    std::shared_ptr<agentxx::middleware::MiddlewareContext> sharedShareStoreContext;

    /// 工具白名单过滤开关 (默认 false = 不过滤, 创建全部工具)
    /// - 子代理"无工具/自定义工具/继承父工具"场景由 AgentHost 按需开启
    bool enableToolFiltering = false;
    /// 工具白名单 (enableToolFiltering 时生效): 仅保留名称在列表中的工具;
    /// 列表中的名称在子代理中不存在时自然跳过 (不报错)
    std::vector<std::string> toolWhitelist;

    /// 上下文压缩 (summarization) 中间件开关 (默认 true)
    /// - 子代理默认继承父配置; summarization 发起的压缩子代理显式关闭,
    ///   避免对透传的上下文前缀二次压缩 (破坏 KV/prefix cache 一致性)
    bool enableSummarization = true;

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
    std::map<std::string, McpServerConfig> mcpServerUrls{};
    std::vector<std::string>               ragDocsPaths{};

    /// 统一数据根目录 (全局设置/会话/codegraph 索引等数据的存放根)
    /// - 为空表示不持久化: 全局设置/会话/codegraph 等数据仅存内存,
    ///   不写入磁盘 (BaseAgent 初始化时输出警告); 此时会话持久化与
    ///   codegraph 索引自动禁用 (除非显式指定了 sessionPersistenceRoot)
    /// - 非空时数据子路径:
    ///   - {dataDir}/sqlite/global.db                     全局设置 (TUI 设置等)
    ///   - {dataDir}/sqlite/sessions/{threadId}/          会话数据
    ///   - {dataDir}/sqlite/codegraph/<折叠路径>/index.db CodeGraph 索引
    /// - 相对路径按程序工作目录解析为绝对路径 (由 client 启动时解析)
    std::string dataDir;

    /// CodeGraph 代码分析由插件 agentxx_codegraph 提供 (yaml `plugins` 段配置):
    /// - 插件参数整体存放于 PluginConfig::args (宿主不解析字段语义,
    ///   由插件自行读取: loadPaths/ignorePaths/loadCwd/useGitignore 等)
    /// - CodeAgent 按 plugins 段 path 统一加载插件; dataDir 未配置时
    ///   插件自动跳过

    /// 权限询问处理模式 (yaml `permission.mode`; 见 PermissionMode)
    /// - CodeAgent 启动时按模式注册文件系统读写默认规则:
    ///   Ask=工作目录内允许+其他询问 / AllAsk=全部询问 / Pass=全部放行 / Deny=全部拒绝
    PermissionMode permissionMode = PermissionMode::Ask;

    /// 权限白名单: 始终放行 (ALLOW) 的路径列表 (yaml `permission.whitelist`)
    /// - 最长前缀匹配, 支持 * 通配符; 相对路径按程序工作目录解析为绝对路径
    /// - 优先级高于模式默认规则 (如 Deny 模式下白名单路径仍可访问)
    std::vector<std::string> permissionAllowPaths;

    /// 权限黑名单: 始终拒绝 (DENY) 的路径列表 (yaml `permission.blacklist`)
    /// - 最长前缀匹配, 支持 * 通配符; 相对路径按程序工作目录解析为绝对路径
    /// - 与白名单同路径时黑名单优先 (后注册覆盖)
    std::vector<std::string> permissionDenyPaths;

    /// 是否启用会话 SQLite 持久化 (消息上下文/展示历史/share store)
    /// - 数据目录: {dataDir}/sqlite/sessions/{threadId}/
    ///   - session.db      展示历史 + LLM 上下文 + 会话元数据
    ///   - share_store.db  agentxx_share_store KV 条目
    /// - 开启后会话在重启后可恢复历史消息/上下文/模型选择/share store
    /// - 默认关闭 (库使用方按需开启); agentxx_cli 在 buildDefaultConfig 中开启
    bool enableSessionPersistence = false;

    /// 会话持久化根目录 (enableSessionPersistence 开启时生效)
    /// - 为空时使用 {dataDir}/sqlite/sessions/ (要求 dataDir 非空;
    ///   dataDir 为空且 root 未指定时, 会话持久化自动禁用, 不落盘)
    /// - 数据目录结构: {root}/{threadId}/{session.db, share_store.db}
    std::string sessionPersistenceRoot;

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

    /// 插件配置 (yaml `plugins` 列表; 启动时由 PluginManager 加载)
    std::vector<PluginConfig> plugins{};
};

} // namespace agent
} // namespace agentxx