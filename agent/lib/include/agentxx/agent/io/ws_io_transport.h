#pragma once

#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/util/ws_client.h"
#include "asio/any_io_executor.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "asio/steady_timer.hpp"
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <string>

namespace agentxx {
namespace agent {

/// WS 传输: 跨进程/设备的 AgentIO 端点间通信
///
/// 对调用方隐藏所有网络细节:
/// - JSON 编解码: send(WireMessage) 内部序列化为 JSON 帧; recv() 内部反序列化
/// - 心跳: 定时发送 ping, 保持连接活性
/// - 重连 (客户端模式): 断线自动重连, 携带 lastSeq 供增量重放
/// - 握手: connect() 完成 hello/helloAck 鉴权
///
/// 两种使用模式:
/// - 客户端: 提供 url, 内部管理连接生命周期 (wsConnect + 重连)
/// - 服务端: 由 AgentServer 接受连接后注入已建立的 WsClient
class WsAgentIOTransport : public AgentIOTransportBase,
                           public std::enable_shared_from_this<WsAgentIOTransport> {
public:

    struct Config {
        std::chrono::seconds      heartbeatInterval    = std::chrono::seconds{20};
        std::chrono::milliseconds reconnectBackoff     = std::chrono::seconds{2};
        int                       maxReconnectAttempts = 0; // 0 = 无限
        std::chrono::milliseconds authTimeout          = std::chrono::seconds{15};
        size_t                    writeQueueCap        = 4096;
    };

    /// 客户端模式: 内部管理连接 (wsConnect + 自动重连)
    WsAgentIOTransport(
        asio::any_io_executor ex,
        std::string           url,
        std::string           token,
        Config                config,
        util::WsClientConfig  wsConfig = {}
    );

    /// 服务端模式: 注入已建立的 WsClient (由 AgentServer accept 后传入)
    WsAgentIOTransport(
        asio::any_io_executor           ex,
        std::unique_ptr<util::WsClient> client,
        Config                          config
    );

    ~WsAgentIOTransport() override;

    WsAgentIOTransport(const WsAgentIOTransport&)            = delete;
    WsAgentIOTransport& operator=(const WsAgentIOTransport&) = delete;

    // ----- AgentIOTransportBase -----

    void send(WireMessage msg) override;

    asio::awaitable<std::optional<WireMessage>> recv() override;

    asio::awaitable<bool> connect(const WireHello& hello) override;

    void close() override;

    bool alive() const noexcept override;

    /// 会话切换: 更新重连握手的 threadId 并复位增量重放状态
    /// (新会话 delta seq 独立编号, 旧会话 seq/tailHash 不再适用)。
    /// 投递到 ex_ 线程执行 (与 readLoop 重连路径同线程, 无数据竞争)
    void updateReconnectThreadId(std::string newThreadId) override;

    // ----- 序列化工具 (供 ServerWsIOTransport 复用) -----

    /// WireMessage -> JSON 文本帧
    static std::string serialize(const WireMessage& msg);

    /// JSON 文本帧 -> WireMessage (解析失败返回 nullopt)
    static std::optional<WireMessage> deserialize(std::string_view jsonText);

private:

    using ErrorCode  = neograph_asio_error_code;
    using WriteQueue = asio::experimental::concurrent_channel<void(ErrorCode, std::string)>;
    using RecvQueue  = asio::experimental::concurrent_channel<void(ErrorCode, WireMessage)>;

    asio::awaitable<void> writeLoop();
    asio::awaitable<void> readLoop();
    asio::awaitable<void> heartbeatLoop();

    /// 客户端模式: 建立 WS 连接 (含重连循环)
    asio::awaitable<bool> establishConnection();

    void spawnLoops();
    void stopLoops();

    asio::any_io_executor ex_;
    Config                config_;

    // 客户端模式参数
    std::string          url_;
    std::string          token_;
    util::WsClientConfig wsConfig_;
    bool                 clientMode_ = false;

    // 连接状态
    std::shared_ptr<util::WsClient>     wsClient_;
    std::shared_ptr<WriteQueue>         writeQueue_;
    std::shared_ptr<RecvQueue>          recvQueue_;
    std::shared_ptr<asio::steady_timer> heartbeatTimer_;
    /// 重连退避定时器 (成员化以便 close() 时取消, 打断退避等待立即退出)
    std::shared_ptr<asio::steady_timer> reconnectTimer_;

    // 重连增量重放状态
    std::atomic<uint64_t> lastDeltaSeq_{0};
    std::string           lastTailHash_;
    std::string           helloThreadId_; // 首次 connect 时的 threadId, 重连时复用

    /// 握手期间 (connect 等待 HelloAck) 到达的非 HelloAck 消息 (仅 ex_ 线程访问)
    /// - 协议上服务端先发 HelloAck 再重放, 正常为空; 防御性保留先于 HelloAck
    ///   到达的消息 (如 Log/ContextStats), 由 recv() 优先消费, 避免被握手循环丢弃
    std::deque<agentxx::agent::WireMessage> helloPending_;

    std::atomic<bool> stopped_{false};
    std::atomic<bool> connected_{false};
};

} // namespace agent
} // namespace agentxx
