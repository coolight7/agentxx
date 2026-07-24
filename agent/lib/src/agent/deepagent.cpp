#include "agentxx/agent/deepagent.h"

#include "agentxx/util/diff_util.h"
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
            [this](
                const std::string& name,
                const neograph::json&,
                const neograph::graph::NodeContext& ctx
            ) {
                return std::make_unique<agentxx::nodes::AgentStartCallWrapNode>(name, agentContext);
            }
        );
        neograph::graph::NodeFactory::instance().register_type(
            std::string{agentxx::nodes::MiddlewareWrapAgentEndCallNode::defNodeType},
            [this](
                const std::string& name,
                const neograph::json&,
                const neograph::graph::NodeContext& ctx
            ) {
                return std::make_unique<agentxx::nodes::MiddlewareWrapAgentEndCallNode>(
                    name,
                    agentContext
                );
            }
        );
        neograph::graph::NodeFactory::instance().register_type(
            std::string{agentxx::nodes::ModelCallWrapNode::defNodeType},
            [this](
                const std::string& name,
                const neograph::json&,
                const neograph::graph::NodeContext& ctx
            ) {
                return std::make_unique<agentxx::nodes::ModelCallWrapNode>(name, ctx, agentContext);
            }
        );
        neograph::graph::NodeFactory::instance().register_type(
            std::string{agentxx::nodes::ToolcallWrapNode::defNodeType},
            [this](
                const std::string& name,
                const neograph::json&,
                const neograph::graph::NodeContext& ctx
            ) {
                return std::make_unique<agentxx::nodes::ToolcallWrapNode>(name, ctx, agentContext);
            }
        );
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
                agentContext->permissionMiddleware
            );
        }
        {
            auto skillMiddleware = std::make_shared<agentxx::middleware::SkillMiddlewareHandle>(
                config->skillDirPaths,
                agentContext
            );
            // skillMiddleware->toolcalls.push_back(
            //     std::make_unique<agentxx::tools::SkillTool>(agentContext));
            agentContext->middlewareHandleContext->handles.push_back(skillMiddleware);
        }
        {
            summarizationMiddleware
                = std::make_shared<agentxx::middleware::SummarizationMiddlewareHandle>(
                    subagentManagerTool.get(),
                    agentContext
                );
            agentContext->middlewareHandleContext->handles.push_back(summarizationMiddleware);
        }
        {
            auto planningMiddleware
                = std::make_shared<agentxx::middleware::PlanningMiddlewareHandle>(agentContext);
            planningMiddleware->toolcalls.push_back(
                std::make_unique<agentxx::tools::WritePlanningTool>(
                    planningMiddleware,
                    agentContext
                )
            );
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
                [config = agentContext->agentConfig](neograph::graph::NodeInput& in
                ) -> asio::awaitable<void> {
                    if (config->logPrintMessagesBeforeLLM) {
                        agentxx::middleware::BaseMiddlewareHandleInterface::printMessages(
                            in.state.get_messages(),
                            config->logPrintMessagesBeforeLLMWithSystemMsg
                        );
                    }
                    co_return;
                },
                (agentxx::middleware::onGraphNodeAfterCallFunc) nullptr,
                [ctx    = std::weak_ptr<AgentContext>(agentContext),
                 config = agentContext->agentConfig](neograph::graph::NodeInput& in
                ) -> asio::awaitable<void> {
                    if (config->logPringToolcall) {
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
                [ctx    = std::weak_ptr<AgentContext>(agentContext),
                 config = agentContext->agentConfig](
                    const neograph::graph::NodeInput& in,
                    neograph::graph::NodeOutput&      result
                ) -> asio::awaitable<void> {
                    if (config->logPringToolcall) {
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
    }

    /// Toolcall
    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> tools{};
    {
        /// middleware tools
        for (auto& item : agentContext->middlewareHandleContext->handles) {
            if (false == item->toolcalls.empty()) {
                tools.insert(
                    tools.end(),
                    std::make_move_iterator(item->toolcalls.begin()),
                    std::make_move_iterator(item->toolcalls.end())
                );
            }
        }
    }
    {
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
                                tools.push_back(mcpClient->createTool(std::move(tool), agentContext)
                                );
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
                        XX_LOGE(
                            "load mcp tool error: {} | {} | {}",
                            mcpNamespace,
                            url,
                            result.error()
                        );
                    }
                    co_return;
                },
                [&](std::string errmsg) -> asio::awaitable<void> {
                    XX_LOGE(
                        "[agentxx] Append mcp tool error: {} | {} | {}",
                        mcpNamespace,
                        url,
                        errmsg
                    );
                    co_return;
                }
            );
        }
    }
    {
        tools.push_back(std::make_unique<agentxx::tools::ThreadShareStoreTool>(agentContext));
        tools.push_back(std::make_unique<agentxx::tools::FileSystemListTool>(agentContext));
        tools.push_back(std::make_unique<agentxx::tools::FilesystemReadTextFileTool>(agentContext));
        tools.push_back(std::make_unique<agentxx::tools::FilesystemReadBinaryFileTool>(agentContext)
        );
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

        if (false == config->ragDocsPaths.empty()) {
            auto client = std::make_shared<agentxx::tools::EmbeddingClient>(
                config->model.baseUrl,
                config->model.apiKey,
                config->model.modelName
            );
            auto docsStore = std::make_shared<agentxx::tools::RAGSearchTool::VectorStore>(client);
            auto docs      = co_await docsStore->scanDocument(config->ragDocsPaths);
            auto docxSize  = docs.size();
            auto isAddSuccess = co_await docsStore->addDocuments(std::move(docs));
            XX_LOGD(
                R"_(
┏━━━━━━ RAG Embedding ━━━━━━┓
{}
┗━━━━━━ RAG Embedding ━━━━━━┛
)_",
                isAddSuccess ? fmt::format("┣━ ✅ success: append {} docs", docxSize)
                             : "┣━ ❌ failed"
            );
            tools.push_back(std::make_unique<agentxx::tools::RAGSearchTool>(docsStore, agentContext)
            );
        }

