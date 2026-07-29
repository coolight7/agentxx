#include "agentxx/agent/base_agent.h"

#include "agentxx/middlewares/summarization.h"
#include "agentxx/util/diff_util.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "neograph/graph/compiler.h"
#include "neograph/graph/validator.h"
#include "neograph/llm/openai_provider.h"
#include <cassert>
#include <chrono>
#include <sstream>
#include <unordered_set>

namespace agentxx {
namespace agent {

BaseAgent::BaseAgent(std::shared_ptr<agentxx::agent::AgentConfig> in_config) {
    ioCtx                     = std::make_shared<asio::io_context>();
    agentContext              = std::make_shared<AgentContext>();
    agentContext->agentConfig = in_config;
    assert(nullptr != in_config);
    assert(in_config->model.isValid());
}

asio::awaitable<void> BaseAgent::init() {
#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
    XX_LOGD("Enable asio/async file RW");
#else
    XX_LOGD("Disable asio/async file RW");
#endif

    setupModelRegistry();
    setupEventBus();

    agentContext->middlewareHandleContext
        = std::make_shared<agentxx::middleware::MiddlewareContext>();

    {
        auto registry = std::make_shared<neograph::graph::GraphRegistry>();
        registerNodes(*registry);
        graphRegistry = std::move(registry);
    }

    co_await setupMiddleware();

    auto tools = co_await createTools();

    collectMiddlewareTools(tools);

    setupSummarizationHandles(tools);

    auto graphDef = buildGraphDefinition();

    auto config = agentContext->agentConfig;

    neograph::graph::NodeContext nodeContext{};
    nodeContext.instructions = config->prompt.systemPrompt;
    nodeContext.provider     = ModelProviderRegistry::createProvider(config->model);
    nodeContext.extra_config = neograph::json{
        {std::string{agentxx::nodes::ModelCallWrapNode::defUseModelRegistryKey}, true},
    };

    std::vector<neograph::Tool*> toolPtrs;
    toolPtrs.reserve(tools.size());
    for (auto& t : tools) {
        toolPtrs.push_back(t.get());
    }
    nodeContext.tools = std::move(toolPtrs);

    auto topology = neograph::graph::GraphCompiler::parse(graphDef, *graphRegistry);
    auto validated
        = neograph::graph::GraphValidator::require_valid(std::move(topology), *graphRegistry);

    neograph::graph::EngineConfig engineConfig;
    engineConfig.node_context     = std::move(nodeContext);
    engineConfig.checkpoint_store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();

    neograph::graph::EngineResources resources;
    resources.registry = graphRegistry;

    engine = neograph::graph::GraphEngine::link(
        std::move(validated),
        std::move(engineConfig),
        std::move(resources)
    );
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

void BaseAgent::setupModelRegistry() {
    auto config   = agentContext->agentConfig;
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

void BaseAgent::setupEventBus() {
    agentContext->bus = std::make_shared<agentxx::middleware::EventBus>(ioCtx->get_executor());
}

void BaseAgent::registerNodes(neograph::graph::GraphRegistry& registry) {
    auto ctx = agentContext;
    registry.register_type(
        std::string{agentxx::nodes::AgentStartCallWrapNode::defNodeType},
        [ctx](const std::string& name, const neograph::json&, const neograph::graph::NodeContext&) {
            return std::make_unique<agentxx::nodes::AgentStartCallWrapNode>(name, ctx);
        }
    );
    registry.register_type(
        std::string{agentxx::nodes::MiddlewareWrapAgentEndCallNode::defNodeType},
        [ctx](const std::string& name, const neograph::json&, const neograph::graph::NodeContext&) {
            return std::make_unique<agentxx::nodes::MiddlewareWrapAgentEndCallNode>(name, ctx);
        }
    );
    registry.register_type(
        std::string{agentxx::nodes::ModelCallWrapNode::defNodeType},
        [ctx](
            const std::string& name,
            const neograph::json&,
            const neograph::graph::NodeContext& nodeCtx
        ) {
            return std::make_unique<agentxx::nodes::ModelCallWrapNode>(name, nodeCtx, ctx);
        }
    );
    registry.register_type(
        std::string{agentxx::nodes::ToolcallWrapNode::defNodeType},
        [ctx](
            const std::string& name,
            const neograph::json&,
            const neograph::graph::NodeContext& nodeCtx
        ) {
            return std::make_unique<agentxx::nodes::ToolcallWrapNode>(name, nodeCtx, ctx);
        }
    );
}

neograph::json BaseAgent::buildGraphDefinition() {
    auto config = agentContext->agentConfig;

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
    return neograph::json{
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
}

asio::awaitable<void> BaseAgent::setupMiddleware() {
    co_return;
}

asio::awaitable<std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>> BaseAgent::createTools() {
    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> tools{};
    tools.push_back(std::make_unique<agentxx::tools::ThreadShareStoreTool>(agentContext));
    tools.push_back(std::make_unique<agentxx::tools::GetCurrentDateTimeTool>(agentContext));
    co_return tools;
}

void BaseAgent::collectMiddlewareTools(
    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>& tools
) {
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

void BaseAgent::setupSummarizationHandles(
    const std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>& tools
) {
    for (auto& handle : agentContext->middlewareHandleContext->handles) {
        auto* summarization
            = dynamic_cast<agentxx::middleware::SummarizationMiddlewareHandle*>(handle.get());
        if (nullptr == summarization) {
            continue;
        }
        for (const auto& tool : tools) {
            auto toolHandle = tool->createSummarizationToolHandle();
            if (toolHandle.has_value()) {
                summarization->summarizationToolHandles[tool->get_name()] = toolHandle.value();
            }
        }
        break;
    }
}

void BaseAgent::selectModel(std::string_view threadId, std::string_view modelName) {
    if (false == modelName.empty() && agentContext->modelRegistry
        && agentContext->modelRegistry->hasModel(modelName)) {
        agentContext->getSession(threadId)->setModelName(modelName);
    }
}

void BaseAgent::collectAppendComponentInfo(std::vector<
                                           AppendComponentNotification>& /*notifications*/) {
    // BaseAgent: 空实现，无需收集信息
}

std::string BaseAgent::getCurrentModelName(std::string_view threadId) const {
    std::string selected;
    if (auto session = agentContext->sessions->get(threadId)) {
        selected = session->getModelName();
    }
    if (agentContext->modelRegistry) {
        return agentContext->modelRegistry->resolveModelName(selected);
    }
    return agentContext->agentConfig->model.modelName;
}

asio::awaitable<BaseAgent::ConversationTurnResult> BaseAgent::runConversationTurnAsync(
    std::string_view             threadId,
    std::string_view             userInput,
    bool                         isFirstMsg,
    std::shared_ptr<AgentIOBase> io,
    std::string_view             modelName
) {
    ConversationTurnResult turnResult;
    auto                   session = agentContext->getSession(threadId);
    session->bindIoThread();
    session->assertIoThread();
    if (!session->bus) {
        session->bus
            = std::make_shared<agentxx::middleware::EventBus>(co_await asio::this_coro::executor);
    }
    if (io) {
        io->registerOnBus(session->bus);
    }
    session->io = std::move(io);

    auto ioPtr = session->io;

    // 记录轮次开始时间 (毫秒)
    const int32_t start_time_ms
        = static_cast<int32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch()
        )
                                   .count());

    auto emitDelta = [&](Delta delta) {
        delta.seq = session->deltaSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (ioPtr) {
            ioPtr->onDelta(delta);
        }
    };

    emitDelta(Delta{.type = Delta::Type::TurnStart});

    selectModel(threadId, modelName);

    bool resumeInterrupt = false;
    if (false == agentContext->middlewareHandleContext->graphData.contains(threadId)) {
        auto data = engine->get_state(std::string{threadId}).value_or(neograph::json{});
        if (data.is_object()
            && data.contains(agentxx::middleware::MiddlewareContext::channel_savedGraphData)) {
            resumeInterrupt = true;
            agentContext->middlewareHandleContext->setGraphDataFromState(data, threadId);
        }
    }

    auto processedInput = std::string{userInput};
    agentxx::util::autoConvertToUtf8(processedInput);

    auto userMsgJson = neograph::json{
        {"role",    "user"        },
        {"content", processedInput},
    };
    session->appendHistory(userMsgJson);
    session->llmMessages.push_back(std::move(userMsgJson));

    auto cancelToken = std::make_shared<neograph::graph::CancelToken>();
    session->setCancelToken(cancelToken);

    auto internalEventCallback = [session, emitDelta, ioPtr, start_time_ms](
                                     const neograph::graph::GraphEvent& event
                                 ) {
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
                    .type = isThinking ? Delta::Type::ThinkingToken : Delta::Type::TextToken,
                    .text = std::move(token),
                });
            } break;
            case T::CHANNEL_WRITE: {
                auto chan  = event.data.value("channel", std::string{});
                auto value = event.data.value("value", neograph::json{});
                if (chan != "messages" || !value.is_array()) {
                    break;
                }
                bool hasLLMOutput = false;
                for (const auto& jm : value) {
                    auto role = jm.value("role", std::string{});
                    if (role == "assistant" && jm.contains("tool_calls")) {
                        hasLLMOutput = true;
                        auto msgId   = session->appendHistory(jm);
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
                        if (!toolCallId.empty()) {
                            continue;
                        }
                        auto historyMsg = jm;
                        if (false == hasError && toolName == "filesystem_edit_text_file") {
                            // 生成 diff 记录
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
                        hasLLMOutput = true;
                        session->appendHistory(jm);
                    }
                }
                // llm node 执行完成，推送上下文统计更新
                if (hasLLMOutput && ioPtr && session->contextStats) {
                    ioPtr->sendToPeer(WireContextStats{
                        session->contextStats->contextTokens.load(std::memory_order_relaxed),
                        session->contextStats->maxContextTokens.load(std::memory_order_relaxed),
                    });
                }
            } break;
            case T::NODE_START: {
                emitDelta(Delta{
                    .type     = Delta::Type::NodeStart,
                    .nodeName = event.node_name,
                });
            } break;
            case T::NODE_END: {
                // 计算轮次持续时间
                const int32_t end_time_ms
                    = static_cast<int32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch()
                    )
                                               .count());
                const int32_t duration_ms = end_time_ms - start_time_ms;
                emitDelta(Delta{
                    .type        = Delta::Type::NodeEnd,
                    .nodeName    = event.node_name,
                    .startTimeMs = start_time_ms,
                    .durationMs  = duration_ms,
                });
            } break;
            default:
                break;
        }
    };

    auto eventCallback = agentxx::middleware::EventBridge::make(
        agentContext->agentConfig->agentName,
        std::string{threadId},
        agentContext,
        std::move(internalEventCallback)
    );
    auto cfg = neograph::graph::RunConfig{
        .thread_id        = std::string{threadId},
        .input            = {{"messages", session->llmMessages}},
        .max_steps        = 1024,
        .stream_mode      = neograph::graph::StreamMode::ALL,
        .cancel_token     = cancelToken,
        .resume_if_exists = isFirstMsg,
    };

    co_await agentxx::util::catchErrorAsync<void>(
        [&]() -> asio::awaitable<void> {
            try {
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
                        = agentContext->middlewareHandleContext->getGraphDataItemValue<
                            neograph::json>(
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

                    engine->update_state(
                        std::string{threadId},
                        [&](neograph::graph::GraphState& state) {
                            auto data = agentContext->middlewareHandleContext->getGraphDataToState(
                                state,
                                threadId
                            );
                            state.overwrite(
                                agentxx::middleware::MiddlewareContext::channel_savedGraphData,
                                data
                            );
                        }
                    );

                    auto crudeResult = std::move(result);
                    result           = std::nullopt;

                    turnResult.interrupted = true;
                    auto interruptNode     = crudeResult->interrupt_node;
                    auto interruptValue    = crudeResult->interrupt_value.dump();

                    auto resumeValues = neograph::json{};

                    const auto interruptArgs
                        = agentxx::middleware::InterruptHandleArg::listFromJson(
                            agentContext->middlewareHandleContext->getGraphDataItemValue<
                                neograph::json>(
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
                                        .parentAgentName
                                        = agentContext->agentConfig
                                              ? agentContext->agentConfig->agentName
                                              : std::string{},
                                        .parentThreadId = std::string{threadId},
                                        .subagentName
                                        = subagentArg.value("subagent", std::string{}),
                                        .systemPrompt
                                        = subagentArg.value("system_prompt", std::string{}),
                                        .message  = subagentArg.value("message", std::string{}),
                                        .resultId = interruptArg.resultId,
                                    }
                                );
                                if (resp.has_value()) {
                                    interruptResult = neograph::json{std::string{resp->content}};
                                }
                            } else if (interruptArg.name == "subagent_batch") {
                                auto batchArg = interruptArg.arg;
                                auto batchReq = events::ReqSubagentBatch{
                                    .parentAgentName = agentContext->agentConfig
                                                           ? agentContext->agentConfig->agentName
                                                           : std::string{},
                                    .parentThreadId  = std::string{threadId},
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
                    r.hasError ? neograph::json{{"error", std::string{r.errorMessage}}}
                               : neograph::json{std::string{r.content}};
                                    }
                                }
                            } else {
                                auto sess = agentContext->sessions->get(threadId);
                                if (sess && sess->bus) {
                                    auto resp = co_await sess->bus->request<
                                        events::ReqInterrupt,
                                        events::RespInterrupt>(
                                        events::Topic::Interrupt,
                                        events::ReqInterrupt{
                                            .agentName         = agentContext->agentConfig
                                                                     ? agentContext->agentConfig->agentName
                                                                     : std::string{},
                                            .threadId          = std::string{threadId},
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

                    if (false != resumeValues.empty()) {
                        agentContext->middlewareHandleContext->setGraphDataItemValue<
                            neograph::json>(
                            threadId,
                            agentxx::middleware::MiddlewareContext::graphDataKey_interruptResult,
                            resumeValues
                        );

                        engine->update_state(
                            std::string{threadId},
                            [&](neograph::graph::GraphState& state) {
                                state.overwrite("messages", session->llmMessages);
                            }
                        );

                        result = co_await engine
                                     ->resume_async(std::string{threadId}, nullptr, eventCallback);

                        if (!result->interrupted) {
                            session->llmMessages = result->channel_raw("messages");
                        }
                    }
                }
            } catch (const neograph::graph::CancelledException&) {
                auto state = engine->get_state(std::string{threadId});
                if (state.has_value() && state->is_object() && state->contains("messages")) {
                    auto msgs = (*state)["messages"];
                    if (msgs.is_array() && !msgs.empty()) {
                        session->llmMessages = std::move(msgs);
                    }
                }
            }

            co_return;
        },
        [&](std::string errmsg) -> asio::awaitable<void> {
            XX_LOGE(R"({{"error": "Agent Response failed: {}"}})", errmsg);
            turnResult.hasError     = true;
            turnResult.errorMessage = std::move(errmsg);
            co_return;
        }
    );

    engine->update_state(std::string{threadId}, [&](neograph::graph::GraphState& state) {
        state.remove(agentxx::middleware::MiddlewareContext::channel_savedGraphData);
    });

    // 计算轮次持续时间
    const int32_t end_time_ms
        = static_cast<int32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch()
        )
                                   .count());
    const int32_t duration_ms = end_time_ms - start_time_ms;

    // 发送 TurnEnd Delta，包含时长统计
    emitDelta(Delta{
        .type         = Delta::Type::TurnEnd,
        .historyCount = session->chainHash.count(),
        .tailHash     = session->chainHash.tailHex(),
        .startTimeMs  = start_time_ms,
        .durationMs   = duration_ms,
    });

    // 通过 ioPtr 发送 TurnResult (供远程模式使用)
    if (ioPtr) {
        auto resultMsg = WireTurnResult{
            .threadId     = std::string{threadId},
            .hasError     = turnResult.hasError,
            .errorMessage = turnResult.errorMessage,
            .interrupted  = turnResult.interrupted,
            .startTimeMs  = start_time_ms,
            .durationMs   = duration_ms,
        };
        ioPtr->sendToPeer(std::move(resultMsg));
    }

    co_return turnResult;
}

