#pragma once

#include "agentxx/util/tool_result.h"
#include "neograph/graph/cancel.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace events {

/// 事件总线 topic 命名空间常量
/// - 命名约定 (唯一一份实现: `agentxx/event/naming.h`):
///   - 内置 agent 端: `agentxx.agent.<域>[.<细节>]`
///   - 内置 client 端: `agentxx.client.<域>[.<细节>]`
///   - 插件: `plugin.<插件名>.<自由部分>` (插件写全名, 框架只校验不补前缀)
///   - 主体模块只通过这些 topic 字符串与 EventBus 交互
struct Topic {
    /// ===== 单向事件 (EventStream<T>) =====
    /// agent 生命周期: EventAgentTurnStart / EventAgentTurnEnd
    inline static constexpr std::string_view AgentTurnStart{"agentxx.agent.turn.start"};
    inline static constexpr std::string_view AgentTurnEnd{"agentxx.agent.turn.end"};

    /// 模型调用: EventModelCallStart / EventModelToken / EventModelCallEnd
    inline static constexpr std::string_view ModelCallStart{"agentxx.agent.model.start"};
    inline static constexpr std::string_view ModelToken{"agentxx.agent.model.token"};
    inline static constexpr std::string_view ModelCallEnd{"agentxx.agent.model.end"};

    /// 工具调用: EventToolCallStart / EventToolCallEnd
    inline static constexpr std::string_view ToolCallStart{"agentxx.agent.tool.start"};
    inline static constexpr std::string_view ToolCallEnd{"agentxx.agent.tool.end"};

    /// 通用显示输出: EventDisplay
    inline static constexpr std::string_view Display{"agentxx.agent.io.display"};

    /// 用户输入 (前端发布): EventUserInput
    inline static constexpr std::string_view UserInput{"agentxx.agent.io.user_input"};

    /// 取消控制信号: EventCancel
    inline static constexpr std::string_view Cancel{"agentxx.agent.io.cancel"};

    /// 错误: EventError
    inline static constexpr std::string_view Error{"agentxx.agent.error"};

    /// 轮次开始 (载荷 std::string 的 JSON; 插件 SDK 基类据此清除会话取消标记)
    inline static constexpr std::string_view AgentRoundStart{"agentxx.agent.round.start"};

    /// ===== 请求-响应事件 (RequestResponseStream<TReq, TResp>) =====
    /// 中断 HIL: ReqInterrupt / RespInterrupt
    inline static constexpr std::string_view Interrupt{"agentxx.agent.service.interrupt"};

    /// 权限询问: ReqPermission / RespPermission
    inline static constexpr std::string_view Permission{"agentxx.agent.service.permission"};

    /// subagent 委派 (统一批量语义): ReqSubagentBatch / RespSubagentBatch
    /// - 单任务与多任务统一为 tasks 数组 (单任务 = 1 个 task)
    inline static constexpr std::string_view Subagent{"agentxx.agent.service.subagent"};

    /// subagent 进度 (单向, 供 UI 观测): EventSubagentProgress
    inline static constexpr std::string_view SubagentProgress{"agentxx.agent.subagent.progress"};

    /// 跨 agent 查询: ReqCrossAgent / RespCrossAgent
    /// - 任一 agent (含 subagent) 可向指定 agentName 发起查询
    /// - 目标 agent 的持有者 (AgentHost) 应答
    inline static constexpr std::string_view CrossAgent{"agentxx.agent.service.crossagent"};

    /// 工具执行权限检查: ReqToolPermissionCheck / RespToolPermissionCheck
    inline static constexpr std::string_view ToolPermissionCheck{
        "agentxx.agent.service.permission.check"
    };

    /// 执行前目标复验: ReqPermissionReverify / RespPermissionReverify
    inline static constexpr std::string_view PermissionReverify{
        "agentxx.agent.service.permission.reverify"
    };

    /// 每会话文件系统隔离边界设置/清除 (单向事件): EventSetSessionIsolation /
    /// EventClearSessionIsolation
    inline static constexpr std::string_view PermissionSetIsolation{
        "agentxx.agent.service.permission.set_isolation"
    };
    inline static constexpr std::string_view PermissionClearIsolation{
        "agentxx.agent.service.permission.clear_isolation"
    };

    /// "完全授权所有权限"状态变更 (单向事件): EventPermissionFullAuthChanged
    inline static constexpr std::string_view PermissionFullAuth{
        "agentxx.agent.service.permission.full_auth"
    };

    /// subagent 工具执行 (请求-响应): ReqSubagentExecute / RespSubagentExecute
    inline static constexpr std::string_view SubagentExecute{
        "agentxx.agent.service.subagent.execute"
    };

