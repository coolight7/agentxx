#pragma once

#include "agentxx/util/log.h"
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace events {

/// 事件流 topic 命名空间常量
/// - 命名约定: <scope>.<subject>[.<detail>]
///   - scope: agent | service | subagent | io
///   - 主体模块只通过这些 topic 字符串与 EventBus 交互
struct Topic {
  /// ===== 单向事件 (EventStream<T>) =====
  /// agent 生命周期: EventAgentTurnStart / EventAgentTurnEnd
  inline static constexpr std::string_view AgentTurnStart{"agent.turn.start"};
  inline static constexpr std::string_view AgentTurnEnd{"agent.turn.end"};

  /// 模型调用: EventModelCallStart / EventModelToken / EventModelCallEnd
  inline static constexpr std::string_view ModelCallStart{"agent.model.start"};
  inline static constexpr std::string_view ModelToken{"agent.model.token"};
  inline static constexpr std::string_view ModelCallEnd{"agent.model.end"};

  /// 工具调用: EventToolCallStart / EventToolCallEnd
  inline static constexpr std::string_view ToolCallStart{"agent.tool.start"};
  inline static constexpr std::string_view ToolCallEnd{"agent.tool.end"};

  /// subagent 进度 (单向, 供 UI 观测): EventSubagentProgress
  inline static constexpr std::string_view SubagentProgress{
      "subagent.progress"};

  /// 通用显示输出 (替代直接 std::cout): EventDisplay
  inline static constexpr std::string_view Display{"io.display"};

  /// 用户输入 (前端发布): EventUserInput
  inline static constexpr std::string_view UserInput{"io.user_input"};

  /// 取消控制信号: EventCancel
  inline static constexpr std::string_view Cancel{"io.cancel"};

  /// 错误: EventError
  inline static constexpr std::string_view Error{"agent.error"};

  /// ===== 请求-响应事件 (RequestResponseStream<TReq, TResp>) =====
  /// 中断 HIL: ReqInterrupt / RespInterrupt
  inline static constexpr std::string_view Interrupt{"service.interrupt"};

  /// 权限询问: ReqPermission / RespPermission
  inline static constexpr std::string_view Permission{"service.permission"};

  /// subagent 委派: ReqSubagentStart / RespSubagentResult
  inline static constexpr std::string_view Subagent{"service.subagent"};

  /// subagent 批量委派: ReqSubagentBatch / RespSubagentBatch
  inline static constexpr std::string_view SubagentBatch{"service.subagent.batch"};

  /// 跨 agent 查询: ReqCrossAgent / RespCrossAgent
  /// - 任一 agent (含 subagent) 可向指定 agentName 发起查询
  /// - 目标 agent 的持有者 (如 SubagentSupervisor) 应答
  inline static constexpr std::string_view CrossAgent{"service.crossagent"};
};

/// ===== agent 生命周期 =====

struct EventAgentTurnStart {
  std::string agentName;
  std::string threadId;
  std::string userInput; // 本轮用户输入
};

struct EventAgentTurnEnd {
  std::string agentName;
  std::string threadId;
  bool hasError = false;
  std::string errorMessage;
};

/// ===== 模型调用 =====

struct EventModelCallStart {
  std::string agentName;
  std::string threadId;
};

struct EventModelToken {
  std::string agentName;
  std::string threadId;
  std::string token; // 增量 token
};

struct EventModelCallEnd {
  std::string agentName;
  std::string threadId;
  /// 本轮 LLM 调用产生的 assistant 消息 content (完整, 非 token 流)
  std::string content;
  /// token 使用量 (若 provider 提供)
  std::optional<int> totalTokens;
};

/// ===== 工具调用 =====

struct EventToolCallStart {
  std::string agentName;
  std::string threadId;
  std::string toolName;
  std::string toolCallId;
  /// 原始 arguments json 字符串 (便于 UI 展示)
  std::string arguments;
};

struct EventToolCallEnd {
  std::string agentName;
  std::string threadId;
  std::string toolName;
  std::string toolCallId;
  /// tool 执行结果 (已截断/摘要后的可见内容)
  std::string result;
  bool hasError = false;
};

/// ===== subagent =====

struct EventSubagentProgress {
  /// subagent 会话标识 (父端 correlationId 或 subagent threadId)
  std::string subagentId;
  std::string agentName;
  /// 进度类型: "token" | "tool_start" | "tool_end" | "turn_end"
  std::string kind;
  std::string data;
};

/// ===== IO =====

struct EventDisplay {
  std::string agentName;
  /// 显示级别: "info" | "token" | "tool" | "error" | "interrupt"
  std::string level;
  std::string content;
};

struct EventUserInput {
  std::string agentName;
  std::string threadId;
  std::string content;
};

struct EventCancel {
  std::string threadId;
  std::string agentName;
};

struct EventError {
  std::string agentName;
  std::string threadId;
  std::string message;
  std::string where; // 节点/模块名
};

/// ===== 请求-响应: 中断 HIL =====
/// - 中断请求由 Session 在捕获 NodeInterrupt 后发出
/// - 处理者 (InterruptHandler / UI 模块) 回填用户输入

struct ReqInterrupt {
  std::string agentName;
  std::string threadId;
  /// 中断源节点名
  std::string interruptNode;
  /// 中断处理句柄名 (对应 InterruptHandleArg.name), 如 "default"/"subagent"
  std::string handleName;
  /// 中断参数原始 json (InterruptHandleArg 列表)
  std::string interruptArgsJson;
  /// 中断结果回填的 resultId (对应 InterruptHandleArg.resultId)
  std::string resultId;
};

struct RespInterrupt {
  /// 是否已处理 (false=放弃/取消, true=有结果)
  bool handled = false;
  /// 回填到 interruptResult channel 的值 (json 字符串)
  std::string resultJson;
};

/// ===== 请求-响应: 权限询问 =====
/// - 权限策略判定留在 PermissionMiddlewareHandle 栈内
/// - 当策略为 INTERRUPT 时, 走总线询问用户/外部授权者

struct ReqPermission {
  std::string agentName;
  std::string threadId;
  std::string toolName;
  /// 权限分类: "filesystem_read" | "filesystem_write" | "command" | ...
  std::string category;
  /// 受权限约束的目标 (如路径/命令)
  std::string target;
  /// tool 调用参数 json
  std::string argumentsJson;
};

struct RespPermission {
  enum class Decision { Allow, Deny };
  Decision decision = Decision::Deny;
  /// 拒绝原因 (供 LLM/日志参考)
  std::string reason;
};

/// ===== 请求-响应: subagent 委派 =====
/// - 父 agent 经 NodeInterrupt 触发 subagent 委派中断
/// - Session 发出 ReqSubagentStart, SubagentSupervisor 应答
/// - 响应到达后 Session resume 父 graph, 注入结果

struct ReqSubagentStart {
  std::string parentAgentName;
  std::string parentThreadId;
  /// 目标 subagent 名 (SubAgentManagerTool.subAgentList key)
  std::string subagentName;
  /// subagent 系统提示 (空则用 subagent 默认)
  std::string systemPrompt;
  /// 派给 subagent 的任务消息 (user role)
  std::string message;
  /// 回填到父 toolcall 的 tool_call_id
  std::string resultId;
};

struct RespSubagentResult {
  /// subagent 最终输出内容
  std::string content;
  bool hasError = false;
  std::string errorMessage;
};

/// ===== 请求-响应: 跨 agent 查询 =====
/// - 任一 agent (含 subagent) 向另一指定 agent 发起查询
/// - 目标 agent 持有者 (SubagentSupervisor / Session) 注册 server 响应
/// - 实现 agent 间 actor 式通信

struct ReqCrossAgent {
  /// 发起查询的 agent 名
  std::string fromAgent;
  /// 发起方的会话 id
  std::string fromThreadId;
  /// 目标 agent 名
  std::string toAgent;
  /// 查询消息 (user role 内容)
  std::string message;
};

struct RespCrossAgent {
  /// 目标 agent 的回复内容
  std::string content;
  bool hasError = false;
  std::string errorMessage;
};

/// ===== 批量 subagent 委派 =====
/// - 单个 interrupt 携带 N 个子任务, SubagentSupervisor 并发运行
/// - 用于一轮内派发多个独立 subagent (如并行研究 + 编码)

struct SubagentBatchItem {
  std::string subagentName;
  std::string systemPrompt;
  std::string message;
  /// 回填到父 toolcall 的 tool_call_id
  std::string resultId;
};

struct ReqSubagentBatch {
  std::string parentAgentName;
  std::string parentThreadId;
  std::vector<SubagentBatchItem> tasks;
};

struct RespSubagentBatchItem {
  std::string resultId;
  std::string content;
  bool hasError = false;
  std::string errorMessage;
};

struct RespSubagentBatch {
  std::vector<RespSubagentBatchItem> results;
};

} // namespace events
} // namespace agentxx
