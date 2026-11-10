/// agentxx 功能埋点 id 清单 (唯一真源)
///
/// 一个"功能埋点"= 一个 id, 同时是
/// - 一条**能力** (`<应用名>.capabilities`, 声明为 `impls: many` + `merge: first`, 只开 `impl`);
/// - 一个**同名广播频道** (`<应用名>.broadcasts`)。
///
/// 插件两种参与方式 (可都选):
/// - **实现**: 登记一条实现行, 返回 JSON 文本 (需要返回值的埋点才看这个);
/// - **订阅**: 订阅同名频道, 只收通知 (纯通知型埋点的等价物就是它)。
///
/// 载荷字段 (广播与实现行入参见下表) 里一律带 `agentId`: 订阅侧用它过滤掉其它 agent
/// 的通知 (同一个插件可能同时挂在多个 agent 上)。
///
/// 与宿主声明表的一致性由测试模块 `boundaries` 校验: 本头的 id 清单必须与
/// `PluginManager` 的埋点声明表逐条相同。
#ifndef AGENTXX_PLUGIN_API_AGENTXX_POINT_IDS_H
#define AGENTXX_PLUGIN_API_AGENTXX_POINT_IDS_H

#include <string_view>

namespace agentxx {
namespace plugin {
namespace points {

/// 通知型: 一轮会话开始 (载荷 `{agentId, sessionId, turnId}`)
inline constexpr std::string_view TurnStart = "agentxx.agent.point.turn.start";
/// 通知型: 一轮会话结束 (载荷 `{agentId, sessionId, turnId, hasError, interrupted}`)
inline constexpr std::string_view TurnEnd = "agentxx.agent.point.turn.end";
/// 通知型: 一次模型调用开始 (载荷 `{agentId, sessionId, model}`)
inline constexpr std::string_view ModelStart = "agentxx.agent.point.model.start";
/// 通知型: 一次模型尝试 (重试会多次触发; 载荷 `{agentId, sessionId, model, attempt}`)
inline constexpr std::string_view ModelRun = "agentxx.agent.point.model.run";
/// 通知型: 一次模型调用结束 (载荷 `{agentId, sessionId, model, usage}`)
inline constexpr std::string_view ModelEnd = "agentxx.agent.point.model.end";
/// 通知型: 一次工具调用开始 (载荷 `{agentId, sessionId, tool, callId}`)
inline constexpr std::string_view ToolStart = "agentxx.agent.point.tool.start";
/// 通知型: 一次工具调用结束 (载荷 `{agentId, sessionId, tool, callId, status, elapsedMs}`)
inline constexpr std::string_view ToolEnd = "agentxx.agent.point.tool.end";

/// 取值型: token 估算 (入参 `{agentId, model, kind, text?}`, 实现行回答 `{tokens}`)
inline constexpr std::string_view ContextCountTokens = "agentxx.agent.point.context.countTokens";
/// 取值型: 上下文压缩 (入参 `{agentId, sessionId, model, targetTokens}`,
/// 实现行回答 `{summary, truncated?}`)
inline constexpr std::string_view ContextSummarize = "agentxx.agent.point.context.summarize";
/// 取值型: 工具输出摘要 (入参 `{agentId, tool, bytes, text}`, 实现行回答 `{summary}`)
inline constexpr std::string_view ToolSummarizeOutput = "agentxx.agent.point.tool.summarizeOutput";

/// 全部通知型埋点 (声明表与测试按它清点)
inline constexpr std::string_view kNotifyPoints[] = {
    TurnStart,
    TurnEnd,
    ModelStart,
    ModelRun,
    ModelEnd,
    ToolStart,
    ToolEnd,
};

/// 全部取值型埋点
inline constexpr std::string_view kValuePoints[] = {
    ContextCountTokens,
    ContextSummarize,
    ToolSummarizeOutput,
};

} // namespace points
} // namespace plugin
} // namespace agentxx

#endif /* AGENTXX_PLUGIN_API_AGENTXX_POINT_IDS_H */