    /// 文本 Token 估算服务 (同步服务)
    inline static constexpr std::string_view TokenCount{"agentxx.agent.service.token.count"};

    /// 上下文手动压缩 (单向事件): EventCompactContext
    inline static constexpr std::string_view SummarizationCompact{
        "agentxx.agent.service.summarization.compact"
    };

    /// ===== 单向事件: 事件中枢生命周期 =====
    /// 值到达 / 流结束: EventHubValue / EventHubEnded
    /// - 在 `agentxx.agent.hub.` 段, 不在中枢收值的 `agentxx.agent.event.` 段里,
    ///   因此不会被中枢自己的前缀订阅再吃回去
    inline static constexpr std::string_view HubValue{"agentxx.agent.hub.value"};
    inline static constexpr std::string_view HubEnded{"agentxx.agent.hub.ended"};

    /// ===== 主程序约定事件 (双端插件协作) =====
    /// agent 端有客户端接入/重连 (载荷 std::string 的 JSON)
    inline static constexpr std::string_view HostClientAttached{
        "agentxx.agent.host.client_attached"
    };
    /// agent 端已加载插件清单 (载荷 std::string 的 JSON)
    inline static constexpr std::string_view HostServerPlugins{"agentxx.agent.host.server_plugins"};
    /// client 端主程序接口集上报 (client → agent; 载荷 std::string 的 JSON)
    inline static constexpr std::string_view ClientInterfaces{
        "agentxx.client.host.client_interfaces"
    };
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
    /// 终态; 订阅方**不得**靠结果文本判断成败
    agentxx::util::ToolStatus status = agentxx::util::ToolStatus::Ok;
};

/// ===== subagent =====

struct EventSubagentProgress {
    /// subagent 会话标识 (父端 correlationId 或 subagent sessionId)
    std::string subagentId;
    std::string agentName;
    /// 进度类型: "token" | "tool_start" | "tool_end" | "turn_end"
    std::string kind;
    std::string data;
    /// agent 节点容器里的 agent 唯一 id (AgentHost 发布时填充; 旧发布方为空)
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
    // 成员按对齐/尺寸从大到小排列, 减少结构体内填充字节
    /// 拒绝原因 (供 LLM/日志参考)
    std::string reason;
    Decision    decision = Decision::Deny;
    /// 用户是否选择"记住本次选择" (权限询问卡片的 remember 勾选项):
    /// - true 时由请求方 ([PermissionMiddlewareHandle]) 按本次 target 为其子树
    ///   注册允许/拒绝规则, 后续同目标访问直接按规则处理, 不再询问
    /// - 应答者只回传用户意图, 不参与规则注册 (规则表归权限中间件所有);
    ///   取消/未提交 (结果无 values) 时应答者需保持为 false
    bool remember = false;
    /// 用户是否选择"完全授权所有权限" (权限询问卡片的 fullAuth 勾选项):
    /// - true 时由请求方 ([PermissionMiddlewareHandle]) 切换为全授权模式,
    ///   后续不再询问权限, 允许任意权限访问; 但配置文件中显式拒绝的路径仍然保持拒绝
    bool fullAuth = false;
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
    /// 结构化消息原样转发 (可选): 提供时作为子代理初始上下文 (可含 system),
    /// 与 message 二选一 (messages 优先); 用于同上下文压缩等需要完整
    /// 消息前缀的场景
    std::optional<utilxx_base::Json> messages;
    /// 指定子代理运行的 thread id (可选): 为空时使用独立 subagent 线程 id;
    /// 指定时进入"同上下文模式": 运行在指定 thread + 使用父会话当前模型 +
    /// 消息前缀原样转发, 三者共同保证与父会话命中 provider KV/prefix cache
    std::string sessionId;
    /// 子代理工具策略 (可选 json 数组, 缺省 = 子代理默认全部工具):
    /// - `[]`: 无工具
    /// - `["*"]`: 全部继承父 agent 的工具 (AgentHost 解析为父工具名白名单)
    /// - `["name1", ...]`: 仅保留列表中的工具
    std::optional<utilxx_base::Json> tools;
    /// 子代理上下文压缩 (summarization) 中间件开关 (可选):
    /// - 缺省: 继承子代理 config 默认 (即父 config 拷贝)
    /// - false: 显式禁用 (summarization 发起的压缩子代理必须禁用,
    ///   避免对原样转发的上下文前缀二次压缩破坏 KV/prefix cache 一致性)
    /// - true: 显式启用
    std::optional<bool> enableSummarization;
    /// 同上下文运行: 保留调用方给出的 system 消息 (不按子代理自身配置重建)
    /// - 用于"复用父会话可缓存前缀"的场景 (上下文压缩)
    bool keepProvidedSystemMessage = false;
    /// 同上下文运行: 继承父会话上一次请求的工具定义 (只用于保持请求前缀一致)
    bool inheritRequestTools = false;
    /// 本次运行不执行工具: 模型若仍返回 tool_calls, 忽略它们并结束本轮
    /// - 工具定义仍在请求里 (前缀一致), 但不会调用任何工具
    bool toolExecutionDisabled = false;
    /// 任务结果标识: 回填到父 toolcall 的 tool_call_id 或自定义 id;
    /// 为空时按任务序号 (1-based) 默认编号
    std::string resultId;
    /// 后台任务标记 (内部传输字段, 模型侧没有这个参数):
    /// - true: 提交为一个 async work, 立即返回 asyncId, 结果经事件回到父会话
    /// - false: 同步等待它跑完 (内部直调路径, 如上下文压缩)
    bool async = false;
    /// 结果事件名 (可选; 空 = 固定 `agentxx.agent.async.done`)
    /// - 仅内部直调需要单独一份事件名时使用; 模型侧统一用固定名 + `asyncId` 区分
    std::string eventName;
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
    bool        hasError  = false;
    bool        Canceled = false;
    std::string errorMessage;
    /// agent 节点容器派生时的 agent 唯一 id (AgentHost 填充; 节点已回收, 仅用于日志/关联)
    std::string agentId;
    /// 异步任务已受理 (started=true 时本次响应不含结果, 结果经事件到达)
    bool started = false;
    /// 后台任务 id (`async-<n>`; started=true 时有效)
    /// - 同时是该任务 agent 的 sessionId; 看/等/取消都只传这个 id
    std::string asyncId;
    /// 后台任务的结果事件名 (started=true 时有效; 固定 `agentxx.agent.async.done`)
    std::string eventName;
    /// 异步任务的子代理名 (started=true 时用于生成提示文本)
    std::string subagent;
};

