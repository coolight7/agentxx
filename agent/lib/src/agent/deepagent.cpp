#include "agentxx/agent/deepagent.h"

#include <cassert>

namespace agentxx {
namespace agent {

DeepAgent::DeepAgent(std::shared_ptr<agentxx::agent::AgentConfig> in_config) {
    ioCtx                     = std::make_shared<asio::io_context>();
    agentContext              = std::make_shared<AgentContext>();
    agentContext->agentConfig = in_config;
    assert(nullptr != in_config);
    assert(in_config->model.isValid());
}

asio::awaitable<void> DeepAgent::init() {
#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
    XX_LOGD("Enable asio/async file RW");
#else
    XX_LOGD("Disable asio/async file RW");
#endif

    auto config = agentContext->agentConfig;
    // subagent 模型配置：未指定时默认取主模型
    const auto& subagentModelCfg = config->getSubagentModel();

    {
        /// 创建模型 Provider 注册表并注入 AgentContext
        /// - 注册可用模型; 各会话的当前选择记录在 Session 中
        /// - 主模型兜底注册, 保证至少有一个可用模型
        auto registry = std::make_shared<agentxx::agent::ModelProviderRegistry>();
        for (const auto& [name, mc] : config->availableModels) {
            registry->registerModel(name, mc);
        }
        if (config->availableModels.empty()) {
            registry->registerModel(config->model.modelName, config->model);
            registry->setDefaultModel(config->model.modelName);
        } else if (false == config->currentModelName.empty()
                   && registry->hasModel(config->currentModelName)) {
            registry->setDefaultModel(config->currentModelName);
        }
        agentContext->modelRegistry = std::move(registry);
    }

    {
        /// 创建事件总线并注入 AgentContext
        /// - executor 取自 DeepAgent::ioCtx, 与 graph 运行在同一 io_context,
        ///   单线程协作式调度, 模块间无需加锁
        /// - 所有节点/middleware/tool 经 weak_ptr<AgentContext> 取
        /// agentContext->bus
        agentContext->bus = std::make_shared<agentxx::middleware::EventBus>(ioCtx->get_executor());
    }

    {
        /// register Node
        neograph::graph::NodeFactory::instance().register_type(
            std::string{agentxx::nodes::AgentStartCallWrapNode::defNodeType},
            [this](const std::string& name,
                   const neograph::json&,
                   const neograph::graph::NodeContext& ctx) {
                return std::make_unique<agentxx::nodes::AgentStartCallWrapNode>(name, agentContext);
            });
        neograph::graph::NodeFactory::instance().register_type(
            std::string{agentxx::nodes::MiddlewareWrapAgentEndCallNode::defNodeType},
            [this](const std::string& name,
                   const neograph::json&,
                   const neograph::graph::NodeContext& ctx) {
                return std::make_unique<agentxx::nodes::MiddlewareWrapAgentEndCallNode>(
                    name,
                    agentContext);
            });
        neograph::graph::NodeFactory::instance().register_type(
            std::string{agentxx::nodes::ModelCallWrapNode::defNodeType},
            [this](const std::string& name,
                   const neograph::json&,
                   const neograph::graph::NodeContext& ctx) {
                return std::make_unique<agentxx::nodes::ModelCallWrapNode>(name, ctx, agentContext);
            });
        neograph::graph::NodeFactory::instance().register_type(
            std::string{agentxx::nodes::ToolcallWrapNode::defNodeType},
            [this](const std::string& name,
                   const neograph::json&,
                   const neograph::graph::NodeContext& ctx) {
                return std::make_unique<agentxx::nodes::ToolcallWrapNode>(name, ctx, agentContext);
            });
    }

    /// middleware
    agentContext->middlewareHandleContext
        = std::make_shared<agentxx::middleware::MiddlewareContext>();
    std::shared_ptr<agentxx::middleware::SummarizationMiddlewareHandle> summarizationMiddleware;
    auto                                                                subagentManagerTool
        = std::make_unique<agentxx::tools::SubAgentManagerTool>("subagent_manager", agentContext);
    agentContext->subagentManagerToolPtr = subagentManagerTool.get();
    {
        {
            agentContext->permissionMiddleware
                = std::make_shared<agentxx::middleware::PermissionMiddlewareHandle>(agentContext);
            agentContext->middlewareHandleContext->handles.push_back(
                agentContext->permissionMiddleware);
        }
        {
            auto skillMiddleware = std::make_shared<agentxx::middleware::SkillMiddlewareHandle>(
                config->skillDirPaths,
                agentContext);
            // skillMiddleware->toolcalls.push_back(
            //     std::make_unique<agentxx::tools::SkillTool>(agentContext));
            agentContext->middlewareHandleContext->handles.push_back(skillMiddleware);
        }
        {
            summarizationMiddleware
                = std::make_shared<agentxx::middleware::SummarizationMiddlewareHandle>(
                    subagentManagerTool.get(),
                    agentContext);
            agentContext->middlewareHandleContext->handles.push_back(summarizationMiddleware);
        }
        {
            auto planningMiddleware
                = std::make_shared<agentxx::middleware::PlanningMiddlewareHandle>(agentContext);
            planningMiddleware->toolcalls.push_back(
                std::make_unique<agentxx::tools::WritePlanningTool>(planningMiddleware,
                                                                    agentContext));
            agentContext->middlewareHandleContext->handles.push_back(planningMiddleware);
        }

        /// Toolcall  应当作为最后一层，输出的日志才会是最终的样子
        agentContext->middlewareHandleContext->handles.push_back(
            std::make_shared<agentxx::middleware::MiddlewareWarpHandle<
                agentxx::middleware::BaseMiddlewareState>>(
                "LogPring",
                agentContext,
                (agentxx::middleware::onGraphNodeBeforeCallFunc) nullptr,
                (agentxx::middleware::onGraphNodeAfterCallFunc) nullptr,
                (agentxx::middleware::onGraphNodeBeforeCallFunc) nullptr,
                [config = agentContext->agentConfig](
                    neograph::graph::NodeInput& in) -> asio::awaitable<void> {
                    if (config->logPrintMessagesBeforeLLM) {
                        agentxx::middleware::BaseMiddlewareHandleInterface::printMessages(
                            in.state.get_messages(),
                            config->logPrintMessagesBeforeLLMWithSystemMsg);
                    }
                    co_return;
                },
                (agentxx::middleware::onGraphNodeAfterCallFunc) nullptr,
                [ctx    = std::weak_ptr<AgentContext>(agentContext),
                 config = agentContext->agentConfig](
                    neograph::graph::NodeInput& in) -> asio::awaitable<void> {
                    if (config->logPringToolcall) {
                        co_await agentxx::nodes::ToolcallWrapNode::defStdoutLogOnToolcallStart(in);
                    }
                    // 转发 toolcall 开始到会话 IO (供 TUI 等展示)
                    if (auto ctxPtr = ctx.lock()) {
                        auto session = ctxPtr->sessions->get(in.ctx.thread_id);
                        auto io      = session ? session->io : nullptr;
                        if (io) {
                            auto  messages = in.state.get_messages();
                            auto* am       = agentxx::middleware::BaseMiddlewareHandleInterface::
                                getLastAssistantToolcallMessage(messages);
                            if (am) {
                                for (const auto& tc : am->tool_calls) {
                                    io->onToolStart(tc.name, tc.id, tc.arguments);
                                }
                            }
                        }
                    }
                    co_return;
                },
                [ctx    = std::weak_ptr<AgentContext>(agentContext),
                 config = agentContext->agentConfig](
                    const neograph::graph::NodeInput& in,
                    neograph::graph::NodeOutput&      result) -> asio::awaitable<void> {
                    if (config->logPringToolcall) {
                        co_await agentxx::nodes::ToolcallWrapNode::defStdoutLogOnToolcallEnd(
                            in,
                            result);
                    }
                    // 转发 toolcall 结果到会话 IO (供 TUI 等展示)
                    if (auto ctxPtr = ctx.lock()) {
                        auto session = ctxPtr->sessions->get(in.ctx.thread_id);
                        auto io      = session ? session->io : nullptr;
                        if (io) {
                            for (const auto& w : result.writes) {
                                if (w.channel != "messages" || !w.value.is_array()) {
                                    continue;
                                }
                                for (const auto& jm : w.value) {
                                    if (jm.value("role", std::string{}) != "tool") {
                                        continue;
                                    }
                                    const std::string content = jm.value("content", std::string{});
                                    // 失败结果形如 {"error": "..."}
                                    bool hasError = false;
                                    try {
                                        auto parsed = neograph::json::parse(content);
                                        if (parsed.is_object() && parsed.contains("error")) {
                                            hasError = true;
                                        }
                                    } catch (...) {
                                        // 非 JSON 结果, 不视为错误
                                    }
                                    io->onToolEnd(jm.value("tool_name", std::string{}),
                                                  jm.value("tool_call_id", std::string{}),
                                                  content,
                                                  hasError);
                                }
                            }
                        }
                    }
                    co_return;
                }));
    }

    /// Toolcall
    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> tools{};
    {
        /// middleware tools
        for (auto& item : agentContext->middlewareHandleContext->handles) {
            if (false == item->toolcalls.empty()) {
                tools.insert(tools.end(),
                             std::make_move_iterator(item->toolcalls.begin()),
                             std::make_move_iterator(item->toolcalls.end()));
            }
        }
    }
    {
        /// MCP tool
        for (const auto& [mcpNamespace, url] : config->mcpServerUrls) {
            try {
                XX_LOGD("load mcp tool: {} | {}", mcpNamespace, url);
                auto mcpClient = std::make_shared<agentxx::server::McpClient>(
                    agentxx::server::McpClient::Config{
                        .serverUrl = url,
                        .protocolVersion
                        = std::string{agentxx::server::McpClient::kProtocol2025_11_25},
                        .toolNamespace = mcpNamespace,
                    });
                auto result = co_await mcpClient->initialize();
                if (result.has_value()) {
                    auto mcpTools = co_await mcpClient->listTools();
                    if (mcpTools.has_value()) {
                        for (auto& tool : mcpTools.value()) {
                            // TODO: 重名检查
                            tools.push_back(mcpClient->createTool(std::move(tool), agentContext));
                        }
                    } else {
                        XX_LOGE("list mcp tool error: {} | {} | {}",
                                mcpNamespace,
                                url,
                                mcpTools.error());
                    }
                } else {
                    XX_LOGE("load mcp tool error: {} | {} | {}", mcpNamespace, url, result.error());
                }
            } catch (const std::exception& e) {
                std::string errmsg = e.what();
                agentxx::util::autoConvertToUtf8(errmsg);
                XX_LOGE("[agentxx] Append mcp tool error: {} | {} | {}", mcpNamespace, url, errmsg);
            } catch (const boost::exception& e) {
                auto errmsg = boost::diagnostic_information(e);
                agentxx::util::autoConvertToUtf8(errmsg);
                XX_LOGE("[agentxx] Append mcp tool error: {} | {} | {}", mcpNamespace, url, errmsg);
            } catch (...) {
                XX_LOGE("[agentxx] Append mcp tool error: {} | {}", mcpNamespace, url);
            }
        }
    }
    {
        tools.push_back(std::make_unique<agentxx::tools::ThreadShareStoreTool>(agentContext));
        tools.push_back(std::make_unique<agentxx::tools::FileSystemListTool>(agentContext));
        tools.push_back(std::make_unique<agentxx::tools::FilesystemReadTextFileTool>(agentContext));
        tools.push_back(
            std::make_unique<agentxx::tools::FilesystemReadBinaryFileTool>(agentContext));
        tools.push_back(std::make_unique<agentxx::tools::FilesystemWriteFileTool>(agentContext));
        tools.push_back(std::make_unique<agentxx::tools::FilesystemEditTextFileTool>(agentContext));
        tools.push_back(std::make_unique<agentxx::tools::FilesystemGlobTool>(agentContext));
        tools.push_back(std::make_unique<agentxx::tools::FilesystemGrepTool>(agentContext));

        tools.push_back(std::make_unique<agentxx::tools::StringHtml2MarkdownTool>(agentContext));
        tools.push_back(std::make_unique<agentxx::tools::StringRegexpTool>(agentContext));

        tools.push_back(std::make_unique<agentxx::tools::GetCurrentDateTimeTool>(agentContext));
#if XX_IS_WIN_D || XX_IS_LINUX_D
        tools.push_back(std::make_unique<agentxx::tools::GetSystemCoreInfoTool>(agentContext));
#endif

        tools.push_back(std::make_unique<agentxx::tools::WebFetchUrlTool>(agentContext));
        tools.push_back(std::make_unique<agentxx::tools::WebFetchUrlMarkdownTool>(agentContext));
        if (config->websearchModel.has_value()) {
            // 使用模型进行网络搜索
            tools.push_back(
                std::make_unique<agentxx::tools::ModelWebSearchTool>(config->websearchModel.value(),
                                                                     agentContext));
        } else if (false == config->websearchApiUrl.empty()) {
            tools.push_back(std::make_unique<agentxx::tools::WebSearchTool>(
                config->websearchApiUrl,
                config->websearchConvertHtml2markdown,
                agentContext));
        }

        if (false == config->ragDocsPaths.empty()) {
            auto client
                = std::make_shared<agentxx::tools::EmbeddingClient>(config->model.baseUrl,
                                                                    config->model.apiKey,
                                                                    config->model.modelName);
            auto docsStore = std::make_shared<agentxx::tools::RAGSearchTool::VectorStore>(client);
            auto docs      = co_await docsStore->scanDocument(config->ragDocsPaths);
            auto docxSize  = docs.size();
            auto isAddSuccess = co_await docsStore->addDocuments(std::move(docs));
            XX_LOGD(R"_(
┏━━━━━━ RAG Embedding ━━━━━━┓
{}
┗━━━━━━ RAG Embedding ━━━━━━┛
)_",
                    isAddSuccess ? fmt::format("┣━ ✅ success: append {} docs", docxSize)
                                 : "┣━ ❌ failed");
            tools.push_back(
                std::make_unique<agentxx::tools::RAGSearchTool>(docsStore, agentContext));
        }

#if XX_IS_WIN_D
        tools.push_back(std::make_unique<agentxx::tools::UIControlKeyboardMouseTool>(agentContext));
        tools.push_back(std::make_unique<agentxx::tools::ExecuteWindowsCommandTool>(agentContext));
#elif XX_IS_LINUX_D
        tools.push_back(std::make_unique<agentxx::tools::ExecuteLinuxCommandTool>(agentContext));
        if (agentxx::util::isRunningInWSL()) {
            tools.push_back(
                std::make_unique<agentxx::tools::ExecuteWindowsCommandTool>(agentContext));
        }
#elif XX_IS_MACOS_D
        tools.push_back(std::make_unique<agentxx::tools::ExecuteLinuxCommandTool>(agentContext));
#endif

        {
            // cross-agent query tool (供主 agent/subagent 互相查询)
            tools.push_back(std::make_unique<agentxx::tools::CrossAgentQueryTool>(agentContext));
        }

        {
            // subagent
            {
                // subagent_task
                neograph::graph::NodeContext nodeContext{};
                nodeContext.instructions = "";
                nodeContext.provider     = ModelProviderRegistry::createProvider(subagentModelCfg);

                /// 复制 tool
                std::vector<neograph::Tool*> toolPtrs;
                toolPtrs.reserve(tools.size());
                for (auto& t : tools) {
                    toolPtrs.push_back(t.get());
                }
                nodeContext.tools = std::move(toolPtrs);

                const auto nodeName = std::string{"subagent_task"};

                subagentManagerTool->subAgentList.insert(std::make_pair(
                    nodeName,
                    std::make_shared<agentxx::tools::SubAgentNormalTask>(
                        nodeName,
                        R"(Create a isolation messages context sub agent to exec. (need system prompt))",
                        nodeContext)));
            }
            // {
            //   // tool_skill_search
            //   // - 复制 除了 subagent 的所有 tool/mcp tool/skill 组合上下文到
            //   // subagent 中 根据需求分析加载/使用的 tool/skill
            //   neograph::graph::NodeContext nodeContext{};
            //   nodeContext.instructions = "";
            //   nodeContext.provider =
            //       ModelProviderRegistry::createProvider(config->model);

            //   // 收集延迟加载的 tool 信息
            //   std::vector<agentxx::tools::ToolSkillSearchSubAgentTask::DelayToolInfo>
            //       delayToolInfos;
            //   for (auto &t : tools) {
            //     auto *xxTool = dynamic_cast<agentxx::tools::XXToolBase
            //     *>(t.get()); if (xxTool && xxTool->isDelayLoad) {
            //       auto def = xxTool->get_definition();
            //       delayToolInfos.push_back({xxTool->get_name(),
            //       def.description});
            //     }
            //   }

            //   // 给子 agent 提供文件系统 tool，用于搜索和读取 SKILL.md
            //   std::vector<neograph::Tool *> searchToolPtrs;
            //   searchToolPtrs.reserve(tools.size());
            //   for (auto &t : tools) {
            //     const auto &name = t->get_name();
            //     if (name == "filesystem_glob" || name == "filesystem_listfile" ||
            //         name == "filesystem_read_text_file" ||
            //         name == "filesystem_grep") {
            //       searchToolPtrs.push_back(t.get());
            //     }
            //   }
            //   nodeContext.tools = std::move(searchToolPtrs);

            //   subagentManagerTool->subAgentList.insert(std::make_pair(
            //       "tool_skill_search",
            //       std::make_shared<agentxx::tools::ToolSkillSearchSubAgentTask>(
            //           nodeContext, delayToolInfos, config->skillDirPaths,
            //           middlewareHandleContext)));
            // }

            tools.push_back(std::move(subagentManagerTool));
        }
    }
    for (const auto& tool : tools) {
        auto handle = tool->createSummarizationToolHandle();
        if (handle.has_value()) {
            summarizationMiddleware->summarizationToolHandles[tool->get_name()] = handle.value();
        }
    }

