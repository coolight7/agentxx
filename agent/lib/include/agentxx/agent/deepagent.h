#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/events.h"
#include "agentxx/middlewares/interrupt_handler.h"
#include "agentxx/middlewares/permission.h"
#include "agentxx/middlewares/planning.h"
#include "agentxx/middlewares/skill.h"
#include "agentxx/middlewares/subagent_supervisor.h"
#include "agentxx/middlewares/summarization.h"
#include "agentxx/nodes/agentcall.h"
#include "agentxx/nodes/modelcall.h"
#include "agentxx/nodes/toolcall.h"
#include "agentxx/protocol/mcp_client.h"
#include "agentxx/tools/cross_agent_query.h"
#include "agentxx/tools/execute_command.h"
#include "agentxx/tools/filesystem.h"
#include "agentxx/tools/planning.h"
#include "agentxx/tools/rag_search.h"
#include "agentxx/tools/share_store.h"
#include "agentxx/tools/string.h"
#include "agentxx/tools/sub_agent.h"
#include "agentxx/tools/system.h"
#include "agentxx/tools/tool_skill_search.h"
#include "agentxx/tools/ui_control.h"
#include "agentxx/tools/web_search.h"
#include "agentxx/util/log.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "neograph/graph/engine.h"
#include "neograph/graph/types.h"
#include "neograph/llm/openai_provider.h"
#include "neograph/mcp/client.h"
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>

namespace agentxx {
namespace agent {

class DeepAgent {
public:
  /// 主协程调度器
  /// - 不要在同线程中传递到 [runCliAsync]
  /// 内使用，因为[engine->run_stream_async]会启动其他 io_context， 交替 ioCtx
  /// 的话会导致互相等待，进而卡住
  /// - 某个异步函数需要 io_context 时，可以通过 `co_await
  /// asio::this_coro::executor` 获取当前异步函数运行时绑定的 io_context
  std::shared_ptr<asio::io_context> ioCtx = nullptr;
  std::shared_ptr<neograph::graph::GraphEngine> engine = nullptr;
  std::shared_ptr<AgentContext> agentContext = nullptr;

  DeepAgent(std::shared_ptr<agentxx::agent::AgentConfig> in_config) {
    ioCtx = std::make_shared<asio::io_context>();
    agentContext = std::make_shared<AgentContext>();
    agentContext->agentConfig = in_config;
    assert(nullptr != in_config);
    assert(in_config->modelOpenAIBaseUrl.empty() == false);
  }

