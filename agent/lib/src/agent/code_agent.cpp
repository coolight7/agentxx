#include "agentxx/agent/code_agent.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/middlewares/memory_file.h"
#include "agentxx/middlewares/permission.h"
#include "agentxx/middlewares/planning.h"
#include "agentxx/middlewares/skill.h"
#include "agentxx/middlewares/subagent_supervisor.h"
#include "agentxx/middlewares/summarization.h"
#include "agentxx/protocol/mcp_client.h"
#include "agentxx/protocol/openai_provider.h"
#include "agentxx/tools/execute_command.h"
#include "agentxx/tools/filesystem.h"
#include "agentxx/tools/planning.h"
#include "agentxx/tools/rag_search.h"
#include "agentxx/tools/string.h"
#include "agentxx/tools/sub_agent.h"
#include "agentxx/tools/system.h"
#include "agentxx/tools/tool_skill_search.h"
#include "agentxx/tools/ui_control.h"
#include "agentxx/tools/web_search.h"
// CodeGraph 代码分析 (头文件内部按 AGENTXX_ENABLE_CODEGRAPH 条件编译)
#include "agentxx/expand/codegraph_manager.h"
#include "agentxx/tools/codegraph_tool.h"
#include "neograph/mcp/client.h"
#include <optional>

namespace agentxx {
namespace agent {

CodeAgent::CodeAgent(std::shared_ptr<agentxx::agent::AgentConfig> in_config) :
    BaseAgent(std::move(in_config)) {}

CodeAgent::~CodeAgent() = default;

asio::awaitable<void> CodeAgent::setupMiddleware() {
    auto config = agentContext->agentConfig;

    subagentManagerTool_
        = std::make_unique<agentxx::tools::SubAgentManagerTool>("subagent_manager", agentContext);
    agentContext->subagentManagerToolPtr = subagentManagerTool_.get();

    {
        auto permission
            = std::make_shared<agentxx::middleware::PermissionMiddlewareHandle>(agentContext);
        // 权限规则注册 (按 yaml 配置 permission 块: mode / whitelist / blacklist,
        // 读写均注册同一套规则):
        // - 白名单: 始终放行 (最长前缀匹配, 支持 * 通配符)
        // - 黑名单: 始终拒绝 (与白名单同路径时后注册覆盖白名单, 优先生效)
        // - 模式默认规则经 noRuleOperator 生效 (未命中任何已注册规则时的
        //   兜底处理, 见 PermissionMiddlewareHandle::noRuleOperator):
        //   ask=工作目录内 ALLOW + 其余 INTERRUPT / all_ask=INTERRUPT /
        //   pass=ALLOW / deny=DENY
        // 注: 不依赖 "/*" 兜底规则 — 路由最长前缀回退到深层注册子树 (如白名单
        // 目录) 时不会命中根节点 "/*" 规则, 必须由 noRuleOperator 保证语义
        for (const auto& p : config->permissionAllowPaths) {
            permission->setFilesystemPermission(
                p,
                agentxx::middleware::PermissionOperator::ALLOW,
                agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
            );
            permission->setFilesystemPermission(
                p,
                agentxx::middleware::PermissionOperator::ALLOW,
                agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionREAD
            );
        }
        for (const auto& p : config->permissionDenyPaths) {
            permission->setFilesystemPermission(
                p,
                agentxx::middleware::PermissionOperator::DENY,
                agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
            );
            permission->setFilesystemPermission(
                p,
                agentxx::middleware::PermissionOperator::DENY,
                agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionREAD
            );
        }
        switch (config->permissionMode) {
            case agentxx::agent::PermissionMode::Pass:
                // 全部放行: 无规则即放行
                permission->noRuleOperator = agentxx::middleware::PermissionOperator::ALLOW;
                break;
            case agentxx::agent::PermissionMode::Deny:
                // 全部拒绝: 无规则即拒绝
                permission->noRuleOperator = agentxx::middleware::PermissionOperator::DENY;
                break;
            case agentxx::agent::PermissionMode::AllAsk:
                // 所有路径均询问: 无规则即询问
                permission->noRuleOperator = agentxx::middleware::PermissionOperator::INTERRUPT;
                break;
            case agentxx::agent::PermissionMode::Ask:
            default:
                // 当前工作目录内允许, 其他路径询问
                permission->setFilesystemPermission(
                    fmt::format(
                        "{}/*",
                        agentxx::agent::AgentConfigStatic::getCurrentWorkPath()
                    ),
                    agentxx::middleware::PermissionOperator::ALLOW,
                    agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionWRITE
                );
                permission->setFilesystemPermission(
                    fmt::format(
                        "{}/*",
                        agentxx::agent::AgentConfigStatic::getCurrentWorkPath()
                    ),
                    agentxx::middleware::PermissionOperator::ALLOW,
                    agentxx::middleware::PermissionMiddlewareHandle::FilesystemPermissionREAD
                );
                permission->noRuleOperator = agentxx::middleware::PermissionOperator::INTERRUPT;
                break;
        }
        // 注册 tool 名 -> 权限处理函数; 未调用则 handles 为空, 权限拦截不会触发
        permission->registerHandles();
        agentContext->permissionMiddleware = permission;
        agentContext->middlewareHandleContext->handles.push_back(agentContext->permissionMiddleware
        );
    }
    // 添加 Skill Middleware 并记录启动信息
    {
        for (const auto& dirPath : config->skillDirPaths) {
            agentContext->appendComponentInfo.skills.push_back(dirPath);
        }

        auto skillMiddleware = std::make_shared<agentxx::middleware::SkillMiddlewareHandle>(
            config->skillDirPaths,
            agentContext
        );
        agentContext->middlewareHandleContext->handles.push_back(skillMiddleware);
    }
    // 添加 Memory File Middleware 并记录启动信息
    {
        for (const auto& memPath : config->memoryFilePaths) {
            agentContext->appendComponentInfo.memoryFiles.push_back(memPath);
        }

        auto memoryFileMiddleware
            = std::make_shared<agentxx::middleware::MemoryFileMiddlewareHandle>(
                config->memoryFilePaths,
                agentContext
            );
        agentContext->middlewareHandleContext->handles.push_back(memoryFileMiddleware);
    }
    {
        summarizationMiddleware_
            = std::make_shared<agentxx::middleware::SummarizationMiddlewareHandle>(
                subagentManagerTool_.get(),
                agentContext
            );
        agentContext->middlewareHandleContext->handles.push_back(summarizationMiddleware_);
    }
    {
        auto planningMiddleware
            = std::make_shared<agentxx::middleware::PlanningMiddlewareHandle>(agentContext);
        planningMiddleware->toolcalls.push_back(
            std::make_unique<agentxx::tools::WritePlanningTool>(planningMiddleware, agentContext)
        );
        agentContext->middlewareHandleContext->handles.push_back(planningMiddleware);
    }

    /// Toolcall  应当作为最后一层，输出的日志才会是最终的样子
    agentContext->middlewareHandleContext->handles.push_back(
        std::make_shared<
            agentxx::middleware::MiddlewareWrapHandle<agentxx::middleware::BaseMiddlewareState>>(
            "LogPrint",
            agentContext,
            (agentxx::middleware::onGraphNodeBeforeCallFunc) nullptr,
            (agentxx::middleware::onGraphNodeAfterCallFunc) nullptr,
            (agentxx::middleware::onGraphNodeBeforeCallFunc) nullptr,
            [config
             = agentContext->agentConfig](neograph::graph::NodeInput& in) -> asio::awaitable<void> {
                if (config->logPrintMessagesBeforeLLM) {
                    agentxx::middleware::BaseMiddlewareHandleInterface::printMessages(
                        in.state.get_messages(),
                        config->logPrintMessagesBeforeLLMWithSystemMsg
                    );
                }
                co_return;
            },
            (agentxx::middleware::onGraphNodeAfterCallFunc) nullptr,
            [ctx = std::weak_ptr<AgentContext>(agentContext),
             config
             = agentContext->agentConfig](neograph::graph::NodeInput& in) -> asio::awaitable<void> {
                if (config->logPrintToolcall) {
                    agentxx::nodes::ToolcallWrapNode::defStdoutLogOnToolcallStart(in);
                }
                if (auto ctxPtr = ctx.lock()) {
                    auto session = ctxPtr->sessions->get(in.ctx.thread_id);
                    if (session) {
                        session->activity = Activity::ExecutingTool;
                    }
                }
                co_return;
            },
            [ctx = std::weak_ptr<AgentContext>(agentContext), config = agentContext->agentConfig](
                const neograph::graph::NodeInput& in,
                neograph::graph::NodeOutput&      result
            ) -> asio::awaitable<void> {
                if (config->logPrintToolcall) {
                    agentxx::nodes::ToolcallWrapNode::defStdoutLogOnToolcallEnd(in, result);
                }
                if (auto ctxPtr = ctx.lock()) {
                    auto session = ctxPtr->sessions->get(in.ctx.thread_id);
                    if (session) {
                        session->activity = Activity::Idle;
                    }
                }
                co_return;
            }
        )
    );

    co_return;
}

asio::awaitable<std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>> CodeAgent::createTools() {
    auto        config           = agentContext->agentConfig;
    const auto& subagentModelCfg = config->getSubagentModel();

    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> tools
        = co_await BaseAgent::createTools();

    /// MCP tool
    for (const auto& [mcpNamespace, mcpCfg] : config->mcpServerUrls) {
        co_await agentxx::util::catchErrorAsync<bool>(
            [&]() -> asio::awaitable<bool> {
                XX_LOGD("load mcp tool: {} | {}", mcpNamespace, mcpCfg.url);
                auto mcpClient = std::make_shared<agentxx::server::McpClient>(
                    agentxx::server::McpClient::Config{
                        .serverUrl = mcpCfg.url,
                        .protocolVersion
                        = std::string{agentxx::server::McpClient::kProtocol2026_07_28},
                        .toolNamespace     = mcpNamespace,
                        .toolCallTimeout   = mcpCfg.toolTimeout,
                    }
                );
                auto result = co_await mcpClient->initialize();
                if (result.has_value()) {
                    auto mcpTools = co_await mcpClient->listTools();
                    if (mcpTools.has_value()) {
                        for (auto& tool : mcpTools.value()) {
                            tools.push_back(mcpClient->createTool(std::move(tool), agentContext));
                        }
                        // 添加到启动信息
                        agentContext->appendComponentInfo.mcpTools.push_back(mcpNamespace);
                    } else {
                        XX_LOGE(
                            "list mcp tool error: {} | {} | {}",
                            mcpNamespace,
                            mcpCfg.url,
                            mcpTools.error()
                        );
                    }
                } else {
                    XX_LOGE(
                        "load mcp tool error: {} | {} | {}",
                        mcpNamespace,
                        mcpCfg.url,
                        result.error()
                    );
                }
                co_return true;
            },
            [&](std::string errmsg) -> asio::awaitable<bool> {
                XX_LOGE(
                    "[agentxx] Append mcp tool error: {} | {} | {}",
                    mcpNamespace,
                    mcpCfg.url,
                    errmsg
                );
                co_return true;
            }
        );
    }

    /// Filesystem
    tools.push_back(std::make_unique<agentxx::tools::FileSystemListTool>(agentContext));
    tools.push_back(std::make_unique<agentxx::tools::FilesystemReadTextFileTool>(agentContext));
    tools.push_back(std::make_unique<agentxx::tools::FilesystemReadBinaryFileTool>(agentContext));
    tools.push_back(std::make_unique<agentxx::tools::FilesystemWriteFileTool>(agentContext));
    tools.push_back(std::make_unique<agentxx::tools::FilesystemEditTextFileTool>(agentContext));
    tools.push_back(std::make_unique<agentxx::tools::FilesystemGlobTool>(agentContext));
    tools.push_back(std::make_unique<agentxx::tools::FilesystemGrepTool>(agentContext));

    /// String
    tools.push_back(std::make_unique<agentxx::tools::StringHtml2MarkdownTool>(agentContext));
    tools.push_back(std::make_unique<agentxx::tools::StringRegexpTool>(agentContext));

    /// System
#if XX_IS_WIN_D || XX_IS_LINUX_D
    tools.push_back(std::make_unique<agentxx::tools::GetSystemCoreInfoTool>(agentContext));
#endif

    /// Web
    tools.push_back(std::make_unique<agentxx::tools::WebFetchUrlTool>(agentContext));
    tools.push_back(std::make_unique<agentxx::tools::WebFetchUrlMarkdownTool>(agentContext));
    if (config->websearchModel.has_value()) {
        tools.push_back(std::make_unique<agentxx::tools::ModelWebSearchTool>(
            config->websearchModel.value(),
            agentContext
        ));
    } else if (false == config->websearchApiUrl.empty()) {
        tools.push_back(std::make_unique<agentxx::tools::WebSearchTool>(
            config->websearchApiUrl,
            config->websearchConvertHtml2markdown,
            agentContext
        ));
    }

    /// RAG
    if (false == config->ragDocsPaths.empty()) {
        auto client = std::make_shared<agentxx::tools::EmbeddingClient>(
            config->model.baseUrl,
            config->model.apiKey,
            config->model.modelName
        );
        auto docsStore = std::make_shared<agentxx::tools::RAGSearchTool::VectorStore>(client);
        auto docs      = co_await docsStore->scanDocument(config->ragDocsPaths);
        [[maybe_unused]] auto docxSize     = docs.size();
        [[maybe_unused]] auto isAddSuccess = co_await docsStore->addDocuments(std::move(docs));
        XX_LOGD(
            R"_(
┏━━━━━━ RAG Embedding ━━━━━━┓
{}
┗━━━━━━ RAG Embedding ━━━━━━┛
)_",
            isAddSuccess ? fmt::format("┣━ ✅ success: append {} docs", docxSize) : "┣━ ❌ failed"
        );
        tools.push_back(std::make_unique<agentxx::tools::RAGSearchTool>(docsStore, agentContext));
    }

