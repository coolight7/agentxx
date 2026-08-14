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
#include "agentxx/util/async_offload.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "neograph/mcp/client.h"
#include <chrono>
#include <optional>

namespace agentxx {
namespace agent {

CodeAgent::CodeAgent(std::shared_ptr<agentxx::agent::AgentConfig> in_config) :
    BaseAgent(std::move(in_config)) {}

CodeAgent::~CodeAgent() = default;

std::shared_ptr<expand::CodeGraphManager> CodeAgent::codegraphManager() {
    // createTools 中启用且初始化成功时填充 codegraph_, 否则为空 (不可用)
    return codegraph_;
}

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
        auto summarizationMiddleware
            = std::make_shared<agentxx::middleware::SummarizationMiddlewareHandle>(
                subagentManagerTool_.get(),
                agentContext
            );
        agentContext->summarizationMiddleware = summarizationMiddleware;
        agentContext->middlewareHandleContext->handles.push_back(summarizationMiddleware);
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
    /// - 多个 server 并行初始化 (独立网络 IO, 互不依赖): 串行加载时每个 server
    ///   需 initialize + listTools 两次网络往返, 多 server 会显著拖慢 agent 启动;
    ///   并行化后总耗时约为最慢 server 的单次加载时间
    /// - 同一 ioCtx 协作式调度, 子协程与主协程交错执行, tools 容器无数据竞争;
    ///   主协程等待全部完成信号后才返回, 期间 createTools 栈帧存活, 引用安全
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
    tools.push_back(std::make_unique<agentxx::tools::UIControlKeyboardMouseTool>(agentContext));
    tools.push_back(std::make_unique<agentxx::tools::ExecuteWindowsCommandTool>(agentContext));
#elif XX_IS_LINUX_D
    tools.push_back(std::make_unique<agentxx::tools::ExecuteBashCommandTool>(agentContext));
    if (agentxx::util::isRunningInWSL()) {
        tools.push_back(std::make_unique<agentxx::tools::ExecuteWindowsCommandTool>(agentContext));
    }
#elif XX_IS_MACOS_D
    tools.push_back(std::make_unique<agentxx::tools::ExecuteBashCommandTool>(agentContext));
#endif

