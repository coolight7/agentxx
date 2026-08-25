#include "agentxx/agent/code_agent.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/resource_applier.h"
#include "agentxx/middlewares/memory_file.h"
#include "agentxx/middlewares/permission.h"
#include "agentxx/middlewares/planning.h"
#include "agentxx/middlewares/skill.h"
#include "agentxx/middlewares/summarization.h"
#include "agentxx/middlewares/worktree.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/protocol/mcp_client.h"
#include "agentxx/protocol/openai_provider.h"
#include "agentxx/tools/git_worktree.h"
#include "agentxx/tools/subagent.h"
#include "agentxx/tools/tool_skill_search.h"
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
                // 会话工作目录内允许, 其他路径询问
                // - 工作目录取 AgentConfig::workDir (yaml work_dir; 未配置回退进程
                //   cwd), 使嵌入多实例/远程 server 场景下权限边界跟随会话配置而非
                //   进程启动目录
                // - 工作目录获取失败 (返回空串) 时不注册默认放行规则, 所有路径
                //   均询问 (安全兜底: 注册根目录 "/" 会退化为放行所有路径)
                {
                    const auto workPath = config->resolvedWorkDir();
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
    // - 目录不存在的配置项记为加载失败 (failedComponents), 供客户端 "Failed"
    //   组统计与弹窗查看; 存在的目录照常记录为成功 (实际 SKILL.md 解析错误由
    //   SkillMiddleware 懒加载时自行处理, 不在此判定)
    std::shared_ptr<agentxx::middleware::SkillMiddlewareHandle> skillMiddleware;
    {
        for (const auto& dirPath : config->skillDirPaths) {
            std::error_code ec;
            const bool       exists = std::filesystem::is_directory(dirPath, ec);
            if (exists) {
                agentContext->appendComponentInfo.skills.push_back(dirPath);
            } else {
                agentContext->appendComponentInfo.failedComponents.push_back(
                    AppendComponentNotification{
                        .type         = AppendComponentNotification::Type::Skill,
                        .name         = dirPath,
                        .success      = false,
                        .errorMessage = ec ? fmt::format("directory not accessible: {}",
                                                         ec.message())
                                           : "directory not found",
                    }
                );
            }
        }

        skillMiddleware = std::make_shared<agentxx::middleware::SkillMiddlewareHandle>(
            config->skillDirPaths,
            agentContext
        );
        agentContext->middlewareHandleContext->handles.push_back(skillMiddleware);
    }
    // 添加 Memory File Middleware 并记录启动信息
    // - 文件不存在的配置项记为加载失败 (failedComponents), 供客户端 "Failed"
    //   组统计与弹窗查看; 存在的文件照常记录为成功 (读取错误由
    //   MemoryFileMiddleware 懒加载时自行处理, 不在此判定)
    std::shared_ptr<agentxx::middleware::MemoryFileMiddlewareHandle> memoryFileMiddleware;
    {
        for (const auto& memPath : config->memoryFilePaths) {
            std::error_code ec;
            const bool      exists = std::filesystem::is_regular_file(memPath, ec);
            if (exists) {
                agentContext->appendComponentInfo.memoryFiles.push_back(memPath);
            } else {
                agentContext->appendComponentInfo.failedComponents.push_back(
                    AppendComponentNotification{
                        .type         = AppendComponentNotification::Type::Memory,
                        .name         = memPath,
                        .success      = false,
                        .errorMessage = ec ? fmt::format("file not accessible: {}", ec.message())
                                           : "file not found",
                    }
                );
            }
        }

        memoryFileMiddleware
            = std::make_shared<agentxx::middleware::MemoryFileMiddlewareHandle>(
                config->memoryFilePaths,
                agentContext
            );
        agentContext->middlewareHandleContext->handles.push_back(memoryFileMiddleware);
    }
    // 会话资源应用器装配 (插件 Skill/Memory/MCP 扩展; 见 resource_applier.h):
    // - 声明式资源由 PluginManager 在插件 entry 成功后经此应用 (失败不生效)
    // - 运行时注册经 vtable register_skill_dir 等转发到本应用器
    // - io executor 记录于本协程 (init 运行在 io 线程), 供 MCP 异步连接派发
    agentContext->resourceApplier = std::make_shared<agentxx::agent::AgentResourceApplier>(
        agentContext,
        co_await asio::this_coro::executor,
        std::move(skillMiddleware),
        std::move(memoryFileMiddleware)
    );
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
        // 规划写入工具 (agentxx_planning_write) 已迁移至 agentxx_planning 插件
        // (经 AgentxxPlanningIface 写入 state); 此处仅记录句柄到上下文,
        // 供插件系统与 system prompt 注入链路取用
        agentContext->planningMiddleware = planningMiddleware;
        agentContext->middlewareHandleContext->handles.push_back(planningMiddleware);
    }
    {
        // worktree 模式提示词中间件 (yaml `worktree.enable`): 每轮按会话绑定
        // 状态注入行为规范 (未绑定→提示创建 / 已绑定→提交与收尾规范 /
        // 子代理继承→隔离提醒), 与 PlanningMiddleware 同一注入通道
        if (config->enableWorktree) {
            agentContext->middlewareHandleContext->handles.push_back(
                std::make_shared<agentxx::middleware::WorktreeMiddlewareHandle>(agentContext)
            );
        }
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
                        session->activity = SessionActivity::ExecutingTool;
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
                        session->activity = SessionActivity::Idle;
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

    // 内置编程工具 (filesystem/string/web/rag/command/planning_write) 已全部
    // 迁移为独立插件 (agent/plugins/agentxx_{filesystem,string,websearch,
    // execute_command,rag_search,planning}), 运行期经 PluginManager 注册,
    // 同名同行为; 本函数仅保留 subagent / MCP / 延迟加载等 lib 侧装配

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

    /// Git worktree 管理 (由 AgentConfig::enableWorktree 控制, yaml worktree.enable)
    /// - 创建即绑定会话: 相对路径基准/权限隔离边界自动切换, 详见 tools/git_worktree.h
    if (config->enableWorktree) {
        tools.push_back(std::make_unique<agentxx::tools::GitWorktreeTool>(agentContext));
    }

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
                    notifyInitProgress(fmt::format("加载 MCP: {} ({})", ns, mcpCfg.url));
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
                            // 记录加载失败组件 (供客户端 "Failed" 组统计与弹窗查看)
                            agentContext->appendComponentInfo.failedComponents.push_back(
                                AppendComponentNotification{
                                    .type    = AppendComponentNotification::Type::Mcp,
                                    .name    = ns,
                                    .success = false,
                                    .errorMessage
                                    = fmt::format("list tools failed: {} ({})",
                                                  mcpTools.error(),
                                                  mcpCfg.url),
                                }
                            );
                        }
                    } else {
                        XX_LOGE(
                            "load mcp tool error: {} | {} | {}",
                            ns,
                            mcpCfg.url,
                            result.error()
                        );
                        // 记录加载失败组件 (供客户端 "Failed" 组统计与弹窗查看)
                        agentContext->appendComponentInfo.failedComponents.push_back(
                            AppendComponentNotification{
                                .type         = AppendComponentNotification::Type::Mcp,
                                .name         = ns,
                                .success      = false,
                                .errorMessage = fmt::format("initialize failed: {} ({})",
                                                            result.error(),
                                                            mcpCfg.url),
                            }
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
                    // 异常路径同样记录加载失败组件 (供客户端 "Failed" 组统计与弹窗查看)
                    agentContext->appendComponentInfo.failedComponents.push_back(
                        AppendComponentNotification{
                            .type         = AppendComponentNotification::Type::Mcp,
                            .name         = ns,
                            .success      = false,
                            .errorMessage = fmt::format("{} ({})", errmsg, mcpCfg.url),
                        }
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

    co_return tools;
}

} // namespace agent
} // namespace agentxx
