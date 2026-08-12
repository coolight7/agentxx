#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/events.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/nodes/agentcall.h"
#include "agentxx/nodes/modelcall.h"
#include "agentxx/nodes/toolcall.h"
#include "agentxx/tools/share_store.h"
#include "agentxx/tools/system.h"
#include "agentxx/tools/tool.h"
#include "agentxx/util/log.h"
#include "asio/io_context.hpp"
#include "neograph/graph/engine.h"
#include "neograph/graph/registry.h"
#include "neograph/graph/types.h"
#include <functional>
#include <memory>

namespace agentxx {
namespace agent {

/// Agent 基类: 提供核心基础设施与 ReAct 执行循环
/// - 管理 ioCtx / GraphEngine / AgentContext
/// - 提供会话执行 (runConversationTurnAsync) 与中断恢复
/// - 子类通过 override 虚函数自定义 middleware / tool / 图结构
/// - 支持多实例: 每个 BaseAgent 使用独立的 GraphRegistry, 不依赖全局 NodeFactory
class BaseAgent {
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
    /// per-agent 节点注册表 (支持多 Agent 实例, 不依赖全局 NodeFactory)
    std::shared_ptr<const neograph::graph::GraphRegistry> graphRegistry = nullptr;

    BaseAgent(std::shared_ptr<agentxx::agent::AgentConfig> in_config);

    asio::awaitable<void> init();

    struct ConversationTurnResult {
        bool        hasError = false;
        std::string errorMessage;
        bool        interrupted = false;
    };

    /// 选择指定会话 modelcall 使用的模型 (运行时切换, 按 thread_id 隔离)
    /// - modelName 为空或不存在时不改变该会话的选择
    void selectModel(std::string_view threadId, std::string_view modelName);

    /// 指定会话当前实际使用的模型显示名称 (解析会话选择/默认模型)
    std::string getCurrentModelName(std::string_view threadId) const;

    /// 执行一轮对话
    /// - 消息由 Session 内部管理 (viewMessages + llmMessages 双消息集)
    /// - 增量事件经 io->sendToPeer(Delta) 推送 (io 端点须已设置 transport);
    ///   io 传 nullptr 时为 headless 模式, 不产出事件
    asio::awaitable<ConversationTurnResult> runConversationTurnAsync(
        std::string_view             threadId,
        std::string_view             userInput,
        bool                         isFirstMsg,
        std::shared_ptr<AgentIOBase> io,
        std::string_view             modelName = ""
    );

    /// 收集会话启动时通知的信息 (MCP/Skill/Memory)
    /// - CodeAgent 覆写以收集实际加载的组件列表
    virtual void collectAppendComponentInfo(std::vector<AppendComponentNotification>& notifications
    );

    virtual ~BaseAgent();

    neograph::graph::GraphEngine*       getEngine();
    const neograph::graph::GraphEngine* getEngine() const;

    std::shared_ptr<AgentContext> getContext();

    /// Run agent with custom system prompt and user input, collect full output as
    /// string
    asio::awaitable<std::string> runNonStreamAsync(
        std::string_view                                        threadId,
        const std::vector<neograph::ChatMessage>&               messages,
        std::function<void(const neograph::graph::GraphEvent&)> callback  = nullptr,
        std::string_view                                        modelName = ""
    );

    /// Run agent with a single user input and optional custom system prompt
    asio::awaitable<std::string> runSingleInputAsync(
        std::string_view threadId,
        std::string_view userInput,
        std::string_view systemPrompt = "",
        std::string_view modelName    = ""
    );

    struct SimpleRunResult {
        std::string                content;
        neograph::graph::RunResult fullResult;
    };

    asio::awaitable<SimpleRunResult> runStreamAsync(
        const std::vector<neograph::ChatMessage>& messages,
        std::string_view                          modelName = ""
    );

protected:

    // =================================================================
    // 扩展点: 子类 override 以自定义 agent 行为
    // =================================================================

    /// 添加中间件到 agentContext->middlewareHandleContext->handles
    /// - 在 init() 中于 createTools() 之前调用
    /// - 中间件自带的 toolcalls 会在 createTools() 之后被自动收集
    virtual asio::awaitable<void> setupMiddleware();

    /// 创建工具列表
    /// - 在 init() 中于 setupMiddleware() 之后调用
    /// - 返回的工具将被注册到 GraphEngine
    virtual asio::awaitable<std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>> createTools();

    /// 构建图定义 JSON
    /// - 默认实现返回标准 ReAct 循环:
    ///   __start__ → agent_start → llm → [has_tool_calls?] → tools/agent_end → __end__
    virtual neograph::json buildGraphDefinition();

    /// 向 per-agent GraphRegistry 注册节点类型
    /// - 默认注册 4 个核心节点: AgentStart / AgentEnd / ModelCall / Toolcall
    /// - 子类可 override 添加自定义节点类型
    virtual void registerNodes(neograph::graph::GraphRegistry& registry);

    // =================================================================
    // 内部辅助方法
    // =================================================================

    /// 创建模型 Provider 注册表并注入 AgentContext
    void setupModelRegistry();

    /// 创建事件总线并注入 AgentContext
    void setupEventBus();

    /// 收集中间件自带的 toolcalls 到工具列表
    void collectMiddlewareTools(std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>& tools);

    /// 为所有工具创建 summarization 压缩句柄
    void setupSummarizationHandles(
        const std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>& tools
    );

    /// 通知 agent 启动进度 (供 init/createTools 各启动阶段调用):
    /// - 经 agentContext->startupNotifier 转发给客户端 (TUI banner 展示);
    ///   未注册回调时 no-op
    /// - 必须由 agent 线程 (init 协程上下文) 调用
    void notifyStartup(std::string_view step);
};

} // namespace agent
} // namespace agentxx
