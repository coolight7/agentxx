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

struct WireTurnResult {
    std::string threadId;
    bool        hasError = false;
    std::string errorMessage;
    bool        interrupted = false;
    int32_t     startTimeMs = 0; // 轮次开始时间戳 (毫秒)
    int32_t     durationMs  = 0; // 运行时长 (毫秒)
};

struct WireContextStats {
    uint64_t contextTokens    = 0;
    uint64_t maxContextTokens = 0;
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

/// 所有可能的线消息类型 (tagged variant)
using WireMessage = std::variant<
    WireHello,
    WireHelloAck,
    WireUserInput,
    WireCancel,
    WireSelectModel,
    WireInterruptRequest,
    WireInterruptResponse,
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
    WireContextMessages>;

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
};

} // namespace agent
} // namespace agentxx