BaseAgent::~BaseAgent() {
    engine = nullptr;
}

neograph::graph::GraphEngine* BaseAgent::getEngine() {
    return engine.get();
}

const neograph::graph::GraphEngine* BaseAgent::getEngine() const {
    return engine.get();
}

std::shared_ptr<AgentContext> BaseAgent::getContext() {
    return agentContext;
}

asio::awaitable<std::string> BaseAgent::runNonStreamAsync(
    std::string_view                                        threadId,
    const std::vector<neograph::ChatMessage>&               messages,
    std::function<void(const neograph::graph::GraphEvent&)> callback,
    std::string_view                                        modelName
) {
    selectModel(threadId, modelName);
    auto inputMessages = neograph::json::array();
    for (const auto& msg : messages) {
        neograph::json msgJson;
        neograph::to_json(msgJson, msg);
        inputMessages.push_back(std::move(msgJson));
    }

    auto cfg = neograph::graph::RunConfig{
        .thread_id        = std::string{threadId},
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

asio::awaitable<std::string> BaseAgent::runSingleInputAsync(
    std::string_view threadId,
    std::string_view userInput,
    std::string_view systemPrompt,
    std::string_view modelName
) {
    std::vector<neograph::ChatMessage> messages;

    if (!systemPrompt.empty()) {
        messages.push_back(neograph::ChatMessage{
            .role    = "system",
            .content = std::string{systemPrompt},
        });
    }

    messages.push_back(neograph::ChatMessage{
        .role    = "user",
        .content = std::string{userInput},
    });

    co_return co_await runNonStreamAsync(threadId, messages, nullptr, modelName);
}

asio::awaitable<BaseAgent::SimpleRunResult> BaseAgent::runStreamAsync(
    const std::vector<neograph::ChatMessage>& messages,
    std::string_view                          modelName
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
