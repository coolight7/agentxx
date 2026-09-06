#pragma once

#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/model_registry.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/nodes/agentcall.h"
#include "agentxx/nodes/modelcall.h"
#include "agentxx/nodes/toolcall.h"
#include "agentxx/tools/share_store.h"
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
/// - 提供会话执行 (runTurnAsync) 与中断恢复
/// - 子类通过 override 虚函数自定义 middleware / tool / 图结构
/// - 支持多实例: 每个 BaseAgent 使用独立的 GraphRegistry, 不依赖全局 NodeFactory
class BaseAgent {
public:

    /// 主协程调度器
    /// - 不要在同线程中传递到 [runCliAsync] 内使用, 因为
    ///   [engine->run_stream_async] 会启动其他 io_context, 交替 ioCtx 的话
    ///   会导致互相等待, 进而卡住
    /// - 某个异步函数需要 io_context 时, 可以通过 `co_await
    ///   asio::this_coro::executor` 获取当前异步函数运行时绑定的 io_context
    std::shared_ptr<asio::io_context>             ioCtx        = nullptr;
    std::shared_ptr<neograph::graph::GraphEngine> engine       = nullptr;
    std::shared_ptr<AgentContext>                 agentContext = nullptr;
    /// per-agent 节点注册表 (支持多 Agent 实例, 不依赖全局 NodeFactory)
    /// - 非 const 引用 (插件可注册节点类型; 与 AgentContext::graphRegistry 同对象)
    std::shared_ptr<neograph::graph::GraphRegistry> graphRegistry = nullptr;

    BaseAgent(std::shared_ptr<agentxx::agent::AgentConfig> in_config);

    asio::awaitable<void> init();

    struct TurnResult {
        bool        hasError = false;
        std::string errorMessage;
        bool        interrupted = false;
    };

    /// 选择指定会话 modelcall 使用的模型 (运行时切换, 按 sessionId 隔离)
    /// - modelName 为空或不存在时不改变该会话的选择
    void selectModel(std::string_view sessionId, std::string_view modelName);

    /// 指定会话当前实际使用的模型显示名称 (解析会话选择/默认模型)
    std::string getCurrentModelName(std::string_view sessionId) const;

    /// 获取当前使用的语言 (若指定 sessionId 且该会话有独立设置则优先返回, 否则返回 agentConfig->language, 兜底 "en")
    std::string getLanguage(std::string_view sessionId = "") const;

    /// 指定使用的语言 (不支持 auto, 为空或 auto 设为 "en")
    void setLanguage(std::string_view language, std::string_view sessionId = "");

    /// 执行一轮对话
    /// - 消息由 Session 内部管理 (viewMessages + llmMessages 双消息集)
    /// - 增量事件经 io->sendToPeer(WireDelta) 推送 (io 端点须已设置 transport);
    ///   io 传 nullptr 时为 headless 模式, 不产出事件
    asio::awaitable<TurnResult> runTurnAsync(
        std::string_view             sessionId,
        std::string_view             userInput,
        std::shared_ptr<AgentIOBase> io,
        std::string_view             modelName = ""
    );

    /// 收集会话启动时通知的信息 (MCP/Skill/Memory)
    /// - CodeAgent 覆写以收集实际加载的组件列表
    virtual void collectAppendComponentInfo(std::vector<AppendComponentNotification>& notifications
    );

    virtual ~BaseAgent();

    /// 获取底层图引擎 (注意生命周期: 与 BaseAgent 共享所有权)
    neograph::graph::GraphEngine* getEngine();

    /// 获取 agent 上下文 (ioCtx / middleware / 会话存储等)
    std::shared_ptr<AgentContext> getContext();

    /// 以指定消息列表运行一轮 agent (可自定义 system prompt 等效消息),
    /// 收集完整输出为字符串
    /// - [messages] 直接作为本轮输入消息 (含 system 角色即自定义系统提示)
    /// - [callback] 可选: 每次图事件回调 (用于流式展示)
    asio::awaitable<std::string> runOverMsgsTurnAsync(
        std::string_view                                        sessionId,
        const std::vector<neograph::ChatMessage>&               messages,
        std::function<void(const neograph::graph::GraphEvent&)> callback  = nullptr,
        std::string_view                                        modelName = ""
    );