    /// Command execution
#if XX_IS_WIN_D
    tools.push_back(std::make_unique<agentxx::tools::UIControlKeyboardMouseTool>(agentContext));
    tools.push_back(std::make_unique<agentxx::tools::ExecuteWindowsCommandTool>(agentContext));
#elif XX_IS_LINUX_D
    tools.push_back(std::make_unique<agentxx::tools::ExecuteLinuxCommandTool>(agentContext));
    if (agentxx::util::isRunningInWSL()) {
        tools.push_back(std::make_unique<agentxx::tools::ExecuteWindowsCommandTool>(agentContext));
    }
#elif XX_IS_MACOS_D
    tools.push_back(std::make_unique<agentxx::tools::ExecuteLinuxCommandTool>(agentContext));
#endif

    /// CodeGraph 代码分析
    /// - 仅当配置启用了 codegraph [config->enableCodeGraph]、编译启用了
    ///   codegraph [AGENTXX_ENABLE_CODEGRAPH] 且配置了 dataDir 时才添加
    ///   codegraph 系列 tool (索引数据库存放于 {dataDir}/sqlite/codegraph/,
    ///   dataDir 为空时不落盘, 故不注册)
    /// - 索引项目根目录固定为当前程序工作目录
    /// - 索引数据库: {dataDir}/sqlite/codegraph/<折叠路径>/index.db,
    ///   深层路径折叠 + 单段截断控制长度, 子目录可前缀复用最近父级索引
    if (config->enableCodeGraph && config->dataDir.empty()) {
        // dataDir 未配置: codegraph 索引无处落盘, 跳过注册 (与 BaseAgent 警告一致)
        XX_LOGW(
            "CodeGraph enabled in config but dataDir is not set, skip codegraph tools "
            "(in-memory only, no index persistence)"
        );
    } else if (config->enableCodeGraph) {
#if AGENTXX_ENABLE_CODEGRAPH
        auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>(
            agentxx::agent::AgentConfigStatic::getSqliteDir(config->dataDir)
        );
        std::optional<std::string> projectRoot
            = agentxx::agent::AgentConfigStatic::getCurrentWorkPath();
        if (!projectRoot.has_value()) {
            XX_LOGE(
                "CodeGraph enabled in config but get current work path failed, skip codegraph tools"
            );
        } else if (codegraph->initialize(*projectRoot)) {
            tools.push_back(
                std::make_unique<agentxx::tools::CodeGraphSearchTool>(codegraph, agentContext)
            );
            tools.push_back(
                std::make_unique<agentxx::tools::CodeGraphContextTool>(codegraph, agentContext)
            );
            tools.push_back(
                std::make_unique<agentxx::tools::CodeGraphCallersTool>(codegraph, agentContext)
            );
            tools.push_back(
                std::make_unique<agentxx::tools::CodeGraphCalleesTool>(codegraph, agentContext)
            );
            tools.push_back(
                std::make_unique<agentxx::tools::CodeGraphImpactTool>(codegraph, agentContext)
            );
            tools.push_back(
                std::make_unique<agentxx::tools::CodeGraphStatusTool>(codegraph, agentContext)
            );
            tools.push_back(
                std::make_unique<agentxx::tools::CodeGraphIndexTool>(codegraph, agentContext)
            );
            tools.push_back(
                std::make_unique<agentxx::tools::CodeGraphPathTool>(codegraph, agentContext)
            );
            XX_LOGI("CodeGraph enabled, added codegraph tools (project root: {})", *projectRoot);
        } else {
            XX_LOGE("CodeGraph enabled in config but initialize failed, skip codegraph tools");
        }
#else
        XX_LOGE("Codegraph is not available, please re-compile.");
#endif
    }

