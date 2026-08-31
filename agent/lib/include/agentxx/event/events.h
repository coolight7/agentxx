#pragma once

#include "agentxx/util/log.h"
#include "neograph/graph/cancel.h"
#include <chrono>
#include <memory>
#include <neograph/json.h>
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

    /// 通用显示输出: EventDisplay
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

    /// subagent 委派 (统一批量语义): ReqSubagentBatch / RespSubagentBatch
    /// - 单任务与多任务统一为 tasks 数组 (单任务 = 1 个 task)
    inline static constexpr std::string_view Subagent{"service.subagent"};

    /// subagent 进度 (单向, 供 UI 观测): EventSubagentProgress
    inline static constexpr std::string_view SubagentProgress{"subagent.progress"};

    /// 跨 agent 查询: ReqCrossAgent / RespCrossAgent
    /// - 任一 agent (含 subagent) 可向指定 agentName 发起查询
    /// - 目标 agent 的持有者 (AgentHost) 应答
    inline static constexpr std::string_view CrossAgent{"service.crossagent"};

    /// 工具执行权限检查: ReqToolPermissionCheck / RespToolPermissionCheck
    inline static constexpr std::string_view ToolPermissionCheck{"service.permission.check"};

    /// 文件系统权限规则设置 (单向事件): EventSetPermissionRule
    inline static constexpr std::string_view PermissionSetRule{"service.permission.set_rule"};

    /// 每会话文件系统隔离边界设置/清除 (单向事件): EventSetSessionIsolation / EventClearSessionIsolation
    inline static constexpr std::string_view PermissionSetIsolation{"service.permission.set_isolation"};
    inline static constexpr std::string_view PermissionClearIsolation{"service.permission.clear_isolation"};

    /// subagent 工具执行 (请求-响应): ReqSubagentExecute / RespSubagentExecute
    inline static constexpr std::string_view SubagentExecute{"service.subagent.execute"};

    /// 文本 Token 估算服务 (同步服务): service.token.count
    inline static constexpr std::string_view TokenCount{"service.token.count"};
};

/// ===== agent 生命周期 =====

struct EventAgentTurnStart {
    std::string agentName;
    std::string sessionId;
    std::string userInput; // 本轮用户输入
};

struct EventAgentTurnEnd {
    std::string agentName;
    std::string sessionId;
    bool        hasError = false;
    std::string errorMessage;
};

/// ===== 模型调用 =====

struct EventModelCallStart {
    std::string agentName;
    std::string sessionId;
};

struct EventModelToken {
    std::string agentName;
    std::string sessionId;
    std::string token;            // 增量 token
    std::string kind = "content"; // "content" | "thinking"
};

struct EventModelCallEnd {
    std::string agentName;
    std::string sessionId;
    /// 本轮 LLM 调用产生的 assistant 消息 content (完整, 非 token 流)
    std::string content;
    /// token 使用量 (若 provider 提供)
    std::optional<int> totalTokens;
};

/// ===== 工具调用 =====

struct EventToolCallStart {
    std::string agentName;
    std::string sessionId;
    std::string toolName;
    std::string toolCallId;
    /// 原始 arguments json 字符串 (便于 UI 展示)
    std::string arguments;
};

struct EventToolCallEnd {
    std::string agentName;
    std::string sessionId;
    std::string toolName;
    std::string toolCallId;
    /// tool 执行结果 (已截断/摘要后的可见内容)
    std::string result;
    bool        hasError = false;
};

/// ===== subagent =====

struct EventSubagentProgress {
    /// subagent 会话标识 (父端 correlationId 或 subagent sessionId)
    std::string subagentId;
    std::string agentName;
    /// 进度类型: "token" | "tool_start" | "tool_end" | "turn_end"
    std::string kind;
    std::string data;
    /// 宿主中的 agent 唯一 id (AgentHost 发布时填充; 旧发布方为空)
    std::string agentId;
    /// 父 agent id (AgentHost 发布时填充; 空 = 根/旧发布方)
    std::string parentAgentId;
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
    std::string sessionId;
    std::string content;
};

struct EventCancel {
    std::string sessionId;
    std::string agentName;
};

struct EventError {
    std::string agentName;
    std::string sessionId;
    std::string message;
    std::string where; // 节点/模块名
};

/// ===== 请求-响应: 中断 HIL =====
/// - 中断请求由 Session 在捕获 NodeInterrupt 后发出
/// - 处理者 (InterruptHandler / UI 模块) 回填用户输入

struct ReqInterrupt {
    std::string agentName;
    std::string sessionId;
    /// 中断源节点名
    std::string interruptNode;
    /// 中断源节点值 (原始值, 供 UI 展示)
    std::string interruptValue;
    /// 中断处理句柄名 (对应 InterruptHandleArg.name), 如 "default"/"subagent"
    std::string handleName;
    /// 中断参数原始 json (单个 InterruptHandleArg 的序列化)
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
    std::string sessionId;
    std::string toolName;
    /// 权限分类: "filesystem_read" | "filesystem_write" | "command" | ...
    std::string category;
    /// 受权限约束的目标 (如路径/命令)
    std::string target;
    /// tool 调用参数 json
    std::string argumentsJson;
};