    /// === Main Agent ===
    neograph::graph::NodeContext nodeContext{};
    nodeContext.instructions = config->prompt.systemPrompt;
    nodeContext.provider     = ModelProviderRegistry::createProvider(config->model);
    // 主 agent 的 llm 节点启用运行时动态模型切换 (经 modelRegistry)
    nodeContext.extra_config = neograph::json{
        {std::string{agentxx::nodes::ModelCallWrapNode::defUseModelRegistryKey}, true},
    };

    std::vector<neograph::Tool*> toolPtrs;
    toolPtrs.reserve(tools.size());
    for (auto& t : tools) {
        toolPtrs.push_back(t.get());
    }
    nodeContext.tools = std::move(toolPtrs);

    auto store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();

    // JSON definition equivalent to the Agent::run() ReAct loop:
    //                 ------- sub_agent_task <--- toolcall/sub_agent_task
    //                 |                            |
    //                 |<---------------------------|
    //                 |                            |
    //                 v                            |
    //  __start__  -> llm ->  has_tool_calls  ->  tools
    //                               |
    //                               v
    //                            __end__
    auto graphDefinition = neograph::json{
        {"name", config->agentName},
        {
         "channels", {
                {"messages", {{"reducer", "append"}}},
                {
                    agentxx::middleware::MiddlewareContext::channel_savedGraphData,
                    {{"reducer", "overwrite"}},
                },
            }, },
        {
         "nodes", {
                {
                    "agent_start",
                    {{
                        "type",
                        agentxx::nodes::AgentStartCallWrapNode::defNodeType,
                    }},
                },
                {
                    "agent_end",
                    {{
                        "type",
                        agentxx::nodes::MiddlewareWrapAgentEndCallNode::defNodeType,
                    }},
                },
                {
                    "tools",
                    {{
                        "type",
                        agentxx::nodes::ToolcallWrapNode::defNodeType,
                    }},
                },
                {
                    "llm",
                    {{
                        "type",
                        agentxx::nodes::ModelCallWrapNode::defNodeType,
                    }},
                },
            }, },
        {
         "edges", neograph::json::array({
                {{"from", "__start__"}, {"to", "agent_start"}},
                {{"from", "agent_start"}, {"to", "llm"}},
                {
                    {"from", "llm"},
                    {"type", "conditional"},
                    {"condition", "has_tool_calls"},
                    {"routes", {{"true", "tools"}, {"false", "agent_end"}}},
                },
                {{"from", "tools"}, {"to", "llm"}},
                {{"from", "agent_end"}, {"to", "__end__"}},
            }),
         },
    };