    /// 以单条用户输入运行一轮 agent, 可附加自定义 system prompt
    asio::awaitable<std::string> runSingleInputAsync(
        std::string_view sessionId,
        std::string_view userInput,
        std::string_view systemPrompt = "",
        std::string_view modelName    = ""
    );

    struct SimpleRunResult {
        std::string                content;
        neograph::graph::RunResult fullResult;
    };

    /// 流式运行一轮 (输入固定为 [messages], 收集最终输出与完整 RunResult)
    asio::awaitable<SimpleRunResult> runStreamTurnAsync(
        const std::vector<neograph::ChatMessage>& messages,
        std::string_view                          modelName = ""
    );

    /// 合并的 run helper: 构造 RunConfig + 执行并收集 content
    asio::awaitable<SimpleRunResult> runInternalAsync(
        std::string_view                     sessionId,
        std::vector<neograph::ChatMessage>   messages,
        neograph::graph::GraphStreamCallback callback     = nullptr,
        std::string_view                     modelName    = "",
        bool                                 cleanupAfter = false
    );

protected:

    // =================================================================
    // 扩展点: 子类 override 以自定义 agent 行为
    // =================================================================

    /// 添加中间件到 agentContext->middlewareHandleContext->handles
    /// - 在 init() 中于 initTools() 之前调用
    /// - 中间件自带的 toolcalls 会在 initTools() 之后被自动收集
    virtual asio::awaitable<void> initMiddleware();

    /// 创建工具列表
    /// - 在 init() 中于 initMiddleware() 之后调用
    /// - 返回的工具将被注册到 GraphEngine
    virtual asio::awaitable<std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>> initTools();

    /// 构建图定义 JSON
    /// - 默认实现返回标准 ReAct 循环:
    ///   __start__ → agent_start → llm → [has_tool_calls?] → tools/agent_end → __end__
    /// - 图名称固定为 "agentxx.default" (插件可经 graph 接口表查看/修改)
    virtual neograph::json initGraphDefinition();

    /// 获取当前执行图 JSON 定义 (构建 engine 前生效的最终值, 含插件修改)
    const neograph::json& getGraphDefinitionJson() const {
        return agentContext ? agentContext->graphDefinitionJson : graphDefinitionJson_;
    }

    /// 获取当前执行图名称 (默认 "agentxx.default")
    std::string getGraphName() const {
        const auto& def = getGraphDefinitionJson();
        if (def.is_object() && def.contains("name") && def["name"].is_string()) {
            return def["name"].get<std::string>();
        }
        return std::string{kDefaultGraphName};
    }

    /// 默认执行图名称 (插件可经 graph 接口表查询/修改)
    inline static constexpr std::string_view kDefaultGraphName{"agentxx.default"};

    /// 向 per-agent GraphRegistry 注册节点类型
    /// - 默认注册 4 个核心节点: AgentStart / AgentEnd / ModelCall / Toolcall
    /// - 子类可 override 添加自定义节点类型
    virtual void initRegisterNodes(neograph::graph::GraphRegistry& registry);

    // =================================================================
    // 内部辅助方法
    // =================================================================

    /// 创建模型 Provider 注册表并注入 AgentContext
    void initModelRegistry();

    /// 创建事件总线并注入 AgentContext
    void initEventBus();

    /// 收集中间件自带的 toolcalls 到工具列表
    void initMiddlewareTools(std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>& tools);

    /// 为所有工具创建 summarization 压缩句柄
    void initSummarizationHandles(
        const std::vector<std::unique_ptr<agentxx::tools::XXToolBase>>& tools
    );

    /// 通知 agent 启动进度 (供 init/initTools 各启动阶段调用):
    /// - 经 agentContext->initNotifier 转发给客户端 (TUI banner 展示);
    ///   未注册回调时 no-op
    /// - 必须由 agent 线程 (init 协程上下文) 调用
    void notifyInitProgress(std::string_view step);

private:

    /// 执行图定义兜底存储 (agentContext 为空时使用; 正常情况下
    /// graphDefinitionJson 存于 AgentContext 供插件读写)
    neograph::json graphDefinitionJson_ = neograph::json::object();
};

} // namespace agent
} // namespace agentxx
