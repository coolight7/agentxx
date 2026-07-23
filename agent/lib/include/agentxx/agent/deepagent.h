#pragma once

#include "agentxx/agent/agent_io.h"
#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/events.h"
#include "agentxx/middlewares/permission.h"
#include "agentxx/middlewares/planning.h"
#include "agentxx/middlewares/skill.h"
#include "agentxx/middlewares/subagent_supervisor.h"
#include "agentxx/middlewares/summarization.h"
#include "agentxx/nodes/agentcall.h"
#include "agentxx/nodes/modelcall.h"
#include "agentxx/nodes/toolcall.h"
#include "agentxx/protocol/mcp_client.h"
#include "agentxx/protocol/openai_provider.h"
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
    std::shared_ptr<asio::io_context>             ioCtx        = nullptr;
    std::shared_ptr<neograph::graph::GraphEngine> engine       = nullptr;
    std::shared_ptr<AgentContext>                 agentContext = nullptr;

    DeepAgent(std::shared_ptr<agentxx::agent::AgentConfig> in_config);

    asio::awaitable<void> init();

    struct ConversationTurnResult {
        neograph::json messages;
        bool           hasError = false;
        std::string    errorMessage;
        bool           interrupted = false;
    };

    using InterruptCallback
        = std::function<asio::awaitable<void>(const std::string& interruptNode,
                                              const std::string& interruptValue,
                                              const std::string& interruptHandleName)>;

    /// 选择指定会话 modelcall 使用的模型 (运行时切换, 按 thread_id 隔离)
    /// - modelName 为空或不存在时不改变该会话的选择
    void selectModel(const std::string& threadId, const std::string& modelName);

    /// 指定会话当前实际使用的模型显示名称 (解析会话选择/默认模型)
    std::string getCurrentModelName(const std::string& threadId) const;

    asio::awaitable<ConversationTurnResult> runConversationTurnAsync(
        const std::string&                                      threadId,
        const std::string&                                      userInput,
        bool                                                    isFirstMsg,
        neograph::json                                          messages,
        std::shared_ptr<AgentIOBase>                            io,
        std::function<void(const neograph::graph::GraphEvent&)> eventCallback,
        InterruptCallback                                       interruptCallback = nullptr,
        const std::string&                                      modelName         = "");

    ~DeepAgent();

    // Get underlying engine
    neograph::graph::GraphEngine*       getEngine();
    const neograph::graph::GraphEngine* getEngine() const;

    // Get agent context
    std::shared_ptr<AgentContext> getContext();

    /// Run agent with custom system prompt and user input, collect full output as
    /// string
    /// - threadId: unique thread ID for this execution
    /// - messages: list of chat messages (system + user)
    /// - callback: optional event callback, nullptr if not needed
    /// - returns: full collected output content
    asio::awaitable<std::string>
        runNonStreamAsync(const std::string&                                      threadId,
                          const std::vector<neograph::ChatMessage>&               messages,
                          std::function<void(const neograph::graph::GraphEvent&)> callback
                          = nullptr,
                          const std::string& modelName = "");

    /// Run agent with a single user input and optional custom system prompt
    /// Convenience wrapper that builds messages automatically
    asio::awaitable<std::string> runSingleInputAsync(const std::string& threadId,
                                                     const std::string& userInput,
                                                     const std::string& systemPrompt = "",
                                                     const std::string& modelName    = "");

    /// Run a simple completion with just messages (for subagent
    /// scoring/optimization) Returns the full content as a string (collects all
    /// tokens)
    struct SimpleRunResult {
        std::string                content;
        neograph::graph::RunResult fullResult;
    };

    asio::awaitable<SimpleRunResult>
        runStreamAsync(const std::vector<neograph::ChatMessage>& messages,
                       const std::string&                        modelName = "");
};

} // namespace agent
} // namespace agentxx