    /// CodeGraph 代码分析
    /// - 仅当配置启用了 codegraph [config->enableCodeGraph]、编译启用了
    ///   codegraph [AGENTXX_ENABLE_CODEGRAPH] 且配置了 dataDir 时才添加
    ///   codegraph 系列 tool (索引数据库存放于 {dataDir}/sqlite/codegraph/,
    ///   dataDir 为空时不落盘, 故不注册)
    /// - 索引数据库按项目根目录 (通常为当前工作目录) 定位:
    ///   {dataDir}/sqlite/codegraph/<折叠路径>/index.db,
    ///   深层路径折叠 + 单段截断控制长度, 子目录可前缀复用最近父级索引
    /// - 索引内容范围由 CodeGraphIndexConfig 控制 (yaml `codegraph` 块):
    ///   loadPaths 加载路径列表 (为空时索引项目根目录) /
    ///   ignorePaths 忽略路径 (支持 * 通配符) / useGitignore (.gitignore 与
    ///   .gitmodules 子模块目录忽略, 默认开启)
    if (config->enableCodeGraph && config->dataDir.empty()) {
        // dataDir 未配置: codegraph 索引无处落盘, 跳过注册 (与 BaseAgent 警告一致)
        XX_LOGW("CodeGraph enabled in config but dataDir is not set, skip codegraph tools "
                "(in-memory only, no index persistence)");
    } else if (config->enableCodeGraph) {
#if AGENTXX_ENABLE_CODEGRAPH
        // 逐步上报启动进度: CodeGraph 初始化涉及索引库打开/项目根扫描
        notifyStartup("初始化 CodeGraph 代码索引 ...");
        // 打印索引数据目录: 便于核对持久化位置是否稳定
        // (project_root / dataDir 变化会导致每次新建索引库, 表现为"每次从头索引")
        XX_LOGI(
            "[codegraph] index data dir: {}",
            agentxx::agent::AgentConfigStatic::getSqliteDir(config->dataDir)
        );
        // 索引过滤配置 (加载路径/忽略路径/gitignore 开关; 相对路径已由
        // client 启动时按工作目录解析为绝对路径)
        agentxx::expand::CodeGraphIndexConfig cgConfig;
        cgConfig.loadPaths          = config->codeGraphPaths;
        cgConfig.ignorePaths        = config->codeGraphIgnorePaths;
        cgConfig.useGitignore       = config->codeGraphUseGitignore;
        cgConfig.autoLoadProjectRoot = config->codeGraphLoadCwd;
        codegraph_             = std::make_shared<agentxx::expand::CodeGraphManager>(
            agentxx::agent::AgentConfigStatic::getSqliteDir(config->dataDir),
            std::move(cgConfig)
        );
        std::optional<std::string> projectRoot
            = agentxx::agent::AgentConfigStatic::getCurrentWorkPath();
        if (!projectRoot.has_value()) {
            XX_LOGE(
                "CodeGraph enabled in config but get current work path failed, skip codegraph tools"
            );
        } else if (codegraph_->initialize(*projectRoot)) {
            tools.push_back(
                std::make_unique<agentxx::tools::CodeGraphSearchTool>(codegraph_, agentContext)
            );
            tools.push_back(
                std::make_unique<agentxx::tools::CodeGraphContextTool>(codegraph_, agentContext)
            );
            tools.push_back(
                std::make_unique<agentxx::tools::CodeGraphCallersTool>(codegraph_, agentContext)
            );
            tools.push_back(
                std::make_unique<agentxx::tools::CodeGraphCalleesTool>(codegraph_, agentContext)
            );
            tools.push_back(
                std::make_unique<agentxx::tools::CodeGraphImpactTool>(codegraph_, agentContext)
            );
            tools.push_back(
                std::make_unique<agentxx::tools::CodeGraphStatusTool>(codegraph_, agentContext)
            );
            tools.push_back(
                std::make_unique<agentxx::tools::CodeGraphIndexTool>(codegraph_, agentContext)
            );
            tools.push_back(
                std::make_unique<agentxx::tools::CodeGraphPathTool>(codegraph_, agentContext)
            );
            // 日志: 展示索引范围 (加载路径列表或项目根) 与过滤配置
            {
                std::string scope;
                if (!config->codeGraphPaths.empty()) {
                    for (size_t i = 0; i < config->codeGraphPaths.size(); ++i) {
                        if (i > 0) {
                            scope += ", ";
                        }
                        scope += config->codeGraphPaths[i];
                    }
                } else if (config->codeGraphLoadCwd) {
                    scope = *projectRoot;
                } else {
                    scope = "(未配置加载路径且 load_cwd=false: 不自动索引)";
                }
                XX_LOGI(
                    "CodeGraph enabled, added codegraph tools (index scope: {}, ignore_paths: {}, "
                    "use_gitignore: {})",
                    scope,
                    config->codeGraphIgnorePaths.size(),
                    config->codeGraphUseGitignore
                );
            }

            // 后台预热索引 (Eager 预计算):
            // - 启动延迟后对索引范围 (加载路径列表, 未配置时为项目根) 执行一次
            //   增量索引, 使随后的 codegraph 查询尽快落在预计算数据上
            //   (已有索引库时仅重扫变更文件, 首次为全量)
            // - 挂在 agent ioCtx 上 (detached), offload 到 blockingPool 执行,
            //   不阻塞 agent 初始化/首个请求; 索引期间查询经共享锁可并发读取旧数据
            // - 编码为弱引用持有 agentContext: agent 已销毁时跳过
            {
                auto warmupAgent = std::weak_ptr<agentxx::agent::AgentContext>{agentContext};
                auto warmupCg    = codegraph_;
                asio::co_spawn(
                    ioCtx->get_executor(),
                    [warmupAgent, warmupCg]() -> asio::awaitable<void> {
                        // 延迟启动: 让 agent 初始化/组件加载等启动流程先行,
                        // 避免预热索引与启动期任务竞争线程池
                        asio::steady_timer timer(co_await asio::this_coro::executor);
                        timer.expires_after(std::chrono::seconds(2));
                        co_await timer.async_wait(asio::use_awaitable);
                        auto agentPtr = warmupAgent.lock();
                        if (!agentPtr || !agentPtr->blockingPool) {
                            co_return;
                        }
                        XX_LOGI("[codegraph] background warmup index start");
                        const auto startAt = std::chrono::steady_clock::now();
                        bool       ok      = co_await agentxx::util::offloadAsync<bool>(
                            *agentPtr->blockingPool,
                            [warmupCg]() -> asio::awaitable<bool> {
                                // 按加载路径列表索引 (未配置时为项目根目录)
                                co_return warmupCg->updateIndex();
                            }
                        );
                        const auto costMs
                            = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - startAt
                              )
                                  .count();
                        XX_LOGI(
                            "[codegraph] background warmup index {} ({}ms)",
                            ok ? "done" : "failed",
                            costMs
                        );
                    },
                    asio::detached
                );
            }
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