    engine = std::move(neograph::graph::GraphEngine::compile(graphDefinition, nodeContext, store));
    {
        auto crudeTools = std::vector<std::unique_ptr<neograph::Tool>>{};
        for (auto& tool : tools) {
            crudeTools.push_back(std::move(tool));
        }
        engine->own_tools(std::move(crudeTools));
    }

    co_return;
}

void DeepAgent::selectModel(const std::string& threadId, const std::string& modelName) {
    if (false == modelName.empty() && agentContext->modelRegistry
        && agentContext->modelRegistry->hasModel(modelName)) {
        agentContext->getSession(threadId)->setModelName(modelName);
    }
}

std::string DeepAgent::getCurrentModelName(const std::string& threadId) const {
    std::string selected;
    if (auto session = agentContext->sessions->get(threadId)) {
        selected = session->getModelName();
    }
    if (agentContext->modelRegistry) {
        return agentContext->modelRegistry->resolveModelName(selected);
    }
    return agentContext->agentConfig->model.modelName;
}

asio::awaitable<DeepAgent::ConversationTurnResult> DeepAgent::runConversationTurnAsync(
    const std::string&                                      threadId,
    const std::string&                                      userInput,
    bool                                                    isFirstMsg,
    neograph::json                                          messages,
    std::shared_ptr<AgentIOBase>                            io,
    std::function<void(const neograph::graph::GraphEvent&)> eventCallback,
    InterruptCallback                                       interruptCallback,
    const std::string&                                      modelName) {
    ConversationTurnResult turnResult;
    auto                   session = agentContext->getSession(threadId);
    session->io                    = std::move(io);

    // 选择本轮使用的模型 (按会话隔离)
    selectModel(threadId, modelName);

    bool resumeInterrupt = false;
    if (false == agentContext->middlewareHandleContext->graphData.contains(threadId)) {
        // - 程序启动后，如果存在 graphData，则从 state 恢复 graphData
        // (被中断时保存的)
        auto data = engine->get_state(threadId).value_or(neograph::json{});
        if (data.is_object()
            && data.contains(agentxx::middleware::MiddlewareContext::channel_savedGraphData)) {
            // 存在被中断时保存的 graphData: 标记 resumeInterrupt, 后续跳过
            // [engine->run_stream_async], 重新处理可能未完成的中断并 [engine->resume_async]
            resumeInterrupt = true;
            agentContext->middlewareHandleContext->setGraphDataFromState(data, threadId);
        }
    }

    try {
        auto processedInput = userInput;
        agentxx::util::autoConvertToUtf8(processedInput);

        messages.push_back(neograph::json{
            {"role",    "user"        },
            {"content", processedInput},
        });
        // 创建本轮取消令牌并注入会话, 供 UI 取消执行
        auto cancelToken = std::make_shared<neograph::graph::CancelToken>();
        session->setCancelToken(cancelToken);
        auto cfg = neograph::graph::RunConfig{
            .thread_id        = threadId,
            .input            = {{"messages", messages}},
            .max_steps        = 1024,
            .stream_mode      = neograph::graph::StreamMode::ALL,
            .cancel_token     = cancelToken,
            .resume_if_exists = isFirstMsg,
        };

        // - 存在值 [neograph::graph::RunResult], 声明 optional 类型用于后续赋值
        // [nullopt]
        std::optional<neograph::graph::RunResult> result;
        if (resumeInterrupt) {
            // 程序重启恢复中断: 跳过 [engine->run_stream_async], 从恢复的 graphData
            // 重建中断结果, 直接进入中断处理循环, 重新处理可能未完成的中断并 resume
            neograph::graph::RunResult recovered;
            recovered.interrupted = true;
            recovered.interrupt_node
                = agentContext->middlewareHandleContext->getGraphDataItemValue<std::string>(
                    threadId,
                    agentxx::middleware::MiddlewareContext::graphDataKey_interruptNode);
            recovered.interrupt_value
                = agentContext->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                    threadId,
                    agentxx::middleware::MiddlewareContext::graphDataKey_interruptValue);
            result   = std::move(recovered);
            auto& im = agentContext->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                threadId,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptMessages);
            if (im.is_array()) {
                messages = im;
            }
        } else {
            result = co_await engine->run_stream_async(cfg, eventCallback);

            if (result->interrupted) {
                auto& im
                    = agentContext->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                        threadId,
                        agentxx::middleware::MiddlewareContext::graphDataKey_interruptMessages);
                if (im.is_array()) {
                    messages = im;
                }
            } else {
                messages = result->channel_raw("messages");
            }
        }

        while (result.has_value() && result->interrupted) {
            // 记录中断节点信息到 graphData, 供程序重启恢复中断时复用
            agentContext->middlewareHandleContext->setGraphDataItemValue<std::string>(
                threadId,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptNode,
                result->interrupt_node);
            agentContext->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
                threadId,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptValue,
                result->interrupt_value);

            engine->update_state(threadId, [&](neograph::graph::GraphState& state) {
                // - 本轮 graph 还没有执行完成，序列化 graphData 到 state checkpoint
                // 以防中断处理期间 程序 终止, 导致 graphData 丢失
                auto data
                    = agentContext->middlewareHandleContext->getGraphDataToState(state, threadId);
                state.overwrite(agentxx::middleware::MiddlewareContext::channel_savedGraphData,
                                data);
            });

            auto crudeResult = std::move(result);
            result           = std::nullopt;

            turnResult.interrupted = true;
            auto interruptNode     = crudeResult->interrupt_node;
            auto interruptValue    = crudeResult->interrupt_value.dump();

            auto resumeValues = neograph::json{};

            // 从 xx_savedGraphData 提取中断参数
            const auto interruptArgs = agentxx::middleware::InterruptHandleArg::listFromJson(
                agentContext->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                    threadId,
                    agentxx::middleware::MiddlewareContext::graphDataKey_interruptArgs));
            size_t argIndex = 0;
            for (const auto& interruptArg : interruptArgs) {
                ++argIndex;
                if (interruptCallback) {
                    co_await interruptCallback(interruptNode, interruptValue, interruptArg.name);
                }

                std::optional<neograph::json> interruptResult;
                {
                    assert(nullptr != agentContext->bus);
                    if (interruptArg.name == "subagent") {
                        // subagent 委派: 经总线派发给 SubagentSupervisor
                        // - 父 agent 已 checkpoint 暂停, supervisor 运行 subagent
                        // - 结果注入 interruptResult, resume 后父 graph 继续
                        auto subagentArg = interruptArg.arg;
                        auto resp = co_await agentContext->bus->request<events::ReqSubagentStart,
                                                                        events::RespSubagentResult>(
                            events::Topic::Subagent,
                            events::ReqSubagentStart{
                                .parentAgentName = agentContext->agentConfig
                                                       ? agentContext->agentConfig->agentName
                                                       : std::string{},
                                .parentThreadId  = threadId,
                                .subagentName    = subagentArg.value("subagent", std::string{}),
                                .systemPrompt = subagentArg.value("system_prompt", std::string{}),
                                .message      = subagentArg.value("message", std::string{}),
                                .resultId     = interruptArg.resultId,
                            });
                        if (resp.has_value()) {
                            interruptResult = neograph::json{resp->content};
                        }
                    } else if (interruptArg.name == "subagent_batch") {
                        // 批量 subagent 委派: 并发运行多个 subagent
                        // - interruptArg.arg 应为
                        // {"tasks":[{subagent,system_prompt,message},...]}
                        // - 结果按 resultId 注入
                        auto batchArg = interruptArg.arg;
                        auto batchReq = events::ReqSubagentBatch{
                            .parentAgentName = agentContext->agentConfig
                                                   ? agentContext->agentConfig->agentName
                                                   : std::string{},
                            .parentThreadId  = threadId,
                        };
                        if (batchArg.contains("tasks") && batchArg["tasks"].is_array()) {
                            for (const auto& t : batchArg["tasks"]) {
                                batchReq.tasks.push_back(events::SubagentBatchItem{
                                    .subagentName = t.value("subagent", std::string{}),
                                    .systemPrompt = t.value("system_prompt", std::string{}),
                                    .message      = t.value("message", std::string{}),
                                    .resultId     = t.value("result_id", std::string{}),
                                });
                            }
                        }
                        auto batchResp
                            = co_await agentContext->bus
                                  ->request<events::ReqSubagentBatch, events::RespSubagentBatch>(
                                      events::Topic::SubagentBatch,
                                      std::move(batchReq));
                        if (batchResp.has_value()) {
                            // 批量结果按 resultId 写入 resumeValues (非单个
                            // interruptResult)
                            for (const auto& r : batchResp->results) {
                                auto rid = r.resultId;
                                if (rid.empty()) {
                                    rid = interruptArg.resultId;
                                }
                                resumeValues[rid] =
                    r.hasError ? neograph::json{{"error", r.errorMessage}}
                               : neograph::json{r.content};
                            }
                        }
                    } else {
                        // HIL 中断: 经总线请求 InterruptHandler
                        auto resp
                            = co_await agentContext->bus
                                  ->request<events::ReqInterrupt, events::RespInterrupt>(
                                      events::Topic::Interrupt,
                                      events::ReqInterrupt{
                                          .agentName         = agentContext->agentConfig
                                                                   ? agentContext->agentConfig->agentName
                                                                   : std::string{},
                                          .threadId          = threadId,
                                          .interruptNode     = interruptNode,
                                          .handleName        = interruptArg.name,
                                          .interruptArgsJson = interruptArg.toJson().dump(),
                                          .resultId          = interruptArg.resultId,
                                      });
                        if (resp.has_value() && resp->handled) {
                            interruptResult = neograph::json::parse(resp->resultJson);
                        }
                    }
                }

                if (interruptResult.has_value()) {
                    auto resultId = interruptArg.resultId;
                    if (resultId.empty()) {
                        resultId = std::to_string(argIndex);
                    }
                    resumeValues[resultId] = interruptResult.value();
                }
            }

            if (false == resumeValues.empty()) {
                agentContext->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
                    threadId,
                    agentxx::middleware::MiddlewareContext::graphDataKey_interruptResult,
                    resumeValues);

                engine->update_state(threadId, [&](neograph::graph::GraphState& state) {
                    // 更新 message
                    state.overwrite("messages", std::move(messages));
                });

                result = co_await engine->resume_async(threadId, nullptr, eventCallback);

                if (result->interrupted) {
                    // 中断时 [result] 内的 messages 是旧的，应该取中断时保存的 messages
                    auto& im = agentContext->middlewareHandleContext->getGraphDataItemValue<
                        neograph::json>(
                        threadId,
                        agentxx::middleware::MiddlewareContext::graphDataKey_interruptMessages);
                    if (im.is_array()) {
                        messages = im;
                    }
                } else {
                    messages = result->channel_raw("messages");
                }
            }
        }

        engine->update_state(threadId, [&](neograph::graph::GraphState& state) {
            // 中断已经处理完成，清理 graphData
            state.remove(agentxx::middleware::MiddlewareContext::channel_savedGraphData);
        });

        turnResult.messages = std::move(messages);
    } catch (const std::exception& e) {
        turnResult.hasError     = true;
        turnResult.errorMessage = e.what();
        XX_LOGE(R"({{"error": "Agent Response failed: {}"}})", e.what());
    } catch (const boost::exception& e) {
        auto errmsg = boost::diagnostic_information(e);
        agentxx::util::autoConvertToUtf8(errmsg);
        XX_LOGE(R"({{"error": "Agent Response failed: {}"}})", errmsg);
    } catch (...) {
        turnResult.hasError     = true;
        turnResult.errorMessage = "Unknown error";
        XX_LOGE(R"({{"error": "Agent Response failed: Unknown error"}})");
    }

    co_return turnResult;
}

