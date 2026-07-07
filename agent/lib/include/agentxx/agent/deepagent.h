#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/permission.h"
#include "agentxx/middlewares/planning.h"
#include "agentxx/middlewares/skill.h"
#include "agentxx/middlewares/summarization.h"
#include "agentxx/nodes/agentcall.h"
#include "agentxx/nodes/modelcall.h"
#include "agentxx/nodes/toolcall.h"
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
#include "neograph/llm/openai_provider.h"
#include "neograph/mcp/client.h"
#include "neograph/neograph.h"
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>

namespace agentxx {
namespace agent {

class DeepAgent {
protected:
  /// 主协程调度器
  /// - 不要在同线程中传递到 [runCliAsync]
  /// 内使用，因为[engine->run_stream_async]会启动其他 io_context， 交替 ioCtx
  /// 的话会导致互相等待，进而卡住
  /// - 某个异步函数需要 io_context 时，可以通过 `co_await
  /// asio::this_coro::executor` 获取当前异步函数运行时绑定的 io_context
  std::shared_ptr<asio::io_context> ioCtx = nullptr;
  std::unique_ptr<neograph::graph::GraphEngine> engine = nullptr;
  std::shared_ptr<AgentContext> agentContext = nullptr;

public:
  DeepAgent(std::shared_ptr<agentxx::agent::AgentConfig> in_config) {
    ioCtx = std::make_shared<asio::io_context>();
    agentContext = std::make_shared<AgentContext>();
    agentContext->agentConfig = in_config;
    assert(nullptr != in_config);
    assert(in_config->modelOpenAIBaseUrl.empty() == false);
  }

