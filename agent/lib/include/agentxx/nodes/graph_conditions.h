#pragma once

#include "neograph/graph/loader.h"
#include "neograph/graph/registry.h"
#include "neograph/graph/state.h"
#include <string>
#include <string_view>

namespace agentxx {
namespace nodes {

/// agentxx 图条件名: 本轮 LLM 是否请求了工具调用
///
/// 语义 (与 neograph 内置 `has_tool_calls` 相同, 但适用于"上下文不在图状态里"
/// 的新架构):
/// - 命中最后一条 assistant 消息: 它带有 tool_calls 返回 "true", 否则 "false";
///   上下文中没有 assistant 消息时返回 "false"
/// - 循环路由应使用本条件而不是内置 `has_tool_calls`: 后者读图状态的 `messages`
///   通道, 而 agentxx 的 LLM 上下文由会话持有 (图状态里没有该通道), 恒为 "false"
///
/// 取值来源 (按优先级):
/// 1. 上下文影子通道 `xx_messagesMeta` 的 `last_assistant_tool_calls`
///    (agentxx 默认图; 由宿主在上下文变更时刷新, 载荷与上下文大小无关);
/// 2. 兼容路径: 图状态里存在 `messages` 通道时按内置 `has_tool_calls` 同语义扫描
///    (自定义图或插件自己保存上下文时仍可用);
/// 3. 两者都没有: 返回 "false" (安全默认: 结束本轮而不是回到 tools 节点)
///
/// 用法 (图定义的条件边):
/// ```json
/// {"from":"llm","type":"conditional","condition":"xx_has_tool_calls",
///  "routes":{"true":"tools","false":"agent_end"}}
/// ```
inline constexpr std::string_view kConditionHasToolCalls = "xx_has_tool_calls";

/// 把 agentxx 提供的图条件注册到指定执行图注册表 (幂等)
/// - BaseAgent 装配执行图前调用 (注册到 per-agent GraphRegistry); 自定义宿主
///   自建 GraphRegistry 时也应当调用, 否则图定义引用 `xx_has_tool_calls` 会编译失败
/// - 已注册同名条件时覆盖为本实现 (条件语义以本文件为准)
void registerAgentGraphConditions(neograph::graph::GraphRegistry& registry);

/// 把 agentxx 的图条件注册到 neograph 的进程级全局条件注册表 (幂等, 进程内只做一次)
/// - 供"直接用 `GraphEngine::compile` / 自建 registry"的场景 (含插件测试) 也能解析
///   该条件名, 不依赖某个 agent 实例存在
/// - 线程安全: 进程内一次性注册 (std::once_flag)
void registerAgentGraphConditionsGlobal();

/// `xx_has_tool_calls` 的条件实现 (导出供测试直接调用)
/// - 纯函数: 只看传入的图状态, 不访问会话
std::string evalHasToolCallsCondition(const neograph::graph::GraphState& state);

} // namespace nodes
} // namespace agentxx