    /// Subagent
    {
        neograph::graph::NodeContext nodeContext{};
        nodeContext.instructions = "";
        nodeContext.provider     = ModelProviderRegistry::createProvider(subagentModelCfg);

        std::vector<neograph::Tool*> toolPtrs;
        toolPtrs.reserve(tools.size());
        for (auto& t : tools) {
            toolPtrs.push_back(t.get());
        }
        nodeContext.tools = std::move(toolPtrs);

        const auto nodeName = std::string{"subagent_task"};

        subagentManagerTool_->subAgentList.insert(std::make_pair(
            nodeName,
            std::make_shared<agentxx::tools::SubAgentNormalTask>(
                nodeName,
                R"(Create a isolation messages context sub agent to exec. (need system prompt))",
                nodeContext,
                graphRegistry
            )
        ));

        tools.push_back(std::move(subagentManagerTool_));
    }

    co_return tools;
}

void CodeAgent::collectAppendComponentInfo(std::vector<AppendComponentNotification>& notifications
) {
    // MCP 工具
    for (const auto& mcp : agentContext->appendComponentInfo.mcpTools) {
        notifications.push_back(AppendComponentNotification{
            .type         = AppendComponentNotification::Type::Mcp,
            .name         = mcp,
            .success      = true,
            .errorMessage = "",
        });
    }

    // Skill
    for (const auto& skill : agentContext->appendComponentInfo.skills) {
        notifications.push_back(AppendComponentNotification{
            .type         = AppendComponentNotification::Type::Skill,
            .name         = skill,
            .success      = true,
            .errorMessage = "",
        });
    }

    // Memory 文件
    for (const auto& memory : agentContext->appendComponentInfo.memoryFiles) {
        notifications.push_back(AppendComponentNotification{
            .type         = AppendComponentNotification::Type::Memory,
            .name         = memory,
            .success      = true,
            .errorMessage = "",
        });
    }
}

} // namespace agent
} // namespace agentxx