#if XX_IS_WIN_D
        tools.push_back(std::make_unique<agentxx::tools::UIControlKeyboardMouseTool>(agentContext));
        tools.push_back(std::make_unique<agentxx::tools::ExecuteWindowsCommandTool>(agentContext));
#elif XX_IS_LINUX_D
        tools.push_back(std::make_unique<agentxx::tools::ExecuteLinuxCommandTool>(agentContext));
        if (agentxx::util::isRunningInWSL()) {
            tools.push_back(std::make_unique<agentxx::tools::ExecuteWindowsCommandTool>(agentContext
            ));
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
                        nodeContext
                    )
                ));
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

    // clang-format off
    auto graphDefinition = neograph::json{
        {"name", config->agentName},
        {
            "channels", {
                {"messages", {{"reducer", "append"}}},
                {
                    agentxx::middleware::MiddlewareContext::channel_savedGraphData,
                    {{"reducer", "overwrite"}},
                },
            }, 
        },
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
            }, 
        },
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
    // clang-format on

    engine = std::move(neograph::graph::GraphEngine::compile(graphDefinition, nodeContext, store));
    assert(nullptr != engine);
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
    const std::string&           threadId,
    const std::string&           userInput,
    bool                         isFirstMsg,
    std::shared_ptr<AgentIOBase> io,
    const std::string&           modelName
) {
    ConversationTurnResult turnResult;
    auto                   session = agentContext->getSession(threadId);
    if (!session->bus) {
        session->bus
            = std::make_shared<agentxx::middleware::EventBus>(co_await asio::this_coro::executor);
    }
    if (io) {
        io->registerOnBus(session->bus);
    }
    session->io = std::move(io);

    auto ioPtr = session->io;

    auto emitDelta = [&](Delta delta) {
        delta.seq = ++session->deltaSeq;
        if (ioPtr) {
            ioPtr->onDelta(delta);
        }
    };

    emitDelta(Delta{.type = Delta::Type::TurnStart});

    selectModel(threadId, modelName);

    bool resumeInterrupt = false;
    if (false == agentContext->middlewareHandleContext->graphData.contains(threadId)) {
        auto data = engine->get_state(threadId).value_or(neograph::json{});
        if (data.is_object()
            && data.contains(agentxx::middleware::MiddlewareContext::channel_savedGraphData)) {
            resumeInterrupt = true;
            agentContext->middlewareHandleContext->setGraphDataFromState(data, threadId);
        }
    }

    co_await agentxx::util::catchErrorAsync<void>(
        [&]() -> asio::awaitable<void> {
            auto processedInput = userInput;
            agentxx::util::autoConvertToUtf8(processedInput);

            auto userMsgJson = neograph::json{
                {"role",    "user"        },
                {"content", processedInput},
            };
            session->appendHistory(userMsgJson);
            session->llmMessages.push_back(std::move(userMsgJson));

            auto cancelToken = std::make_shared<neograph::graph::CancelToken>();
            session->setCancelToken(cancelToken);

            auto internalEventCallback = [session,
                                          emitDelta](const neograph::graph::GraphEvent& event) {
                using T = neograph::graph::GraphEvent::Type;
                switch (event.type) {
                    case T::LLM_TOKEN: {
                        std::string token;
                        bool        isThinking = false;
                        if (event.data.is_string()) {
                            token = event.data.get<std::string>();
                        } else if (event.data.is_object()) {
                            neograph::ChatStreamChunk chunk;
                            neograph::from_json(event.data, chunk);
                            token      = std::move(chunk.data);
                            isThinking = (chunk.type == neograph::ChatStreamChunk::TYPE_THINKING);
                        }
                        emitDelta(Delta{
                            .type
                            = isThinking ? Delta::Type::ThinkingToken : Delta::Type::TextToken,
                            .text = std::move(token),
                        });
                    } break;
                    case T::CHANNEL_WRITE: {
                        auto chan  = event.data.value("channel", std::string{});
                        auto value = event.data.value("value", neograph::json{});
                        if (chan != "messages" || !value.is_array()) {
                            break;
                        }
                        for (const auto& jm : value) {
                            auto role = jm.value("role", std::string{});
                            if (role == "assistant" && jm.contains("tool_calls")) {
                                auto msgId = session->appendHistory(jm);
                                for (const auto& tc : jm["tool_calls"]) {
                                    emitDelta(Delta{
                                        .type       = Delta::Type::ToolStart,
                                        .msgId      = msgId,
                                        .toolName   = tc.value("name", std::string{}),
                                        .toolCallId = tc.value("id", std::string{}),
                                        .arguments  = tc.value("arguments", std::string{}),
                                    });
                                }
                            } else if (role == "tool") {
                                auto content  = jm.value("content", std::string{});
                                bool hasError = false;
                                try {
                                    auto parsed = neograph::json::parse(content);
                                    hasError    = parsed.is_object() && parsed.contains("error");
                                } catch (...) {
                                }
                                auto toolName   = jm.value("tool_name", std::string{});
                                auto toolCallId = jm.value("tool_call_id", std::string{});
                                // 文本编辑工具: 向 fullHistory 额外记录 git diff 对比信息
                                // (仅写入 fullHistory 副本, 不影响 LLM 上下文)
                                auto historyMsg = jm;
                                if (false == hasError && toolName == "filesystem_edit_text_file") {
                                    for (auto it = session->fullHistory.rbegin();
                                         it != session->fullHistory.rend();
                                         ++it) {
                                        const auto& hd = it->data;
                                        if (hd.value("role", std::string{}) != "assistant"
                                            || !hd.contains("tool_calls")) {
                                            continue;
                                        }
                                        bool foundArgs = false;
                                        for (const auto& tc : hd["tool_calls"]) {
                                            if (tc.value("id", std::string{}) != toolCallId) {
                                                continue;
                                            }
                                            foundArgs = true;
                                            try {
                                                auto args = neograph::json::parse(
                                                    tc.value("arguments", std::string{})
                                                );
                                                historyMsg["diff"] = agentxx::util::makeUnifiedDiff(
                                                    args.value("old_str", std::string{}),
                                                    args.value("new_str", std::string{}),
                                                    args.value("path", std::string{})
                                                );
                                            } catch (...) {
                                            }
                                            break;
                                        }
                                        if (foundArgs) {
                                            break;
                                        }
                                    }
                                }
                                session->appendHistory(historyMsg);
                                emitDelta(Delta{
                                    .type       = Delta::Type::ToolEnd,
                                    .toolName   = toolName,
                                    .toolCallId = toolCallId,
                                    .result     = content,
                                    .hasError   = hasError,
                                });
                            } else if (role == "assistant") {
                                session->appendHistory(jm);
                            }
                        }
                    } break;
                    default:
                        break;
                }
            };

            auto eventCallback = agentxx::middleware::EventBridge::make(
                agentContext->agentConfig->agentName,
                threadId,
                agentContext,
                std::move(internalEventCallback)
            );

            auto cfg = neograph::graph::RunConfig{
                .thread_id        = threadId,
                .input            = {{"messages", session->llmMessages}},
                .max_steps        = 1024,
                .stream_mode      = neograph::graph::StreamMode::ALL,
                .cancel_token     = cancelToken,
                .resume_if_exists = isFirstMsg,
            };

            std::optional<neograph::graph::RunResult> result;
            if (resumeInterrupt) {
                neograph::graph::RunResult recovered;
                recovered.interrupted = true;
                recovered.interrupt_node
                    = agentContext->middlewareHandleContext->getGraphDataItemValue<std::string>(
                        threadId,
                        agentxx::middleware::MiddlewareContext::graphDataKey_interruptNode
                    );
                recovered.interrupt_value
                    = agentContext->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                        threadId,
                        agentxx::middleware::MiddlewareContext::graphDataKey_interruptValue
                    );
                result = std::move(recovered);
            } else {
                result = co_await engine->run_stream_async(cfg, eventCallback);

                if (!result->interrupted) {
                    session->llmMessages = result->channel_raw("messages");
                }
            }

            while (result.has_value() && result->interrupted) {
                agentContext->middlewareHandleContext->setGraphDataItemValue<std::string>(
                    threadId,
                    agentxx::middleware::MiddlewareContext::graphDataKey_interruptNode,
                    result->interrupt_node
                );
                agentContext->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
                    threadId,
                    agentxx::middleware::MiddlewareContext::graphDataKey_interruptValue,
                    result->interrupt_value
                );

                engine->update_state(threadId, [&](neograph::graph::GraphState& state) {
                    auto data = agentContext->middlewareHandleContext->getGraphDataToState(
                        state,
                        threadId
                    );
                    state.overwrite(
                        agentxx::middleware::MiddlewareContext::channel_savedGraphData,
                        data
                    );
                });

                auto crudeResult = std::move(result);
                result           = std::nullopt;

                turnResult.interrupted = true;
                auto interruptNode     = crudeResult->interrupt_node;
                auto interruptValue    = crudeResult->interrupt_value.dump();

                auto resumeValues = neograph::json{};

                const auto interruptArgs = agentxx::middleware::InterruptHandleArg::listFromJson(
                    agentContext->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                        threadId,
                        agentxx::middleware::MiddlewareContext::graphDataKey_interruptArgs
                    )
                );
                size_t argIndex = 0;
                for (const auto& interruptArg : interruptArgs) {
                    ++argIndex;

                    std::optional<neograph::json> interruptResult;
                    {
                        if (interruptArg.name == "subagent") {
                            auto subagentArg = interruptArg.arg;
                            auto resp        = co_await agentContext->bus->request<
                                       events::ReqSubagentStart,
                                       events::RespSubagentResult>(
                                events::Topic::Subagent,
                                events::ReqSubagentStart{
                                    .parentAgentName = agentContext->agentConfig
                                                           ? agentContext->agentConfig->agentName
                                                           : std::string{},
                                    .parentThreadId  = threadId,
                                    .subagentName    = subagentArg.value("subagent", std::string{}),
                                    .systemPrompt
                                    = subagentArg.value("system_prompt", std::string{}),
                                    .message  = subagentArg.value("message", std::string{}),
                                    .resultId = interruptArg.resultId,
                                }
                            );
                            if (resp.has_value()) {
                                interruptResult = neograph::json{resp->content};
                            }
                        } else if (interruptArg.name == "subagent_batch") {
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
                            auto batchResp = co_await agentContext->bus->request<
                                events::ReqSubagentBatch,
                                events::RespSubagentBatch>(
                                events::Topic::SubagentBatch,
                                std::move(batchReq)
                            );
                            if (batchResp.has_value()) {
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
                            auto sess = agentContext->sessions->get(threadId);
                            if (sess && sess->bus) {
                                auto resp
                                    = co_await sess->bus
                                          ->request<events::ReqInterrupt, events::RespInterrupt>(
                                              events::Topic::Interrupt,
                                              events::ReqInterrupt{
                                                  .agentName
                                                  = agentContext->agentConfig
                                                        ? agentContext->agentConfig->agentName
                                                        : std::string{},
                                                  .threadId          = threadId,
                                                  .interruptNode     = interruptNode,
                                                  .interruptValue    = interruptValue,
                                                  .handleName        = interruptArg.name,
                                                  .interruptArgsJson = interruptArg.toJson().dump(),
                                                  .resultId          = interruptArg.resultId,
                                              }
                                          );
                                if (resp.has_value() && resp->handled) {
                                    interruptResult = neograph::json::parse(resp->resultJson);
                                }
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
                        resumeValues
                    );

                    engine->update_state(threadId, [&](neograph::graph::GraphState& state) {
                        state.overwrite("messages", session->llmMessages);
                    });

                    result = co_await engine->resume_async(threadId, nullptr, eventCallback);

                    if (!result->interrupted) {
                        session->llmMessages = result->channel_raw("messages");
                    }
                }
            }

            engine->update_state(threadId, [&](neograph::graph::GraphState& state) {
                state.remove(agentxx::middleware::MiddlewareContext::channel_savedGraphData);
            });
            co_return;
        },
        [&](std::string errmsg) -> asio::awaitable<void> {
            turnResult.hasError = true;
            XX_LOGE(R"({{"error": "Agent Response failed: {}"}})", errmsg);
            turnResult.errorMessage = std::move(errmsg);
            co_return;
        }
    );

    emitDelta(Delta{
        .type         = Delta::Type::TurnEnd,
        .historyCount = session->chainHash.count(),
        .tailHash     = session->chainHash.tailHex(),
    });

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

asio::awaitable<std::string> DeepAgent::runNonStreamAsync(
    const std::string&                                      threadId,
    const std::vector<neograph::ChatMessage>&               messages,
    std::function<void(const neograph::graph::GraphEvent&)> callback,
    const std::string&                                      modelName
) {
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
            case neograph::graph::GraphEvent::Type::LLM_TOKEN: {
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
            } break;
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

asio::awaitable<std::string> DeepAgent::runSingleInputAsync(
    const std::string& threadId,
    const std::string& userInput,
    const std::string& systemPrompt,
    const std::string& modelName
) {
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

asio::awaitable<DeepAgent::SimpleRunResult> DeepAgent::runStreamAsync(
    const std::vector<neograph::ChatMessage>& messages,
    const std::string&                        modelName
) {
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