DeepAgent::~DeepAgent() {
    engine = nullptr;
}

neograph::graph::GraphEngine* DeepAgent::getEngine() {
    return engine.get();
}

const neograph::graph::GraphEngine* DeepAgent::getEngine() const {
    return engine.get();
}

std::shared_ptr<AgentContext> DeepAgent::getContext() {
    return agentContext;
}

asio::awaitable<std::string>
    DeepAgent::runNonStreamAsync(const std::string&                                      threadId,
                                 const std::vector<neograph::ChatMessage>&               messages,
                                 std::function<void(const neograph::graph::GraphEvent&)> callback,
                                 const std::string& modelName) {
    selectModel(threadId, modelName);
    auto inputMessages = neograph::json::array();
    for (const auto& msg : messages) {
        neograph::json msgJson;
        neograph::to_json(msgJson, msg);
        inputMessages.push_back(std::move(msgJson));
    }

    auto cfg = neograph::graph::RunConfig{
        .thread_id        = threadId,
        .input            = {{"messages", std::move(inputMessages)}},
        .resume_if_exists = false,
    };

    std::ostringstream oss;
    auto wrappedCallback = [&oss, callback](const neograph::graph::GraphEvent& event) {
        switch (event.type) {
        case neograph::graph::GraphEvent::Type::LLM_TOKEN:
            {
                std::string token;
                std::string kind = "content";
                if (event.data.is_string()) {
                    token = event.data.get<std::string>();
                } else if (event.data.is_object()) {
                    neograph::ChatStreamChunk chunk;
                    neograph::from_json(event.data, chunk);
                    token = std::move(chunk.data);
                    if (chunk.type == neograph::ChatStreamChunk::TYPE_THINKING) {
                        kind = "thinking";
                    }
                }
                if (kind == "content") {
                    oss << token;
                }
                if (callback) {
                    callback(event);
                }
            }
            break;
        default:
            if (callback) {
                callback(event);
            }
            break;
        }
    };

    co_await engine->run_stream_async(cfg, wrappedCallback);
    co_return oss.str();
}