struct RespPermission {
    enum class Decision {
        Allow,
        Deny
    };
    Decision decision = Decision::Deny;
    /// 拒绝原因 (供 LLM/日志参考)
    std::string reason;
};

/// ===== 请求-响应: subagent 委派 (统一批量) =====
/// - 父 agent 经 NodeInterrupt 触发 subagent 委派中断 (中断名统一为 "subagent")
/// - Session 发出 ReqSubagentBatch, AgentHost 应答
/// - 响应到达后 Session resume 父 graph, 注入结果

/// 单个子代理任务
struct SubagentBatchItem {
    /// 目标 subagent 名 (SubAgentManagerTool.subAgentList key)
    std::string subagentName;
    /// subagent 系统提示 (空则用 subagent 默认)
    std::string systemPrompt;
    /// 派给 subagent 的任务消息 (user role)
    std::string message;
    /// 结构化消息透传 (可选): 提供时作为子代理初始上下文 (可含 system),
    /// 与 message 二选一 (messages 优先); 用于同上下文压缩等需要完整
    /// 消息前缀的场景
    std::optional<neograph::json> messages;
    /// 指定子代理运行的 thread id (可选): 为空时使用独立 subagent 线程 id;
    /// 指定时进入"同上下文模式": 运行在指定 thread + 使用父会话当前模型 +
    /// 消息前缀原样透传, 三者共同保证与父会话命中 provider KV/prefix cache
    std::string sessionId;
    /// 子代理工具策略 (可选 json 数组, 缺省 = 子代理默认全量工具):
    /// - `[]`: 无工具
    /// - `["*"]`: 全量继承父 agent 的工具 (AgentHost 解析为父工具名白名单)
    /// - `["name1", ...]`: 仅保留列表中的工具
    std::optional<neograph::json> tools;
    /// 子代理上下文压缩 (summarization) 中间件开关 (可选):
    /// - 缺省: 继承子代理 config 默认 (即父 config 拷贝)
    /// - false: 显式禁用 (summarization 发起的压缩子代理必须禁用,
    ///   避免对透传的上下文前缀二次压缩破坏 KV/prefix cache 一致性)
    /// - true: 显式启用
    std::optional<bool> enableSummarization;
    /// 任务结果标识: 回填到父 toolcall 的 tool_call_id 或自定义 id;
    /// 为空时按任务序号 (1-based) 兜底编号
    std::string resultId;
};

struct ReqSubagentBatch {
    std::string parentAgentName;
    std::string parentSessionId;
    /// 父会话取消令牌 (可空): 取消时级联中止全部在跑子代理
    std::shared_ptr<neograph::graph::CancelToken> cancelToken;
    std::vector<SubagentBatchItem>                tasks;
};

/// ===== 请求-响应: 跨 agent 查询 =====
/// - 任一 agent (含 subagent) 向另一指定 agent 发起查询
/// - 目标 agent 持有者 (AgentHost) 注册 server 响应
/// - 实现 agent 间 actor 式通信

struct ReqCrossAgent {
    /// 发起查询的 agent 名
    std::string fromAgent;
    /// 发起方的会话 id
    std::string fromSessionId;
    /// 目标 agent 名
    std::string toAgent;
    /// 查询消息 (user role 内容)
    std::string message;
};

struct RespCrossAgent {
    /// 目标 agent 的回复内容
    std::string content;
    bool        hasError = false;
    std::string errorMessage;
};

/// ===== subagent 批量结果 =====
/// - 单个 interrupt 携带 N 个子任务, AgentHost 并发运行
/// - 用于一轮内派发多个独立 subagent (如并行研究 + 编码)

struct RespSubagentBatchItem {
    std::string resultId;
    std::string content;
    bool        hasError = false;
    std::string errorMessage;
    /// 宿主派生时的 agent 唯一 id (AgentHost 填充; 节点已回收, 仅用于日志/关联)
    std::string agentId;
};

struct RespSubagentBatch {
    std::vector<RespSubagentBatchItem> results;
};

/// ===== 权限检查与规则设置 =====

/// 工具执行权限检查 (service.permission.check)
struct ReqToolPermissionCheck {
    std::string    agentName;
    std::string    sessionId;
    std::string    toolName;
    neograph::json arguments;
};

struct RespToolPermissionCheck {
    bool        allow = true;
    std::string reason;
};

/// 权限规则设置 (service.permission.set_rule)
struct EventSetPermissionRule {
    std::string path;
    bool        allow = false;
    size_t      index = 0; // 0 = read, 1 = write
};

/// 会话文件系统隔离设置 (service.permission.set_isolation)
struct EventSetSessionIsolation {
    std::string sessionId;
    std::string allowPath;
    std::string denyWritePath;
};

/// 会话文件系统隔离清除 (service.permission.clear_isolation)
struct EventClearSessionIsolation {
    std::string sessionId;
};

/// ===== subagent 工具执行 =====

/// subagent 工具执行请求/响应 (service.subagent.execute)
struct ReqSubagentExecute {
    neograph::json arguments;
};

struct RespSubagentExecute {
    std::string result;
    bool        hasError = false;
    std::string errorMessage;
};

} // namespace events
} // namespace agentxx
