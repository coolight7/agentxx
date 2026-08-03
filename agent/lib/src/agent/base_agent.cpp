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

    // 检查 tools 的提示词
    for (const auto& item : tools) {
        assert(item->get_definition().name == item->get_name());
    }

    auto graphDef = buildGraphDefinition();

    auto config = agentContext->agentConfig;

    neograph::graph::NodeContext nodeContext{};
    nodeContext.instructions = config->prompt.systemPrompt;
    nodeContext.provider     = ModelProviderRegistry::createProvider(config->model);
    nodeContext.extra_config = neograph::json{
        {agentxx::nodes::ModelCallWrapNode::defUseModelRegistryKey, true},
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
    } else {
        // [currentModelName] 不存在
        XX_LOGE("指定使用的模型不存在: `{}`", config->currentModelName);
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

    // 基于 system_clock 记录开始时间，用于后续时长计算
    const auto start_time = std::chrono::system_clock::now();
    // 记录轮次开始时间 (毫秒, Unix 时间戳, 用于显示)
    const auto start_time_ms = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(start_time.time_since_epoch()).count()
    );
    auto node_start_time    = start_time;
    auto node_start_time_ms = start_time_ms;

    // 产出增量事件的唯一出站口: 经 sendToPeer 发往对端 (server 端点会缓冲并经
    // transport 转发 client; io 为 nullptr 的 headless 场景则跳过)
    auto emitDelta = [&](Delta delta) {
        delta.seq = session->deltaSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (ioPtr) {
            ioPtr->sendToPeer(std::move(delta));
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

    auto lastChatChunkType = neograph::ChatStreamChunk::TYPE_UNKNOWN;

    auto internalEventCallback = [session,
                                  emitDelta,
                                  ioPtr,
                                  &lastChatChunkType,
                                  &start_time,
                                  &start_time_ms,
                                  &node_start_time,
                                  &node_start_time_ms](const neograph::graph::GraphEvent& event) {
        using T = neograph::graph::GraphEvent::Type;
        switch (event.type) {
            case T::LLM_TOKEN: {
                std::string token;
                bool        sendDuration = false;

                if (event.data.is_string()) {
                    token             = event.data.get<std::string>();
                    lastChatChunkType = neograph::ChatStreamChunk::TYPE_CONTENT;
                } else if (event.data.is_object()) {
                    neograph::ChatStreamChunk chunk;
                    neograph::from_json(event.data, chunk);
                    token             = std::move(chunk.data);
                    sendDuration      = (lastChatChunkType != chunk.type);
                    lastChatChunkType = chunk.type;
                }

                emitDelta(Delta{
                    .type        = (lastChatChunkType == neograph::ChatStreamChunk::TYPE_THINKING)
                                       ? Delta::Type::ThinkingToken
                                       : Delta::Type::TextToken,
                    .text        = std::move(token),
                    .startTimeMs = node_start_time_ms,
                    .durationMs  = sendDuration
                                       ? static_cast<int64_t>(
                                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::system_clock::now() - node_start_time
                                            )
                                                .count()
                                        )
                                       : 0,
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
                        auto content    = jm.value("content", std::string{});
                        auto toolName   = jm.value("tool_name", std::string{});
                        auto toolCallId = jm.value("tool_call_id", std::string{});
                        if (toolCallId.empty()) {
                            continue;
                        }
                        auto historyMsg = jm;
                        if (toolName == "filesystem_edit_text_file") {
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
                            .hasError   = false,
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
                lastChatChunkType = neograph::ChatStreamChunk::TYPE_UNKNOWN;
                node_start_time   = std::chrono::system_clock::now();
                node_start_time_ms
                    = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               node_start_time.time_since_epoch()
                    )
                                               .count());
                emitDelta(Delta{
                    .type        = Delta::Type::NodeStart,
                    .nodeName    = event.node_name,
                    .startTimeMs = node_start_time_ms,
                });
            } break;
            case T::NODE_END: {
                lastChatChunkType = neograph::ChatStreamChunk::TYPE_UNKNOWN;
                // 计算持续时间
                const int64_t duration_ms
                    = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::system_clock::now() - node_start_time
                    )
                                               .count());
                emitDelta(Delta{
                    .type        = Delta::Type::NodeEnd,
                    .nodeName    = event.node_name,
                    .startTimeMs = node_start_time_ms,
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

    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
            // - 存在值 [neograph::graph::RunResult], 声明 optional 类型用于后续赋值 [nullopt]
            std::optional<neograph::graph::RunResult> result;
            if (resumeInterrupt) {
                // - 程序重启恢复中断: 跳过 [engine->run_stream_async], 从恢复的 graphData
                // 重建中断结果, 直接进入中断处理循环, 重新处理可能未完成的中断并 resume
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
                // - 中断、异常、取消执行会让 neograph::engine 丢弃本轮 session
                // 已经产生的上下文，因此这里取 [graphDataKey_tempMessages] 而不是
                // [result->channel_raw("messages")]
                auto& im
                    = agentContext->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
                        threadId,
                        agentxx::middleware::MiddlewareContext::graphDataKey_tempMessages
                    );
                if (im.is_array()) {
                    session->llmMessages = im;
                }
            } else {
                result = co_await engine->run_stream_async(cfg, eventCallback);

                if (result->interrupted) {
                    auto& im
                        = agentContext->middlewareHandleContext
                              ->getGraphDataItemValue<neograph::json>(
                                  threadId,
                                  agentxx::middleware::MiddlewareContext::graphDataKey_tempMessages
                              );
                    if (im.is_array()) {
                        session->llmMessages = im;
                    }
                } else {
                    session->llmMessages = result->channel_raw("messages");
                }
            }

            while (result.has_value() && result->interrupted) {
                // 记录中断节点信息到 graphData, 供程序重启恢复中断时复用
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
                        // - 本轮 graph 还没有执行完成，序列化 graphData 到 state checkpoint
                        // 以防中断处理期间 程序 终止, 导致 graphData 丢失
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

                // 从 [graphDataKey_interruptArgs] 提取中断参数
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
                                    .parentThreadId  = std::string{threadId},
                                    .subagentName    = subagentArg.value("subagent", std::string{}),
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
                            auto session = agentContext->sessions->get(threadId);
                            if (session && session->bus) {
                                auto resp
                                    = co_await session->bus
                                          ->request<events::ReqInterrupt, events::RespInterrupt>(
                                              events::Topic::Interrupt,
                                              events::ReqInterrupt{
                                                  .agentName
                                                  = agentContext->agentConfig
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

                if (false == resumeValues.empty()) {
                    // 中断处理完成，写回结果
                    agentContext->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
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

                    // 恢复执行中断点，直接回到触发中断的 Node
                    result = co_await engine
                                 ->resume_async(std::string{threadId}, nullptr, eventCallback);

                    if (result->interrupted) {
                        // 中断时 [result] 内的 messages 是被 neograph::engine
                        // 回滚的，本轮 session 的上下文已经被丢弃；应该取中断时保存的 messages
                        auto& im = agentContext->middlewareHandleContext->getGraphDataItemValue<
                            neograph::json>(
                            threadId,
                            agentxx::middleware::MiddlewareContext::graphDataKey_tempMessages
                        );
                        if (im.is_array()) {
                            session->llmMessages = im;
                        }
                    } else {
                        session->llmMessages = result->channel_raw("messages");
                    }
                }
            }

            co_return true;
        },
        [&](std::string errmsg) -> asio::awaitable<bool> {
            XX_LOGE("Agent Session Response failed: {}", errmsg);
            turnResult.hasError     = true;
            turnResult.errorMessage = std::move(errmsg);
            co_return true;
        },
        [&](std::string& errmsg) -> std::optional<bool> {
            XX_LOGI("Agent Session Cancelled: {}", errmsg);
            turnResult.hasError     = true;
            turnResult.errorMessage = "Cancelled by user";
            return true;
        }
    );
    if (turnResult.hasError) {
        // - 出现异常时 state.messages 已经被回滚，提取临时保存的上下文，并写回 state
        auto& im = agentContext->middlewareHandleContext->getGraphDataItemValue<neograph::json>(
            threadId,
            agentxx::middleware::MiddlewareContext::graphDataKey_tempMessages
        );
        if (im.is_array()) {
            session->llmMessages = im;
            engine->update_state(std::string{threadId}, [&](neograph::graph::GraphState& state) {
                state.overwrite("messages", session->llmMessages);
            });
        }
    }

    engine->update_state(std::string{threadId}, [&](neograph::graph::GraphState& state) {
        // 中断已经处理完成，清理 graphData
        state.remove(agentxx::middleware::MiddlewareContext::channel_savedGraphData);
    });

    // 计算轮次持续时间
    const auto duration_ms
        = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now() - start_time
        )
                                   .count());

    // 发送 TurnEnd Delta，包含时长统计
    emitDelta(Delta{
        .type         = Delta::Type::TurnEnd,
        .historyCount = session->chainHash.count(),
        .tailHash     = session->chainHash.tailHex(),
        .startTimeMs  = start_time_ms,
        .durationMs   = duration_ms,
    });

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
