#pragma once

#include "agentxx/event/events.h"
#include "neograph/graph/cancel.h"
#include <memory>
#include <string>

namespace agentxx {
namespace events {

/// 宿主总线 (HostBus) topic 命名空间常量
/// - 宿主 (AgentHost) 是所有 agent (主 agent 与子代理平等) 的注册表与消息路由:
///   主 agent 与子代理都是宿主中的 AgentNode, 通过宿主总线交互
/// - 与会话级事件 (agent.* / io.*) 的区别: 本组 topic 跨 agent 路由
struct HostTopic {
    /// 派生子代理 (RR): ReqHostSpawn / RespHostSpawn
    /// - 任意 agent 可发起; 宿主强制嵌套深度与并发预算
    inline static constexpr std::string_view AgentSpawn{"agent.spawn"};
    /// 任意→任意 agent 消息 (RR): ReqHostMessage / RespHostMessage
    /// - 目标 agent 需注册 mailbox; 未注册时返回明确的 not-implemented 错误
    /// - 本地节点直投; 远程 agent (A2A) 由宿主转发
    inline static constexpr std::string_view AgentMessage{"agent.message"};
    /// 任意 agent 的进度事件 (单向): EventHostProgress
    inline static constexpr std::string_view AgentProgress{"agent.progress"};
    /// agent 运行结束事件 (单向): EventHostDone
    /// - 宿主据此回收 AgentNode (生命周期归宿主)
    inline static constexpr std::string_view AgentDone{"agent.done"};
};

/// ===== 派生子代理 =====

struct ReqHostSpawn {
    /// 发起方 agent id (空 = 宿主外部/根)
    std::string parentAgentId;
    /// 子代理名称 (模板名/标签)
    std::string name;
    /// 子代理系统提示 (空则用默认)
    std::string systemPrompt;
    /// 派给子代理的任务消息 (user role)
    std::string message;
    /// 指定模型 (空 = 使用配置的 subagent 模型)
    std::string modelName;
    /// 取消令牌 (可空): 取消时级联中止子代理
    std::shared_ptr<neograph::graph::CancelToken> cancelToken;
};

struct RespHostSpawn {
    /// 子代理本次运行的 agent id (节点已回收, 仅用于日志/关联)
    std::string agentId;
    /// 子代理最终输出内容
    std::string content;
    bool        hasError = false;
    std::string errorMessage;
};

/// ===== 跨 agent 消息 =====

struct ReqHostMessage {
    /// 发起方 agent id
    std::string fromAgentId;
    /// 目标 agent id
    std::string toAgentId;
    /// 查询消息 (user role 内容)
    std::string message;
    /// 取消令牌 (可空)
    std::shared_ptr<neograph::graph::CancelToken> cancelToken;
};

struct RespHostMessage {
    /// 目标 agent 的回复内容
    std::string content;
    bool        hasError = false;
    std::string errorMessage;
};

/// ===== agent 进度 / 结束 (单向) =====

struct EventHostProgress {
    /// 产出事件的 agent id
    std::string agentId;
    /// 父 agent id (根为空)
    std::string parentAgentId;
    /// 进度类型: "token" | "thinking" | "tool_start" | "tool_end" | "turn_end"
    std::string kind;
    std::string data;
};

struct EventHostDone {
    std::string agentId;
    std::string parentAgentId;
    bool        hasError = false;
    std::string errorMessage;
};

} // namespace events
} // namespace agentxx
