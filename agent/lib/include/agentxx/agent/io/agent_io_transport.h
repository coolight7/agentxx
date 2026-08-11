#pragma once

#include "agentxx/agent/conversation_types.h"
#include "asio/awaitable.hpp"
#include "neograph/json.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace agentxx {
namespace agent {

// ---------------------------------------------------------------------------
// WireMessage: 两个 AgentIOBase 端点之间传递的结构化消息
// - Channel 传输: 直接传递 C++ 对象 (免序列化)
// - WS 传输: 内部负责 JSON 编解码, 调用方无感知
// ---------------------------------------------------------------------------

struct WireHello {
    std::string threadId;
    std::string token;
    uint64_t    lastSeq = 0;
    std::string tailHash;
    std::string model;
};

struct WireHelloAck {
    bool                     ok = false;
    std::string              threadId;
    std::string              tailHash;
    std::vector<std::string> models;
};

struct WireUserInput {
    std::string threadId;
    std::string text;
};

struct WireCancel {
    std::string threadId;
};

struct WireSelectModel {
    std::string threadId;
    std::string model;
};

struct WireInterruptRequest {
    int64_t     id = 0;
    std::string threadId;
    std::string node;
    std::string value;
    std::string argJson;
};

struct WireInterruptResponse {
    int64_t        id = 0;
    neograph::json result;
};

/// 服务端通知中断已过期 (超时/会话取消) (Server -> Client)
/// - id 对应 WireInterruptRequest.id; 客户端应将对应未操作的中断消息标记为过期
struct WireInterruptExpired {
    int64_t     id = 0;
    std::string threadId;
};

struct WireTurnResult {
    std::string threadId;
    bool        hasError = false;
    std::string errorMessage;
    bool        interrupted = false;
    int64_t     startTimeMs = 0; // 轮次开始时间戳 (毫秒)
    int64_t     durationMs  = 0; // 运行时长 (毫秒)
};

struct WireContextStats {
    uint64_t contextTokens    = 0;
    uint64_t maxContextTokens = 0;
    /// 当前 ModelCall 平均生成速度 (token/s, 估算值); 0 = 无流式/无数据
    double tps = 0.0;
};

struct WireError {
    int         code = 0;
    std::string message;
};

/// 服务端日志转发 (Server -> Client)
struct WireLog {
    int         level = 0;
    std::string message;
};

/// 客户端请求当前模型信息 (Client -> Server)
struct WireGetModel {
    std::string threadId;
};

/// 服务端模型信息响应 (Server -> Client)
struct WireModelInfo {
    std::string              currentModel;
    std::vector<std::string> models;
};

/// 客户端请求会话启动信息 (Client -> Server): 拉取已加载的 MCP/Skill/Memory 列表
struct WireGetAppendComponentInfo {
    std::string threadId;
};

/// 服务端加载组件响应 (Server -> Client): collectAppendComponentInfo 收集的结果
struct WireAppendComponentInfo {
    std::vector<AppendComponentNotification> notifications;
};

/// 客户端请求当前会话 LLM 上下文消息 (Client -> Server)
struct WireGetContext {
    std::string threadId;
};

/// 服务端 LLM 上下文消息响应 (Server -> Client)
struct WireContextMessages {
    neograph::json messages;
};

/// 客户端请求持久化会话列表 (Client -> Server): 会话选择弹窗数据源
/// - 不携带 threadId: 列举全部持久化会话, 与当前连接会话无关
struct WireListSessions {};

/// 服务端持久化会话列表响应 (Server -> Client)
/// - sessions 按最近活动时间降序排列 (最新在前)
struct WireSessionList {
    std::vector<SessionInfo> sessions;
};

/// 客户端请求切换当前连接的会话 (Client -> Server): 将会话端点重新绑定到
/// 目标 threadId, 服务端加载其历史并回推 Sync/模型/上下文统计 (见
/// SessionServerAgentIO::switchSession)
struct WireSwitchSession {
    std::string threadId;
};

/// 客户端记住权限选择 (Client -> Server): 将路径规则注册到服务端权限中间件,
/// 后续访问该路径或其子目录时按规则直接允许/拒绝, 不再询问
struct WireSetPermission {
    std::string threadId;
    /// 标准化绝对路径 (规则作用于该路径及其子目录, 最长前缀匹配)
    std::string path;
    /// true = 允许 (PermissionOperator::ALLOW), false = 拒绝 (PermissionOperator::DENY)
    bool allow = true;
    /// 规则作用域: FilesystemPermissionREAD(0) / FilesystemPermissionWRITE(1)
    size_t index = 0;
};

/// 所有可能的线消息类型 (tagged variant)
using WireMessage = std::variant<
    WireHello,
    WireHelloAck,
    WireUserInput,
    WireCancel,
    WireSelectModel,
    WireInterruptRequest,
    WireInterruptResponse,
    WireInterruptExpired,
    Delta,
    SyncPayload,
    WireTurnResult,
    WireContextStats,
    WireError,
    WireLog,
    WireGetModel,
    WireModelInfo,
    WireGetAppendComponentInfo,
    WireAppendComponentInfo,
    WireGetContext,
    WireContextMessages,
    WireListSessions,
    WireSessionList,
    WireSwitchSession,
    WireSetPermission>;

// ---------------------------------------------------------------------------
// AgentIOTransportBase: 两个 AgentIOBase 端点之间的协议传输层
//
// 设计原则:
// - 对调用方隐藏传输细节 (编解码/重连/心跳/序列化)
// - Channel 实现: 进程内线程间, 直接传递 WireMessage 对象, 无序列化开销
// - WS 实现: 内部处理 JSON 编解码、hello 握手、心跳、断线重连
// - 语义: 至多一个 outstanding recv + 一个 outstanding send
// ---------------------------------------------------------------------------

class AgentIOTransportBase {
public:

    virtual ~AgentIOTransportBase() = default;

    /// 发送结构化消息到对端
    /// - Channel: 直接 move 到 channel (零拷贝)
    /// - WS: 内部序列化为 JSON 帧发送
    virtual void send(WireMessage msg) = 0;

    /// 接收对端发来的下一条消息 (协程阻塞直到有消息)
    /// - 返回 nullopt 表示传输已关闭/对端断开 (不可恢复)
    /// - WS 实现内部处理重连; 重连成功时调用方无感知, 重连失败才返回 nullopt
    virtual asio::awaitable<std::optional<WireMessage>> recv() = 0;

    /// 建立连接 (WS: TCP+握手+hello; Channel: 构造时已连通, 此为 no-op)
    /// - 返回 false 表示连接/鉴权失败
    virtual asio::awaitable<bool> connect(const WireHello& hello) {
        (void)hello;
        co_return true;
    }

    /// 关闭传输 (线程安全; 使挂起的 recv 返回 nullopt)
    virtual void close() = 0;

    /// 传输是否仍然存活
    virtual bool alive() const noexcept = 0;

    /// 会话切换通知: 更新客户端重连时握手携带的 threadId, 并复位增量重放状态
    /// (新会话的 delta seq 独立编号, 旧会话的 seq/tailHash 不再适用)。
    /// - WS 客户端模式: 覆写实现 (见 WsAgentIOTransport)
    /// - Channel/服务端模式: 无重连, 默认 no-op
    /// 线程安全: 可从任意线程调用 (实现内部投递回自身 executor)
    virtual void updateReconnectThreadId(std::string /*newThreadId*/) {}
};

} // namespace agent
} // namespace agentxx
