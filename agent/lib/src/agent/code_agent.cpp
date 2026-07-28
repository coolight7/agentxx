#include "agentxx/agent/code_agent.h"

#include "agentxx/middlewares/memory_file.h"
#include "agentxx/middlewares/permission.h"
#include "agentxx/middlewares/planning.h"
#include "agentxx/middlewares/skill.h"
#include "agentxx/middlewares/subagent_supervisor.h"
#include "agentxx/middlewares/summarization.h"
#include "agentxx/protocol/mcp_client.h"
#include "agentxx/protocol/openai_provider.h"
#include "agentxx/tools/cross_agent_query.h"
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
#include "neograph/mcp/client.h"

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
        agentContext->permissionMiddleware
            = std::make_shared<agentxx::middleware::PermissionMiddlewareHandle>(agentContext);
        agentContext->middlewareHandleContext->handles.push_back(agentContext->permissionMiddleware
        );
    }
    {
        auto skillMiddleware = std::make_shared<agentxx::middleware::SkillMiddlewareHandle>(
            config->skillDirPaths,
            agentContext
        );
        agentContext->middlewareHandleContext->handles.push_back(skillMiddleware);
    }
    {
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
                    co_await agentxx::nodes::ToolcallWrapNode::defStdoutLogOnToolcallStart(in);
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
                    co_await agentxx::nodes::ToolcallWrapNode::defStdoutLogOnToolcallEnd(
                        in,
                        result
                    );
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

    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> tools{};

    /// MCP tool
    for (const auto& [mcpNamespace, url] : config->mcpServerUrls) {
        co_await agentxx::util::catchErrorAsync<void>(
            [&]() -> asio::awaitable<void> {
                XX_LOGD("load mcp tool: {} | {}", mcpNamespace, url);
                auto mcpClient = std::make_shared<agentxx::server::McpClient>(
                    agentxx::server::McpClient::Config{
                        .serverUrl = url,
                        .protocolVersion
                        = std::string{agentxx::server::McpClient::kProtocol2025_11_25},
                        .toolNamespace = mcpNamespace,
                    }
                );
                auto result = co_await mcpClient->initialize();
                if (result.has_value()) {
                    auto mcpTools = co_await mcpClient->listTools();
                    if (mcpTools.has_value()) {
                        for (auto& tool : mcpTools.value()) {
                            tools.push_back(mcpClient->createTool(std::move(tool), agentContext));
                        }
                    } else {
                        XX_LOGE(
                            "list mcp tool error: {} | {} | {}",
                            mcpNamespace,
                            url,
                            mcpTools.error()
                        );
                    }
                } else {
                    XX_LOGE("load mcp tool error: {} | {} | {}", mcpNamespace, url, result.error());
                }
                co_return;
            },
            [&](std::string errmsg) -> asio::awaitable<void> {
                XX_LOGE("[agentxx] Append mcp tool error: {} | {} | {}", mcpNamespace, url, errmsg);
                co_return;
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
        auto docsStore    = std::make_shared<agentxx::tools::RAGSearchTool::VectorStore>(client);
        auto docs         = co_await docsStore->scanDocument(config->ragDocsPaths);
        auto docxSize     = docs.size();
        auto isAddSuccess = co_await docsStore->addDocuments(std::move(docs));
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

    /// Cross-agent query
    tools.push_back(std::make_unique<agentxx::tools::CrossAgentQueryTool>(agentContext));

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

} // namespace agent
} // namespace agentxx