struct RespSubagentBatch {
    std::vector<RespSubagentBatchItem> results;
};

/// ===== 权限检查与规则设置 =====

/// 工具执行权限检查 (service.permission.check)
struct ReqToolPermissionCheck {
    std::string       agentName;
    std::string       sessionId;
    std::string       toolName;
    utilxx_base::Json arguments;
};

struct RespToolPermissionCheck {
    bool        allow = true;
    std::string reason;
};

/// 执行前目标复验 (service.permission.reverify)
/// - 调用方: 工具调用节点在执行执行体之前 (判定阶段已记录已批准目标)
/// - 语义: 用当前参数按同一套规则重新解析目标并与已批准目标比对, 不一致即拒绝执行
struct ReqPermissionReverify {
    std::string       sessionId;
    std::string       toolName;
    std::string       toolCallId;
    utilxx_base::Json arguments;
};

struct RespPermissionReverify {
    bool        ok = true;
    std::string reason;
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

/// "完全授权所有权限"状态变更 (service.permission.full_auth; 单向事件)
///
/// 发布方: [PermissionMiddlewareHandle] (切换状态时; 来源可能是权限询问卡片的
/// 勾选, 也可能是客户端 (TUI Info 侧边栏) 经 WireSetFullAuth 的切换请求)。
/// 订阅方: 会话服务端点 (SessionServerAgentIO) —— 转发为 WirePermissionState
/// 广播给所有已连接客户端, 使多端界面同时更新。
struct EventPermissionFullAuthChanged {
    bool fullAuth = false;
};

/// ===== subagent 工具执行 =====

/// subagent 工具执行请求/响应 (service.subagent.execute)
struct ReqSubagentExecute {
    utilxx_base::Json arguments;
};

struct RespSubagentExecute {
    std::string result;
    bool        hasError = false;
    std::string errorMessage;
};

/// 上下文手动压缩 (service.summarization.compact)
struct EventCompactContext {
    std::string sessionId;
};

/// ===== 事件中枢 (EventHub) 生命周期 =====
/// - 主题 `agentxx.agent.hub.value` / `agentxx.agent.hub.ended`, 不在中枢收值的
///   `agentxx.agent.event.` 段里, 因此不会被中枢自己的前缀订阅再吃回去 (不自激)
/// - 想观测"谁发布了什么值"的插件/UI 订阅这两个主题即可
struct EventHubValue {
    /// 事件名 (= 总线主题)
    std::string name;
    /// 值 (紧凑 JSON 文本)
    std::string valueJson;
    /// 该事件已发布的值总数
    uint64_t valueCount = 0;
};

struct EventHubEnded {
    std::string name;
    /// `end` 时附带的摘要 (紧凑 JSON 文本, 无摘要时为空)
    std::string summaryJson;
    /// 该事件共发布的值数
    uint64_t valueCount = 0;
};

} // namespace events
} // namespace agentxx
