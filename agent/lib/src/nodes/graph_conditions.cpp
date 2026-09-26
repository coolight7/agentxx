#include "agentxx/nodes/graph_conditions.h"

#include "agentxx/middlewares/middleware.h"
#include <mutex>

namespace agentxx {
namespace nodes {

namespace {

const std::string& hasToolCallsName() {
    static const std::string kName{kConditionHasToolCalls};
    return kName;
}

/// 条件声明的取值集合 (闭合): 与 neograph 内置 has_tool_calls 一致,
/// 便于校验器检查条件边的 routes 覆盖
neograph::graph::ConditionSpec hasToolCallsSpec() {
    return neograph::graph::ConditionSpec{{"false", "true"}, /*open=*/false};
}

} // namespace

std::string evalHasToolCallsCondition(const neograph::graph::GraphState& state) {
    // 1) agentxx 默认图: 读上下文影子通道 (会话是上下文唯一权威, 图状态里没有消息正文)
    if (state.has_channel(agentxx::middleware::MiddlewareContext::channel_messagesMeta)) {
        const auto meta = state.get(agentxx::middleware::MiddlewareContext::channel_messagesMeta);
        if (meta.is_object()) {
            // 影子通道缺失该字段 (老版本写入的元信息) 时按"没有工具调用"处理
            const auto toolCalls
                = meta.value("last_assistant_tool_calls", static_cast<int64_t>(0));
            return toolCalls > 0 ? "true" : "false";
        }
    }

    // 2) 兼容路径: 图状态自己保存上下文 (自定义图 / 插件自带 messages 通道),
    //    与 neograph 内置 has_tool_calls 完全同语义 (从末尾回溯第一条 assistant)
    if (state.has_channel("messages")) {
        auto messages = state.get_messages();
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->role == "assistant") {
                return it->tool_calls.empty() ? "false" : "true";
            }
        }
    }

    // 3) 都没有: 安全默认 (结束本轮, 不回到 tools 节点)
    return "false";
}

void registerAgentGraphConditions(neograph::graph::GraphRegistry& registry) {
    registry.register_condition(
        hasToolCallsName(),
        [](const neograph::graph::GraphState& state) -> std::string {
            return evalHasToolCallsCondition(state);
        },
        hasToolCallsSpec()
    );
}

void registerAgentGraphConditionsGlobal() {
    static std::once_flag once;
    std::call_once(once, [] {
        neograph::graph::ConditionRegistry::instance().register_condition(
            hasToolCallsName(),
            [](const neograph::graph::GraphState& state) -> std::string {
                return evalHasToolCallsCondition(state);
            },
            hasToolCallsSpec()
        );
    });
}

} // namespace nodes
} // namespace agentxx