asio::awaitable<std::string> DeepAgent::runSingleInputAsync(const std::string& threadId,
                                                            const std::string& userInput,
                                                            const std::string& systemPrompt,
                                                            const std::string& modelName) {
    std::vector<neograph::ChatMessage> messages;

    if (!systemPrompt.empty()) {
        messages.push_back(neograph::ChatMessage{
            .role    = "system",
            .content = systemPrompt,
        });
    }

    messages.push_back(neograph::ChatMessage{
        .role    = "user",
        .content = userInput,
    });

    co_return co_await runNonStreamAsync(threadId, messages, nullptr, modelName);
}

asio::awaitable<DeepAgent::SimpleRunResult>
    DeepAgent::runStreamAsync(const std::vector<neograph::ChatMessage>& messages,
                              const std::string&                        modelName) {
    auto threadId
        = fmt::format("subagent_{}", std::chrono::system_clock::now().time_since_epoch().count());
    selectModel(threadId, modelName);
    auto inputMessages = neograph::json::array();
    for (const auto& msg : messages) {
        neograph::json msgJson;
        neograph::to_json(msgJson, msg);
        inputMessages.push_back(std::move(msgJson));
    }

    neograph::graph::RunConfig cfg{
        .thread_id        = threadId,
        .input            = {{"messages", std::move(inputMessages)}},
        .resume_if_exists = false,
    };

    std::ostringstream oss;
    auto               callback = [&oss](const neograph::graph::GraphEvent& event) {
        if (event.type == neograph::graph::GraphEvent::Type::LLM_TOKEN) {
            std::string token;
            std::string kind = "content";
            if (event.data.is_string()) {
                token = event.data.get<std::string>();
            } else if (event.data.is_object()) {
                neograph::ChatStreamChunk chunk;
                neograph::from_json(event.data, chunk);
                token = std::move(chunk.data);
                if (chunk.type == neograph::ChatStreamChunk::TYPE_THINKING) {
                    kind = "thinking";
                }
            }
            if (kind == "content") {
                oss << token;
            }
        }
    };

    auto result = co_await engine->run_stream_async(cfg, callback);
    co_return SimpleRunResult{
        .content    = oss.str(),
        .fullResult = std::move(result),
    };
}

} // namespace agent
} // namespace agentxx