  asio::awaitable<void> init() {
#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
    XX_LOGD("Enable asio/async file RW");
#else
    XX_LOGD("Disable asio/async file RW");
#endif

    auto config = agentContext->agentConfig;
    neograph::llm::OpenAIProvider::Config provideConfig{
        .api_key = config->modelOpenAIApiKey,
        .base_url = config->modelOpenAIBaseUrl,
        .default_model = config->modelOpenAIModelName,
    };

    {
      /// 创建事件总线并注入 AgentContext
      /// - executor 取自 DeepAgent::ioCtx, 与 graph 运行在同一 io_context,
      ///   单线程协作式调度, 模块间无需加锁
      /// - 所有节点/middleware/tool 经 weak_ptr<AgentContext> 取
      /// agentContext->bus
      agentContext->bus = std::make_shared<agentxx::middleware::EventBus>(
          ioCtx->get_executor());
    }

    {
      /// register Node
      neograph::graph::NodeFactory::instance().register_type(
          std::string{agentxx::nodes::AgentStartCallWrapNode::defNodeType},
          [this](const std::string &name, const neograph::json &,
                 const neograph::graph::NodeContext &ctx) {
            return std::make_unique<agentxx::nodes::AgentStartCallWrapNode>(
                name, agentContext);
          });
      neograph::graph::NodeFactory::instance().register_type(
          std::string{
              agentxx::nodes::MiddlewareWrapAgentEndCallNode::defNodeType},
          [this](const std::string &name, const neograph::json &,
                 const neograph::graph::NodeContext &ctx) {
            return std::make_unique<
                agentxx::nodes::MiddlewareWrapAgentEndCallNode>(name,
                                                                agentContext);
          });
      neograph::graph::NodeFactory::instance().register_type(
          std::string{agentxx::nodes::ModelCallWrapNode::defNodeType},
          [this](const std::string &name, const neograph::json &,
                 const neograph::graph::NodeContext &ctx) {
            return std::make_unique<agentxx::nodes::ModelCallWrapNode>(
                name, ctx, agentContext);
          });
      neograph::graph::NodeFactory::instance().register_type(
          std::string{agentxx::nodes::ToolcallWrapNode::defNodeType},
          [this](const std::string &name, const neograph::json &,
                 const neograph::graph::NodeContext &ctx) {
            return std::make_unique<agentxx::nodes::ToolcallWrapNode>(
                name, ctx, agentContext);
          });
    }

    /// middleware
    agentContext->middlewareHandleContext =
        std::make_shared<agentxx::middleware::MiddlewareContext>();
    std::shared_ptr<agentxx::middleware::SummarizationMiddlewareHandle>
        summarizationMiddleware;
    auto subagentManagerTool =
        std::make_unique<agentxx::tools::SubAgentManagerTool>(
            "subagent_manager", agentContext);
    agentContext->subagentManagerToolPtr = subagentManagerTool.get();
    {
      {
        agentContext->permissionMiddleware =
            std::make_shared<agentxx::middleware::PermissionMiddlewareHandle>(
                agentContext);
        agentContext->middlewareHandleContext->handles.push_back(
            agentContext->permissionMiddleware);
      }
      {
        auto skillMiddleware =
            std::make_shared<agentxx::middleware::SkillMiddlewareHandle>(
                config->skillDirPaths, agentContext);
        // skillMiddleware->toolcalls.push_back(
        //     std::make_unique<agentxx::tools::SkillTool>(agentContext));
        agentContext->middlewareHandleContext->handles.push_back(
            skillMiddleware);
      }
      {
        summarizationMiddleware = std::make_shared<
            agentxx::middleware::SummarizationMiddlewareHandle>(
            subagentManagerTool.get(), agentContext, 256 * 1024);
        agentContext->middlewareHandleContext->handles.push_back(
            summarizationMiddleware);
      }
      {
        auto planningMiddleware =
            std::make_shared<agentxx::middleware::PlanningMiddlewareHandle>(
                agentContext);
        planningMiddleware->toolcalls.push_back(
            std::make_unique<agentxx::tools::WritePlanningTool>(
                planningMiddleware, agentContext));
        agentContext->middlewareHandleContext->handles.push_back(
            planningMiddleware);
      }

      /// Toolcall  应当作为最后一层，输出的日志才会是最终的样子
      agentContext->middlewareHandleContext->handles.push_back(
          std::make_shared<agentxx::middleware::MiddlewareWarpHandle<
              agentxx::middleware::BaseMiddlewareState>>(
              "LogPring", agentContext,
              (agentxx::middleware::onGraphNodeBeforeCallFunc) nullptr,
              (agentxx::middleware::onGraphNodeAfterCallFunc) nullptr,
              (agentxx::middleware::onGraphNodeBeforeCallFunc) nullptr,
              [config = agentContext->agentConfig](
                  neograph::graph::NodeInput &in) -> asio::awaitable<void> {
                if (config->logPrintMessagesBeforeLLM) {
                  agentxx::middleware::BaseMiddlewareHandleInterface::
                      printMessages(
                          in.state.get_messages(),
                          config->logPrintMessagesBeforeLLMWithSystemMsg);
                }
                co_return;
              },
              (agentxx::middleware::onGraphNodeAfterCallFunc) nullptr,
              [config = agentContext->agentConfig](
                  neograph::graph::NodeInput &in) -> asio::awaitable<void> {
                if (config->logPringToolcall) {
                  co_await agentxx::nodes::ToolcallWrapNode::
                      defStdoutLogOnToolcallStart(in);
                }
              },
              [config = agentContext->agentConfig](
                  const neograph::graph::NodeInput &in,
                  neograph::graph::NodeOutput &result)
                  -> asio::awaitable<void> {
                if (config->logPringToolcall) {
                  co_await agentxx::nodes::ToolcallWrapNode::
                      defStdoutLogOnToolcallEnd(in, result);
                }
              }));
    }

    /// Toolcall
    std::vector<std::unique_ptr<agentxx::tools::XXToolBase>> tools{};
    {
      /// middleware tools
      for (auto &item : agentContext->middlewareHandleContext->handles) {
        if (false == item->toolcalls.empty()) {
          tools.insert(tools.end(),
                       std::make_move_iterator(item->toolcalls.begin()),
                       std::make_move_iterator(item->toolcalls.end()));
        }
      }
    }
    {
      /// MCP tool
      for (const auto &url : config->mcpServerUrls) {
        try {
          XX_LOGD("load mcp tool: {}", url);
          auto mcpClient = std::make_shared<agentxx::server::McpClient>(
              agentxx::server::McpClient::Config{
                  .serverUrl = url,
                  .protocolVersion =
                      std::string{
                          agentxx::server::McpClient::kProtocol2025_11_25},
              });
          auto result = co_await mcpClient->initialize();
          if (result.has_value()) {
            auto mcpTools = co_await mcpClient->listTools();
            if (mcpTools.has_value()) {
              for (auto &tool : mcpTools.value()) {
                // TODO: 重名检查
                tools.push_back(
                    mcpClient->createTool(std::move(tool), agentContext));
              }
            } else {
              XX_LOGE("list mcp tool error: {} | {}", url, mcpTools.error());
            }
          } else {
            XX_LOGE("load mcp tool error: {} | {}", url, result.error());
          }
          // auto mcpClient = neograph::mcp::MCPClient{url};
          // if (co_await mcpClient.initialize_async(config->agentName)) {
          //   auto mcpTools = co_await mcpClient.get_tools_async();
          //   XX_LOGD("append mcp tool size: {}", mcpTools.size());
          //   for (auto &tool : mcpTools) {
          //     // TODO: 重名检查
          //     tools.push_back(std::make_unique<agentxx::tools::XXToolWarp>(
          //         std::move(tool), agentContext, false, true, 0));
          //   }
          // }
        } catch (const std::exception &e) {
          std::string errmsg = e.what();
          agentxx::util::autoConvertToUtf8(errmsg);
          XX_LOGE("[agentxx] Append mcp tool error: {} | {}", url, errmsg);
        } catch (const boost::exception &e) {
          auto errmsg = boost::diagnostic_information(e);
          agentxx::util::autoConvertToUtf8(errmsg);
          XX_LOGE("[agentxx] Append mcp tool error: {} | {}", url, errmsg);
        } catch (...) {
          XX_LOGE("[agentxx] Append mcp tool error: {}", url);
        }
      }
    }
    {
      tools.push_back(
          std::make_unique<agentxx::tools::ThreadShareStoreTool>(agentContext));
      tools.push_back(
          std::make_unique<agentxx::tools::FileSystemListTool>(agentContext));
      tools.push_back(
          std::make_unique<agentxx::tools::FilesystemReadTextFileTool>(
              agentContext));
      tools.push_back(
          std::make_unique<agentxx::tools::FilesystemReadBinaryFileTool>(
              agentContext));
      tools.push_back(std::make_unique<agentxx::tools::FilesystemWriteFileTool>(
          agentContext));
      tools.push_back(
          std::make_unique<agentxx::tools::FilesystemEditTextFileTool>(
              agentContext));
      tools.push_back(
          std::make_unique<agentxx::tools::FilesystemGlobTool>(agentContext));
      tools.push_back(
          std::make_unique<agentxx::tools::FilesystemGrepTool>(agentContext));

      tools.push_back(std::make_unique<agentxx::tools::StringHtml2MarkdownTool>(
          agentContext));
      tools.push_back(
          std::make_unique<agentxx::tools::StringRegexpTool>(agentContext));

      tools.push_back(std::make_unique<agentxx::tools::GetCurrentDateTimeTool>(
          agentContext));
#if XX_IS_WIN_D || XX_IS_LINUX_D
      tools.push_back(std::make_unique<agentxx::tools::GetSystemCoreInfoTool>(
          agentContext));
#endif

      tools.push_back(
          std::make_unique<agentxx::tools::WebFetchUrlTool>(agentContext));
      tools.push_back(std::make_unique<agentxx::tools::WebFetchUrlMarkdownTool>(
          agentContext));
      if (false == config->websearchApiUrl.empty()) {
        tools.push_back(std::make_unique<agentxx::tools::WebSearchTool>(
            config->websearchApiUrl, config->websearchConvertHtml2markdown,
            agentContext));
      }

      if (false == config->ragDocsPaths.empty()) {
        auto client = std::make_shared<agentxx::tools::EmbeddingClient>(
            config->modelOpenAIBaseUrl, config->modelOpenAIApiKey,
            config->modelOpenAIModelName);
        auto docsStore =
            std::make_shared<agentxx::tools::RAGSearchTool::VectorStore>(
                client);
        auto docs = co_await docsStore->scanDocument(config->ragDocsPaths);
        auto docxSize = docs.size();
        auto isAddSuccess = co_await docsStore->addDocuments(std::move(docs));
        XX_LOGD(R"_(
┏━━━━━━ RAG Embedding ━━━━━━┓
{}
┗━━━━━━ RAG Embedding ━━━━━━┛
)_",
                isAddSuccess
                    ? fmt::format("┣━ ✅ success: append {} docs", docxSize)
                    : "┣━ ❌ failed");
        tools.push_back(std::make_unique<agentxx::tools::RAGSearchTool>(
            docsStore, agentContext));
      }

#if XX_IS_WIN_D
      tools.push_back(
          std::make_unique<agentxx::tools::UIControlKeyboardMouseTool>(
              agentContext));
      tools.push_back(
          std::make_unique<agentxx::tools::ExecuteWindowsCommandTool>(
              agentContext));
#elif XX_IS_LINUX_D
      tools.push_back(std::make_unique<agentxx::tools::ExecuteLinuxCommandTool>(
          agentContext));
      if (agentxx::util::isRunningInWSL()) {
        tools.push_back(
            std::make_unique<agentxx::tools::ExecuteWindowsCommandTool>(
                agentContext));
      }
#elif XX_IS_MACOS_D
      tools.push_back(std::make_unique<agentxx::tools::ExecuteLinuxCommandTool>(
          agentContext));
#endif

      {
        // cross-agent query tool (供主 agent/subagent 互相查询)
        tools.push_back(std::make_unique<agentxx::tools::CrossAgentQueryTool>(
            agentContext));
      }

      {
        // subagent
        {
          // subagent_task
          neograph::graph::NodeContext nodeContext{};
          nodeContext.instructions = "";
          nodeContext.provider =
              neograph::llm::OpenAIProvider::create_shared(provideConfig);

          /// 复制 tool
          std::vector<neograph::Tool *> toolPtrs;
          toolPtrs.reserve(tools.size());
          for (auto &t : tools) {
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
        //       neograph::llm::OpenAIProvider::create_shared(provideConfig);

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
    for (const auto &tool : tools) {
      auto handle = tool->createSummarizationToolHandle();
      if (handle.has_value()) {
        summarizationMiddleware->summarizationToolHandles[tool->get_name()] =
            handle.value();
      }
    }

    /// === Main Agent ===
    neograph::graph::NodeContext nodeContext{};
    nodeContext.instructions = config->prompt.systemPrompt;
    nodeContext.provider =
        neograph::llm::OpenAIProvider::create_shared(provideConfig);

    std::vector<neograph::Tool *> toolPtrs;
    toolPtrs.reserve(tools.size());
    for (auto &t : tools) {
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
            "channels",
            {
                {"messages", {{"reducer", "append"}}},
                {
                    agentxx::middleware::MiddlewareContext::
                        channel_savedGraphData,
                    {{"reducer", "overwrite"}},
                },
            },
        },
        {
            "nodes",
            {
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
                        agentxx::nodes::MiddlewareWrapAgentEndCallNode::
                            defNodeType,
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
            "edges",
            neograph::json::array({
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

    engine = std::move(neograph::graph::GraphEngine::compile(
        graphDefinition, nodeContext, store));
    {
      auto crudeTools = std::vector<std::unique_ptr<neograph::Tool>>{};
      for (auto &tool : tools) {
        crudeTools.push_back(std::move(tool));
      }
      engine->own_tools(std::move(crudeTools));
    }

    co_return;
  }

  struct ConversationTurnResult {
    neograph::json messages;
    bool hasError = false;
    std::string errorMessage;
    bool interrupted = false;
  };

  using InterruptCallback = std::function<asio::awaitable<void>(
      const std::string &interruptNode, const std::string &interruptValue,
      const std::string &interruptHandleName)>;

  asio::awaitable<ConversationTurnResult> runConversationTurnAsync(
      const std::string &threadId, const std::string &userInput,
      bool isFirstMsg, neograph::json messages,
      std::function<void(const neograph::graph::GraphEvent &)> eventCallback,
      InterruptCallback interruptCallback = nullptr) {
    ConversationTurnResult turnResult;

    bool resumeInterrupt = false;
    if (false ==
        agentContext->middlewareHandleContext->graphData.contains(threadId)) {
      // - 程序启动后，如果存在 graphData，则从 state 恢复 graphData
      // (被中断时保存的)
      auto data = engine->get_state(threadId).value_or(neograph::json{});
      if (data.is_object() &&
          data.contains(
              agentxx::middleware::MiddlewareContext::channel_savedGraphData)) {
        resumeInterrupt = true;
        agentContext->middlewareHandleContext->setGraphDataFromState(data,
                                                                     threadId);
      }
    }

    try {
      auto processedInput = userInput;
      agentxx::util::autoConvertToUtf8(processedInput);

      messages.push_back(neograph::json{
          {"role", "user"},
          {"content", processedInput},
      });
      auto cfg = neograph::graph::RunConfig{
          .thread_id = threadId,
          .input = {{"messages", messages}},
          .max_steps = 1024,
          .stream_mode = neograph::graph::StreamMode::ALL,
          .cancel_token = std::make_shared<neograph::graph::CancelToken>(),
          .resume_if_exists = isFirstMsg,
      };

      std::optional<neograph::graph::RunResult> result =
          co_await engine->run_stream_async(cfg, eventCallback);

      if (result->interrupted) {
        auto &im = agentContext->middlewareHandleContext
                       ->getGraphDataItemValue<neograph::json>(
                           threadId, agentxx::middleware::MiddlewareContext::
                                         graphDataKey_interruptMessages);
        if (im.is_array()) {
          messages = im;
        }
      } else {
        messages = result->channel_raw("messages");
      }

      const auto isInterrupted = result->interrupted;
      while (result.has_value() && result->interrupted) {
        engine->update_state(threadId, [&](neograph::graph::GraphState &state) {
          // - 本轮 graph 还没有执行完成，序列化 graphData 到 state checkpoint
          // 以防中断处理期间 程序 终止, 导致 graphData 丢失
          auto data =
              agentContext->middlewareHandleContext->getGraphDataToState(
                  state, threadId);
          state.overwrite(
              agentxx::middleware::MiddlewareContext::channel_savedGraphData,
              data);
        });

        auto crudeResult = std::move(result);
        result = std::nullopt;

        turnResult.interrupted = true;
        auto interruptNode = crudeResult->interrupt_node;
        auto interruptValue = crudeResult->interrupt_value.dump();

        auto resumeValues = neograph::json{};

        // 从 xx_savedGraphData 提取中断参数
        const auto interruptArgs =
            agentxx::middleware::InterruptHandleArg::listFromJson(
                agentContext->middlewareHandleContext
                    ->getGraphDataItemValue<neograph::json>(
                        threadId, agentxx::middleware::MiddlewareContext::
                                      graphDataKey_interruptArgs));
        size_t argIndex = 0;
        for (const auto &interruptArg : interruptArgs) {
          ++argIndex;
          if (interruptCallback) {
            co_await interruptCallback(interruptNode, interruptValue,
                                       interruptArg.name);
          }

          std::optional<neograph::json> interruptResult;
          {
            assert(nullptr != agentContext->bus);
            if (interruptArg.name == "subagent") {
              // subagent 委派: 经总线派发给 SubagentSupervisor
              // - 父 agent 已 checkpoint 暂停, supervisor 运行 subagent
              // - 结果注入 interruptResult, resume 后父 graph 继续
              auto subagentArg = interruptArg.arg;
              auto resp = co_await agentContext->bus->request<
                  events::ReqSubagentStart, events::RespSubagentResult>(
                  events::Topic::Subagent,
                  events::ReqSubagentStart{
                      .parentAgentName =
                          agentContext->agentConfig
                              ? agentContext->agentConfig->agentName
                              : std::string{},
                      .parentThreadId = threadId,
                      .subagentName =
                          subagentArg.value("subagent", std::string{}),
                      .systemPrompt =
                          subagentArg.value("system_prompt", std::string{}),
                      .message = subagentArg.value("message", std::string{}),
                      .resultId = interruptArg.resultId,
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
                  .parentThreadId = threadId,
              };
              if (batchArg.contains("tasks") && batchArg["tasks"].is_array()) {
                for (const auto &t : batchArg["tasks"]) {
                  batchReq.tasks.push_back(events::SubagentBatchItem{
                      .subagentName = t.value("subagent", std::string{}),
                      .systemPrompt = t.value("system_prompt", std::string{}),
                      .message = t.value("message", std::string{}),
                      .resultId = t.value("result_id", std::string{}),
                  });
                }
              }
              auto batchResp = co_await agentContext->bus->request<
                  events::ReqSubagentBatch, events::RespSubagentBatch>(
                  events::Topic::SubagentBatch, std::move(batchReq));
              if (batchResp.has_value()) {
                // 批量结果按 resultId 写入 resumeValues (非单个
                // interruptResult)
                for (const auto &r : batchResp->results) {
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
              auto resp =
                  co_await agentContext->bus
                      ->request<events::ReqInterrupt, events::RespInterrupt>(
                          events::Topic::Interrupt,
                          events::ReqInterrupt{
                              .agentName =
                                  agentContext->agentConfig
                                      ? agentContext->agentConfig->agentName
                                      : std::string{},
                              .threadId = threadId,
                              .interruptNode = interruptNode,
                              .handleName = interruptArg.name,
                              .interruptArgsJson = interruptArg.toJson().dump(),
                              .resultId = interruptArg.resultId,
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
          agentContext->middlewareHandleContext
              ->setGraphDataItemValue<neograph::json>(
                  threadId,
                  agentxx::middleware::MiddlewareContext::
                      graphDataKey_interruptResult,
                  resumeValues);

          engine->update_state(
              threadId, [&](neograph::graph::GraphState &state) {
                // 更新 message
                state.overwrite("messages", std::move(messages));
              });

          result =
              co_await engine->resume_async(threadId, nullptr, eventCallback);

          if (result->interrupted) {
            // 中断时 [result] 内的 messages 是旧的，应该取中断时保存的 messages
            auto &im =
                agentContext->middlewareHandleContext
                    ->getGraphDataItemValue<neograph::json>(
                        threadId, agentxx::middleware::MiddlewareContext::
                                      graphDataKey_interruptMessages);
            if (im.is_array()) {
              messages = im;
            }
          } else {
            messages = result->channel_raw("messages");
          }
        }
      }

      engine->update_state(threadId, [&](neograph::graph::GraphState &state) {
        // 中断已经处理完成，清理 graphData
        state.remove(
            agentxx::middleware::MiddlewareContext::channel_savedGraphData);
      });

      turnResult.messages = std::move(messages);
    } catch (const std::exception &e) {
      turnResult.hasError = true;
      turnResult.errorMessage = e.what();
      XX_LOGE(R"({{"error": "Agent Response failed: {}"}})", e.what());
    } catch (const boost::exception &e) {
      auto errmsg = boost::diagnostic_information(e);
      agentxx::util::autoConvertToUtf8(errmsg);
      XX_LOGE(R"({{"error": "Agent Response failed: {}"}})", errmsg);
    } catch (...) {
      turnResult.hasError = true;
      turnResult.errorMessage = "Unknown error";
      XX_LOGE(R"({{"error": "Agent Response failed: Unknown error"}})");
    }

    co_return turnResult;
  }

  asio::awaitable<void> runCliAsync() {
    const auto cliEventCallback = [](const neograph::graph::GraphEvent &event) {
      switch (event.type) {
      case neograph::graph::GraphEvent::Type::NODE_START:
      case neograph::graph::GraphEvent::Type::NODE_END:
        break;
      case neograph::graph::GraphEvent::Type::LLM_TOKEN: {
        std::cout << event.data.get<std::string>() << std::flush;
      } break;
      case neograph::graph::GraphEvent::Type::CHANNEL_WRITE:
      case neograph::graph::GraphEvent::Type::INTERRUPT:
      case neograph::graph::GraphEvent::Type::ERROR:
        break;
      }
    };
    const auto cliInterruptCallback =
        [](const std::string &interruptNode, const std::string &interruptValue,
           const std::string &interruptHandleName) -> asio::awaitable<void> {
      XX_OUT(R"_(
┏━━━━━━ Interrupted ━━━━━━┓
┣━ Interrupted at: {}
┣━ Value: {}
{}
┗━━━━━━ Interrupted ━━━━━━┛
)_",
             interruptNode, interruptValue,
             (false == interruptHandleName.empty())
                 ? fmt::format("┣━ Interrupt Handle: {}", interruptHandleName)
                 : "┣━ Unknown InterruptHandleArg");
      co_return;
    };

    bool isFirstMsg = true;
    const auto thread_id = "session";
    auto messages = neograph::json::array();

    // 注册 CLI 的中断/权限/subagent HIL 处理到总线
    // - GUI/ACP 前端应注册各自的 handler 替换此处
    agentxx::middleware::CliInterruptHandler cliInterruptHandler{agentContext};
    agentxx::middleware::CliPermissionPrompter cliPermissionPrompter{
        agentContext};
    agentxx::middleware::SubagentSupervisor subagentSupervisor{agentContext};
    co_await cliInterruptHandler.start();
    co_await cliPermissionPrompter.start();
    co_await subagentSupervisor.start();

    std::cout << ">>> " << std::flush;

    for (std::string line; std::getline(std::cin, line);) {
      if (false == line.empty()) {
        std::cout << agentContext->agentConfig->agentNameView << ": "
                  << std::flush;

        auto turnResult = co_await runConversationTurnAsync(
            thread_id, line, isFirstMsg, std::move(messages),
            agentxx::middleware::EventBridge::make(
                agentContext->agentConfig->agentName, thread_id, agentContext,
                cliEventCallback),
            cliInterruptCallback);
        messages = std::move(turnResult.messages);
        isFirstMsg = false;
      }
      std::cout << "\n\n>>> ";
    }
  }

  void runCli() {
    asio::co_spawn(
        *ioCtx,
        [this]() -> asio::awaitable<void> {
          co_await init();
          co_return co_await runCliAsync();
        },
        asio::detached);
    ioCtx->run();
  }

  ~DeepAgent() { engine = nullptr; }

  // Get underlying engine
  neograph::graph::GraphEngine *getEngine() { return engine.get(); }
  const neograph::graph::GraphEngine *getEngine() const { return engine.get(); }

  // Get agent context
  std::shared_ptr<AgentContext> getContext() { return agentContext; }

  /// Run agent with custom system prompt and user input, collect full output as
  /// string
  /// - threadId: unique thread ID for this execution
  /// - messages: list of chat messages (system + user)
  /// - callback: optional event callback, nullptr if not needed
  /// - returns: full collected output content
  asio::awaitable<std::string> runNonStreamAsync(
      const std::string &threadId,
      const std::vector<neograph::ChatMessage> &messages,
      std::function<void(const neograph::graph::GraphEvent &)> callback =
          nullptr) {
    auto inputMessages = neograph::json::array();
    for (const auto &msg : messages) {
      neograph::json msgJson;
      neograph::to_json(msgJson, msg);
      inputMessages.push_back(std::move(msgJson));
    }

    auto cfg = neograph::graph::RunConfig{
        .thread_id = threadId,
        .input = {{"messages", std::move(inputMessages)}},
        .resume_if_exists = false,
    };

    std::ostringstream oss;
    auto wrappedCallback =
        [&oss, callback](const neograph::graph::GraphEvent &event) {
          switch (event.type) {
          case neograph::graph::GraphEvent::Type::LLM_TOKEN: {
            auto token = event.data.get<std::string>();
            oss << token;
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

  /// Run agent with a single user input and optional custom system prompt
  /// Convenience wrapper that builds messages automatically
  asio::awaitable<std::string>
  runSingleInputAsync(const std::string &threadId, const std::string &userInput,
                      const std::string &systemPrompt = "") {
    std::vector<neograph::ChatMessage> messages;

    if (!systemPrompt.empty()) {
      messages.push_back(neograph::ChatMessage{
          .role = "system",
          .content = systemPrompt,
      });
    }

    messages.push_back(neograph::ChatMessage{
        .role = "user",
        .content = userInput,
    });

    co_return co_await runNonStreamAsync(threadId, messages);
  }

  /// Run a simple completion with just messages (for subagent
  /// scoring/optimization) Returns the full content as a string (collects all
  /// tokens)
  struct SimpleRunResult {
    std::string content;
    neograph::graph::RunResult fullResult;
  };

  asio::awaitable<SimpleRunResult>
  runStreamAsync(const std::vector<neograph::ChatMessage> &messages) {
    auto inputMessages = neograph::json::array();
    for (const auto &msg : messages) {
      neograph::json msgJson;
      neograph::to_json(msgJson, msg);
      inputMessages.push_back(std::move(msgJson));
    }

    auto threadId = fmt::format(
        "subagent_{}",
        std::chrono::system_clock::now().time_since_epoch().count());

    neograph::graph::RunConfig cfg{
        .thread_id = threadId,
        .input = {{"messages", std::move(inputMessages)}},
        .resume_if_exists = false,
    };

    std::ostringstream oss;
    auto callback = [&oss](const neograph::graph::GraphEvent &event) {
      if (event.type == neograph::graph::GraphEvent::Type::LLM_TOKEN) {
        oss << event.data.get<std::string>();
      }
    };

    auto result = co_await engine->run_stream_async(cfg, callback);
    co_return SimpleRunResult{
        .content = oss.str(),
        .fullResult = std::move(result),
    };
  }
};

} // namespace agent
} // namespace agentxx