  asio::awaitable<void> init() {
    auto config = agentContext->agentConfig;
    neograph::llm::OpenAIProvider::Config provideConfig{
        .api_key = config->modelOpenAIApiKey,
        .base_url = config->modelOpenAIBaseUrl,
        .default_model = config->modelOpenAIModelName,
    };

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

      /// Toolcall  应当作为最后一层
      agentContext->middlewareHandleContext->handles.push_back(
          std::make_shared<agentxx::middleware::MiddlewareWarpHandle<
              agentxx::middleware::BaseMiddlewareState>>(
              "toolcall_log", agentContext,
              (agentxx::middleware::onGraphNodeBeforeCallFunc) nullptr,
              (agentxx::middleware::onGraphNodeAfterCallFunc) nullptr,
              (agentxx::middleware::onGraphNodeBeforeCallFunc) nullptr,
              (agentxx::middleware::onGraphNodeBeforeCallFunc) nullptr,
              (agentxx::middleware::onGraphNodeAfterCallFunc) nullptr,
              [](neograph::graph::NodeInput &in) -> asio::awaitable<void> {
                return agentxx::nodes::ToolcallWrapNode::
                    defStdoutLogOnToolcallStart(in);
              },
              [](const neograph::graph::NodeInput &in,
                 neograph::graph::NodeOutput &result) {
                return agentxx::nodes::ToolcallWrapNode::
                    defStdoutLogOnToolcallEnd(in, result);
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
      for (auto &url : config->mcpServerUrls) {
        auto mcpClient = neograph::mcp::MCPClient{url};
        if (mcpClient.initialize(config->agentName)) {
          auto mcpTools = mcpClient.get_tools();
          XX_LOGD("append mcp tool size: {}", mcpTools.size());
          for (auto &tool : mcpTools) {
            tools.push_back(std::make_unique<agentxx::tools::XXToolWarp>(
                std::move(tool), agentContext, false, true, 0));
          }
        }
      }
    }
    {
      tools.push_back(
          std::make_unique<agentxx::tools::ThreadShareStoreTool>(agentContext));
      tools.push_back(std::make_unique<agentxx::tools::FileSystemListFileTool>(
          agentContext));
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

      if (false == config->ragDocsPaths.empty()) {
        auto client = std::make_shared<agentxx::tools::EmbeddingClient>(
            config->modelOpenAIBaseUrl, config->modelOpenAIApiKey,
            config->modelOpenAIModelName);
        auto docsStore =
            std::make_shared<agentxx::tools::RAGSearchTool::VectorStore>(
                client);
        auto docs = co_await docsStore->scanDocument(config->ragDocsPaths);
        std::cout << "\n┏━━━━━━ RAG Embedding ━━━━━━┓" << std::endl;
        if (co_await docsStore->addDocuments(std::move(docs))) {
          fmt::println("┣━ ✅ success: append {} docs", docs.size());
        } else {
          std::cout << "┣━ ❌ failed" << std::endl;
        }
        std::cout << "┗━━━━━━ RAG Embedding ━━━━━━┛\n" << std::endl;
        tools.push_back(std::make_unique<agentxx::tools::RAGSearchTool>(
            docsStore, agentContext));
      }

      if (false == config->websearchApiUrl.empty()) {
        tools.push_back(std::make_unique<agentxx::tools::WebSearchTool>(
            config->websearchApiUrl, config->websearchConvertHtml2markdown,
            agentContext));
      }
      tools.push_back(
          std::make_unique<agentxx::tools::WebFetchUrlTool>(agentContext));
      tools.push_back(std::make_unique<agentxx::tools::WebFetchUrlMarkdownTool>(
          agentContext));

      tools.push_back(std::make_unique<agentxx::tools::GetCurrentDateTimeTool>(
          agentContext));

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
#endif

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
              nodeName, std::make_shared<agentxx::tools::SubAgentNormalTask>(
                            nodeName,
                            "Create a isolation messages context sub agent to "
                            "exec. (need system prompt)",
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

        // {
        //   // training_scorer: 训练模式专用评分 subagent，无需工具
        //   neograph::graph::NodeContext scorerCtx{};
        //   scorerCtx.instructions = "";
        //   scorerCtx.provider =
        //       neograph::llm::OpenAIProvider::create_shared(provideConfig);

        //   const auto scorerName = std::string{"training_scorer"};

        //   subagentManagerTool->subAgentList.insert(std::make_pair(
        //       scorerName,
        //       std::make_shared<agentxx::tools::SubAgentNormalTask>(
        //           scorerName,
        //           "Scoring sub-agent for training mode: evaluates agent "
        //           "responses against test case criteria",
        //           scorerCtx)));
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

    /// Main Agent
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
                {"messages", {{"type", "list"}, {"reducer", "append"}}},
                {
                    agentxx::middleware::BaseMiddlewareHandleInterface::
                        channelKey_interruptMessages,
                    {{"reducer", "overwrite"}},
                },
                {
                    agentxx::middleware::BaseMiddlewareHandleInterface::
                        channelKey_interruptArg,
                    {{"reducer", "overwrite"}},
                },
                {
                    agentxx::middleware::BaseMiddlewareHandleInterface::
                        channelKey_interruptResult,
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

    engine = neograph::graph::GraphEngine::compile(graphDefinition, nodeContext,
                                                   store);
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
      const std::string &interruptHandleName, bool interruptHandleFound)>;

  asio::awaitable<ConversationTurnResult> runConversationTurnAsync(
      const std::string &threadId, const std::string &userInput,
      bool isFirstMsg, neograph::json messages,
      std::function<void(const neograph::graph::GraphEvent &)> eventCallback,
      InterruptCallback interruptCallback = nullptr) {
    ConversationTurnResult turnResult;

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
          .max_steps = 100,
          .stream_mode = neograph::graph::StreamMode::ALL,
          .cancel_token = std::make_shared<neograph::graph::CancelToken>(),
          .resume_if_exists = isFirstMsg,
      };

      std::optional<neograph::graph::RunResult> result =
          co_await engine->run_stream_async(cfg, eventCallback);
      if (result->interrupted) {
        messages = result->channel_raw(
            agentxx::middleware::BaseMiddlewareHandleInterface::
                channelKey_interruptMessages);
      } else {
        messages = result->channel_raw("messages");
      }

      while (result.has_value() && result->interrupted) {
        auto crudeResult = std::move(result);
        result = std::nullopt;

        turnResult.interrupted = true;
        auto interruptNode = crudeResult->interrupt_node;
        auto interruptValue = crudeResult->interrupt_value.dump();

        std::string interruptHandleName;
        bool interruptHandleFound = false;
        std::optional<neograph::json> resumeValue;

        auto interruptArg = agentxx::middleware::InterruptHandleArg::fromJson(
            crudeResult->channel_raw(
                agentxx::middleware::BaseMiddlewareHandleInterface::
                    channelKey_interruptArg));
        if (interruptArg.has_value()) {
          interruptHandleName = interruptArg->name;
          auto handleIt =
              agentContext->middlewareHandleContext->interruptHandles.find(
                  interruptArg->name);
          interruptHandleFound =
              (handleIt !=
               agentContext->middlewareHandleContext->interruptHandles.end());
        }

        if (interruptCallback) {
          co_await interruptCallback(interruptNode, interruptValue,
                                     interruptHandleName, interruptHandleFound);
        }

        if (interruptArg.has_value() && interruptHandleFound) {
          resumeValue = co_await agentContext->middlewareHandleContext
                            ->execInterruptHandle(interruptArg->name,
                                                  interruptArg.value());
        }

        if (resumeValue.has_value()) {
          engine->update_state(
              threadId, [&](neograph::graph::GraphState &state) {
                state.overwrite(
                    agentxx::middleware::BaseMiddlewareHandleInterface::
                        channelKey_interruptResult,
                    resumeValue.value());
                state.overwrite("messages", std::move(messages));
              });

          result =
              co_await engine->resume_async(threadId, nullptr, eventCallback);
          if (result->interrupted) {
            messages = result->channel_raw(
                agentxx::middleware::BaseMiddlewareHandleInterface::
                    channelKey_interruptMessages);
          } else {
            messages = result->channel_raw("messages");
          }
        }
      }

      turnResult.messages = std::move(messages);
    } catch (const std::exception &e) {
      turnResult.hasError = true;
      turnResult.errorMessage = e.what();
      XX_LOGE(R"({{"error": "Agent Response failed: {}"}})", e.what());
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
           const std::string &interruptHandleName,
           bool interruptHandleFound) -> asio::awaitable<void> {
      std::cout << "\n┏━━━━━━ Interrupted ━━━━━━┓" << std::endl;
      std::cout << "┣━ Interrupted at: " << interruptNode << std::endl;
      std::cout << "┣━ Value: " << interruptValue << std::endl;
      if (false == interruptHandleName.empty()) {
        if (interruptHandleFound) {
          std::cout << "┣━ Interrupt Handle: " << interruptHandleName
                    << std::endl;
        } else {
          std::cout << "┣━ Interrupt Handle Not Found for: "
                    << interruptHandleName << std::endl;
        }
      } else {
        std::cout << "┣━ Unknown InterruptHandleArg" << std::endl;
      }
      std::cout << "┗━━━━━━ Interrupted ━━━━━━┛\n" << std::endl;
      co_return;
    };

    bool isFirstMsg = true;
    const auto thread_id = "session";
    auto messages = neograph::json::array();

    std::cout << ">>> " << std::flush;

    for (std::string line; std::getline(std::cin, line);) {
      if (false == line.empty()) {
        std::cout << agentContext->agentConfig->agentNameView << ": "
                  << std::flush;

        auto turnResult = co_await runConversationTurnAsync(
            thread_id, line, isFirstMsg, std::move(messages), cliEventCallback,
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
    neograph::json inputMessages = neograph::json::array();
    for (const auto &msg : messages) {
      neograph::json msgJson;
      neograph::to_json(msgJson, msg);
      inputMessages.push_back(std::move(msgJson));
    }

    neograph::graph::RunConfig cfg{
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
    neograph::json inputMessages = neograph::json::array();
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