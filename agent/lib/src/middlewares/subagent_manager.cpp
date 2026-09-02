#include "agentxx/middlewares/subagent_manager.h"

#include "agentxx/event/event_stream.h"
#include "agentxx/util/log.h"
#include "fmt/format.h"
#include <memory>
#include <string>
#include <utility>

namespace agentxx {
namespace middleware {

namespace {

/// 注入给模型的 subagent tool 薄封装 (实际执行委托给中间件持有的
/// SubAgentManagerTool 单实例)
/// - 中间件自身持有 shared_ptr<SubAgentManagerTool> (生命周期 + 事件总线
///   服务注册), 而 toolcalls 中的 unique_ptr 会在
///   BaseAgent::initMiddlewareTools 被 move 进 engine, 故此处为委托封装,
///   而非直接放入同一对象 (两处共享同一实例, 行为一致)
class SubAgentDelegatingTool : public agentxx::tools::XXToolBase {
public:

    SubAgentDelegatingTool(
        std::weak_ptr<agentxx::agent::AgentContext>          in_agentContext,
        std::shared_ptr<agentxx::tools::SubAgentManagerTool> in_tool
    ) :
        agentxx::tools::XXToolBase("subagent_manager", std::move(in_agentContext)),
        tool_(std::move(in_tool)) {}

    std::string get_name() const override {
        return tool_->get_name();
    }

    neograph::ChatTool get_definition() const override {
        return tool_->get_definition();
    }

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override {
        co_return co_await tool_->execute_async(arguments);
    }

private:

    std::shared_ptr<agentxx::tools::SubAgentManagerTool> tool_;
};

} // namespace

SubagentManagerMiddlewareHandle::SubagentManagerMiddlewareHandle(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    BaseMiddlewareHandle<_SubagentManagerMiddlewareState>("subagent_manager", in_agentContext) {
    auto agentCtx = agentContext.lock();
    // 先创建实际 subagent 管理工具 (单实例; 事件总线服务与 toolcall 执行共用),
    // 再 applyConfig: applyConfig 按配置决定是否把委托工具注入 toolcalls,
    // 依赖 tool 已存在 (注入的委托工具转发到同一实例)
    tool = std::make_shared<agentxx::tools::SubAgentManagerTool>(
        "subagent_manager",
        agentContext
    );
    // 注册默认 subagent 任务 (历史逻辑从 BaseAgent::initMiddleware 迁移)
    const auto nodeName = std::string{"subagent_task"};
    tool->subAgentList.insert(std::make_pair(
        nodeName,
        std::make_shared<agentxx::tools::SubAgentNormalTask>(
            nodeName,
            R"(Create a isolation messages context sub agent to exec. (need system prompt))"
        )
    ));
    applyConfig(agentCtx->agentConfig);
    registerOnBus(agentCtx->bus);
}

SubagentManagerMiddlewareHandle::~SubagentManagerMiddlewareHandle() {
    unregisterFromBus();
}

void SubagentManagerMiddlewareHandle::applyConfig(
    const std::shared_ptr<agentxx::agent::AgentConfig>& config
) {
    if (!config || !tool) {
        return;
    }
    // 重复调用幂等: 先清空已注入项再按配置决定
    toolcalls.clear();
    if (config->enableSubagent) {
        // 启用: 注入 `agentxx_subagent` tool, 模型可发起子代理委派
        toolcalls.push_back(std::make_unique<SubAgentDelegatingTool>(agentContext, tool));
        XX_LOGD("subagent manager: tool `agentxx_subagent` injected (enableSubagent=true)");
    }
}

void SubagentManagerMiddlewareHandle::registerOnBus(
    const std::shared_ptr<agentxx::event::EventBus>& bus
) {
    if (!bus) {
        return;
    }
    unregisterFromBus();
    registeredBus_ = bus;
    // 事件总线服务始终注册 (与 tool 是否注入无关):
    // summarization 压缩等非 toolcall 路径经 service.subagent.execute 委派
    if (tool) {
        tool->registerOnBus(bus);
    }
}

void SubagentManagerMiddlewareHandle::unregisterFromBus() {
    if (auto bus = registeredBus_.lock()) {
        if (tool) {
            tool->unregisterFromBus();
        }
    }
    registeredBus_.reset();
}

} // namespace middleware
} // namespace agentxx
