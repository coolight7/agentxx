#include "agentxx/agent/code_agent.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/middlewares/memory_file.h"
#include "agentxx/middlewares/permission.h"
#include "agentxx/middlewares/planning.h"
#include "agentxx/middlewares/skill.h"
#include "agentxx/middlewares/summarization.h"
#include "agentxx/plugin/plugin_manager.h"
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
#include "agentxx/tools/web_search.h"
#include "agentxx/util/async_offload.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "neograph/mcp/client.h"
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

#if XX_IS_WIN_D
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace agentxx {
namespace agent {

CodeAgent::CodeAgent(std::shared_ptr<agentxx::agent::AgentConfig> in_config) :
    BaseAgent(std::move(in_config)) {}

CodeAgent::~CodeAgent() = default;

asio::awaitable<void> CodeAgent::initMiddleware() {
    auto config = agentContext->agentConfig;

    if (config->enableSubagent) {
        subagentManagerTool_ = std::make_unique<agentxx::tools::SubAgentManagerTool>(
            "subagent_manager",
            agentContext
        );
        agentContext->subagentManagerToolPtr = subagentManagerTool_.get();
    }

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
                // - 工作目录获取失败 (返回空串) 时不注册默认放行规则, 所有路径
                //   均询问 (安全兜底: 注册根目录 "/" 会退化为放行所有路径)
                {
                    const auto workPath = agentxx::agent::AgentConfigStatic::getCurrentWorkPath();
                    if (workPath.empty()) {
                        XX_LOGW("PermissionMode::Ask: getCurrentWorkPath failed, "
                                "no default allow rule registered, all paths will be asked");
                    } else {
                        // 注册工作目录本身: 权限路由最长前缀回退 (prefix_fallback)
                        // 使其下任意子路径均命中此规则 (与 "{workPath}/*" 等价且
                        // 额外覆盖对工作目录自身的访问, 如列出工作目录)
                        permission->setFilesystemPermission(
                            workPath,
                            agentxx::middleware::PermissionOperator::ALLOW,
                            agentxx::middleware::PermissionMiddlewareHandle::
                                FilesystemPermissionWRITE
                        );
                        permission->setFilesystemPermission(
                            workPath,
                            agentxx::middleware::PermissionOperator::ALLOW,
                            agentxx::middleware::PermissionMiddlewareHandle::
                                FilesystemPermissionREAD
                        );
                    }
                }
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
        // 上下文压缩 (summarization) 中间件: 由 AgentConfig::enableSummarization 控制
        // - 子代理默认继承父配置; summarization 发起的压缩子代理显式关闭,
        //   避免对透传的上下文前缀二次压缩 (破坏 KV/prefix cache 一致性)
        if (config->enableSummarization) {
            auto summarizationMiddleware
                = std::make_shared<agentxx::middleware::SummarizationMiddlewareHandle>(agentContext
                );
            agentContext->summarizationMiddleware = summarizationMiddleware;
            agentContext->middlewareHandleContext->handles.push_back(summarizationMiddleware);
        }
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

asio::awaitable<std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>> CodeAgent::initTools() {
    auto config = agentContext->agentConfig;

    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> tools
        = co_await BaseAgent::initTools();

    /// MCP tool
    /// - 多个 server 并行初始化 (独立网络 IO, 互不依赖): 串行加载时每个 server
    ///   需 initialize + listTools 两次网络往返, 多 server 会显著拖慢 agent 启动;
    ///   并行化后总耗时约为最慢 server 的单次加载时间
    /// - 同一 ioCtx 协作式调度, 子协程与主协程交错执行, tools 容器无数据竞争;
    ///   主协程等待全部完成信号后才返回, 期间 initTools 栈帧存活, 引用安全
    /// - 单个 server 失败仅记录日志, 不影响其他 server 与 agent 启动
    const size_t mcpCount = config->mcpServerUrls.size();
    if (mcpCount > 0) {
        using McpDoneChannel
            = asio::experimental::concurrent_channel<void(neograph_asio_error_code, size_t)>;
        auto mcpEx  = co_await asio::this_coro::executor;
        auto doneCh = std::make_shared<McpDoneChannel>(mcpEx, mcpCount);
        // 单个 MCP server 加载 (工具 push 到 tools; 失败仅记录日志)
        auto loadOneMcp
            = [&](std::string ns, agentxx::agent::McpServerConfig mcpCfg) -> asio::awaitable<void> {
            co_await agentxx::util::catchErrorAsync<bool>(
                [&]() -> asio::awaitable<bool> {
                    // 逐步上报启动进度: MCP 网络连接较慢, 逐 server 报告名称+地址
                    notifyStartup(fmt::format("加载 MCP server: {} ({})", ns, mcpCfg.url));
                    XX_LOGD("load mcp tool: {} | {}", ns, mcpCfg.url);
                    auto mcpClient = std::make_shared<agentxx::server::McpClient>(
                        agentxx::server::McpClient::Config{
                            .serverUrl = mcpCfg.url,
                            .protocolVersion
                            = std::string{agentxx::server::McpClient::kProtocol2026_07_28},
                            .toolNamespace   = ns,
                            .toolCallTimeout = mcpCfg.toolTimeout,
                        }
                    );
                    auto result = co_await mcpClient->initialize();
                    if (result.has_value()) {
                        auto mcpTools = co_await mcpClient->listTools();
                        if (mcpTools.has_value()) {
                            for (auto& tool : mcpTools.value()) {
                                tools.push_back(mcpClient->createTool(std::move(tool), agentContext)
                                );
                            }
                            // 添加到启动信息
                            agentContext->appendComponentInfo.mcpTools.push_back(ns);
                        } else {
                            XX_LOGE(
                                "list mcp tool error: {} | {} | {}",
                                ns,
                                mcpCfg.url,
                                mcpTools.error()
                            );
                        }
                    } else {
                        XX_LOGE(
                            "load mcp tool error: {} | {} | {}",
                            ns,
                            mcpCfg.url,
                            result.error()
                        );
                    }
                    co_return true;
                },
                [&](std::string errmsg) -> asio::awaitable<bool> {
                    XX_LOGE(
                        "[agentxx] Append mcp tool error: {} | {} | {}",
                        ns,
                        mcpCfg.url,
                        errmsg
                    );
                    co_return true;
                }
            );
            co_return;
        };

        for (const auto& [mcpNamespace, mcpCfg] : config->mcpServerUrls) {
            asio::co_spawn(
                mcpEx,
                [&tools, loadOneMcp, ns = mcpNamespace, cfg = mcpCfg]() -> asio::awaitable<void> {
                    co_await loadOneMcp(ns, cfg);
                },
                // 完成处理器: 捕获协程内部未捕获的异常 (loadOneMcp 已捕获普通异常,
                // 此处兜底防止异常逃逸 detached 协程 → terminate), 并保证无论如何
                // 都发送一次完成信号, 避免主协程收不满 n 个信号死锁
                [doneCh](std::exception_ptr ep) {
                    if (ep) {
                        agentxx::util::catchError<bool>(
                            [&]() {
                                std::rethrow_exception(ep);
                                return true;
                            },
                            [](std::string errmsg) {
                                XX_LOGE("[agentxx] MCP load coroutine error: {}", errmsg);
                                return false;
                            }
                        );
                    }
                    doneCh->try_send(neograph_asio_error_code{}, 0);
                }
            );
        }
        for (size_t i = 0; i < mcpCount; ++i) {
            co_await doneCh->async_receive(asio::use_awaitable);
        }
    }

    /// Filesystem
    tools.push_back(std::make_unique<agentxx::tools::FileSystemListTool>(agentContext));
    tools.push_back(std::make_unique<agentxx::tools::FilesystemReadTextFileTool>(agentContext));
    tools.push_back(std::make_unique<agentxx::tools::FilesystemWriteFileTool>(agentContext));
    tools.push_back(std::make_unique<agentxx::tools::FilesystemEditTextFileTool>(agentContext));
    tools.push_back(std::make_unique<agentxx::tools::FilesystemGlobTool>(agentContext));
    tools.push_back(std::make_unique<agentxx::tools::FilesystemGrepTool>(agentContext));

    /// String
    tools.push_back(std::make_unique<agentxx::tools::StringHtml2MarkdownTool>(agentContext));
    tools.push_back(std::make_unique<agentxx::tools::StringRegexpTool>(agentContext));

    /// System (系统资源监控工具已迁移至插件 agentxx_system_monitor:
    /// agentxx_get_system_core_info 由插件注册, 见 agentxx-config.yaml plugins 段)

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
        // 逐步上报启动进度: RAG 文档扫描 + embedding 生成耗时较长
        notifyStartup("加载 RAG 文档并生成向量索引 ...");
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
    tools.push_back(std::make_unique<agentxx::tools::ExecuteWindowsCommandTool>(agentContext));
#elif XX_IS_LINUX_D
    tools.push_back(std::make_unique<agentxx::tools::ExecuteBashCommandTool>(agentContext));
    if (agentxx::util::isRunningInWSL()) {
        tools.push_back(std::make_unique<agentxx::tools::ExecuteWindowsCommandTool>(agentContext));
    }
#elif XX_IS_MACOS_D
    tools.push_back(std::make_unique<agentxx::tools::ExecuteBashCommandTool>(agentContext));
#endif

    /// Subagent (由 AgentConfig::enableSubagent 控制, yaml subagent.enable)
    /// - 注册表仅承载名称/描述等静态元数据 (SubAgentTaskBase);
    ///   实际执行由 AgentHost 派生独立 agent 完成 (中断委派, 不在此创建
    ///   嵌套 subgraph / nodeContext)
    if (config->enableSubagent) {
        const auto nodeName = std::string{"subagent_task"};

        subagentManagerTool_->subAgentList.insert(std::make_pair(
            nodeName,
            std::make_shared<agentxx::tools::SubAgentNormalTask>(
                nodeName,
                R"(Create a isolation messages context sub agent to exec. (need system prompt))"
            )
        ));

        tools.push_back(std::move(subagentManagerTool_));
    } else {
        // 已构造的 subagent 管理器无需注册, 清理指针避免悬空
        subagentManagerTool_.reset();
        agentContext->subagentManagerToolPtr = nullptr;
        XX_LOGD("CodeAgent: subagent disabled by config (subagent.enable=false)");
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

    // Agent 侧加载的插件 (数量 + 列表; success 反映 enabled 状态)
    if (agentContext->pluginManager) {
        for (const auto& plugin : agentContext->pluginManager->list()) {
            notifications.push_back(AppendComponentNotification{
                .type         = AppendComponentNotification::Type::Plugin,
                .name         = plugin.name,
                .success      = plugin.enabled,
                .errorMessage = "",
            });
        }
    }
}

} // namespace agent
} // namespace agentxx
