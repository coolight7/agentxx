#pragma once

#include "agentxx/agent/agent_io.h"
#include "agentxx/agent/remote/message_transport.h"
#include "agentxx/util/ws_client.h"
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "asio/steady_timer.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace agentxx {
namespace agent {
namespace remote {

/// 客户端 AgentIO: 包裹本地 IO (TUI/stdio) 并经 WS 连接远程 DeepAgent 服务
/// - AgentIOBase 接口转发到 inner_ (本地渲染 IO)
/// - 读协程接收 server 的 delta/sync/interrupt_request/turn_result 并转发到 inner_
/// - 单写协程串行化发送; 心跳维持连接活性
/// - 支持断线自动重连: 跟踪 lastDeltaSeq/tailHash, 重连时供 server 增量重放
class RemoteClientAgentIO : public AgentIOBase,
                            public std::enable_shared_from_this<RemoteClientAgentIO> {
public:

    struct TurnResult {
        bool        hasError    = false;
        bool        interrupted = false;
        std::string errorMessage;
    };

    struct Config {
        std::chrono::milliseconds authTimeout       = std::chrono::seconds{15};
        std::chrono::seconds      heartbeatInterval = std::chrono::seconds{20};
        std::chrono::milliseconds reconnectBackoff  = std::chrono::seconds{2};
        /// 最大重连次数; <=0 表示无限重连
        int    maxReconnectAttempts = 0;
        size_t writeQueueCap        = 4096;
    };

    /// 手动传输模式 (测试/自定义传输): 由调用方提供已建立的 transport
    RemoteClientAgentIO(
        asio::any_io_executor             ex,
        std::unique_ptr<MessageTransport> transport,
        std::shared_ptr<AgentIOBase>      inner,
        Config                            config
    );

    /// 自动重连模式: 由本类管理连接 (wsConnect url), 断线自动重连
    RemoteClientAgentIO(
        asio::any_io_executor        ex,
        std::shared_ptr<AgentIOBase> inner,
        std::string                  url,
        std::string                  token,
        Config                       config,
        util::WsClientConfig         wsConfig = {}
    );

    ~RemoteClientAgentIO() override;

    // ----- AgentIOBase (转发到 inner_) -----
    void                                        onDelta(const Delta& delta) override;
    void                                        onSync(const SyncPayload& payload) override;
    asio::awaitable<std::optional<std::string>> getInput() override;
    asio::awaitable<neograph::json>             handleInterrupt(
                    const std::string& threadId,
                    const std::string& interruptNode,
                    const std::string& interruptValue,
                    const std::string& interruptArgJson
                ) override;
    void requestCancel(const std::string& threadId) override;
    void requestSelectModel(const std::string& threadId, const std::string& model) override;

    // ----- 手动模式: 调用方驱动输入循环 -----

    /// 启动读/写/心跳协程并握手鉴权; 返回是否连接成功
    asio::awaitable<bool> start(const std::string& threadId, const std::string& token);

    void sendUserInput(
        const std::string& threadId,
        const std::string& text,
        bool               isFirstMsg,
        const std::string& model = ""
    );

    /// 等待当前轮次结束; 断线时抛异常
    asio::awaitable<TurnResult> awaitTurnResult();

    void selectModel(const std::string& threadId, const std::string& model);
    void cancel(const std::string& threadId);

    /// 设置上下文统计更新回调 (server 推送 context_stats 时调用; 供更新本地 TUI 显示)
    using ContextStatsCallback
        = std::function<void(uint64_t contextTokens, uint64_t maxContextTokens)>;

    void setContextStatsCallback(ContextStatsCallback cb) {
        contextStatsCallback_ = std::move(cb);
    }

    /// 停止读/写/心跳协程并关闭连接
    asio::awaitable<void> shutdown();

    // ----- 自动模式: 内置重连 + 输入泵 -----

    /// 连接(可重连)并循环: 取本地输入 -> 发送 -> 等待轮次; 断线自动重连,
    /// 本地输入结束(nullopt)时退出
    asio::awaitable<void> runSession(const std::string& threadId, const std::string& model = "");

    bool disconnected() const noexcept {
        return disconnected_.load(std::memory_order_acquire);
    }

    uint64_t lastDeltaSeq() const noexcept {
        return lastDeltaSeq_.load(std::memory_order_acquire);
    }

private:

    using ErrorCode = boost::system::error_code;

    using WriteQueue        = asio::experimental::concurrent_channel<void(ErrorCode, std::string)>;
    using AuthChannel       = asio::experimental::concurrent_channel<void(ErrorCode, bool)>;
    using TurnChannel       = asio::experimental::concurrent_channel<void(ErrorCode, TurnResult)>;
    using JoinChannel       = asio::experimental::concurrent_channel<void(ErrorCode)>;
    using DisconnectChannel = asio::experimental::concurrent_channel<void(ErrorCode, bool)>;

    asio::awaitable<void> readLoop();
    asio::awaitable<void> writeLoop();
    asio::awaitable<void> heartbeat();

    asio::awaitable<void> handleInterruptRequest(
        int64_t            id,
        const std::string& threadId,
        const std::string& node,
        const std::string& value,
        const std::string& argJson
    );

    asio::awaitable<bool> connect(const std::string& threadId, const std::string& token);

    /// 单次连接会话: 启动协程 -> 握手 -> 输入泵 -> 关闭; 返回握手是否成功
    asio::awaitable<bool> runOnce();

    asio::awaitable<std::optional<std::string>> waitInnerInput();
    asio::awaitable<bool>                       waitDisconnect();
    asio::awaitable<void>                       sleepFor(std::chrono::milliseconds d);

    void                  resetConnState();
    void                  spawnLoops();
    asio::awaitable<void> shutdownLoops();

    void enqueue(neograph::json msg);
    /// 断开当前连接 (关闭 channel/传输/心跳); 不设置 stopped_ (由 shutdown/输入耗尽设置)
    void breakConnection();
    void onDisconnected();

    asio::any_io_executor        ex_;
    std::shared_ptr<AgentIOBase> inner_;
    Config                       config_;

    // 自动重连参数
    std::string          url_;
    std::string          token_;
    util::WsClientConfig wsConfig_;
    bool                 autoReconnect_ = false;

    std::unique_ptr<MessageTransport> transport_;

    std::shared_ptr<WriteQueue>         writeQueue_;
    std::shared_ptr<AuthChannel>        authChannel_;
    std::shared_ptr<TurnChannel>        turnChannel_;
    std::shared_ptr<JoinChannel>        joinChannel_;
    std::shared_ptr<DisconnectChannel>  disconnectChannel_;
    std::shared_ptr<asio::steady_timer> heartbeatTimer_;

    // 重连状态跟踪
    std::atomic<uint64_t> lastDeltaSeq_{0};
    std::string           lastTailHash_;
    std::string           threadId_;
    std::string           model_;
    bool                  first_ = true;

    std::atomic<bool> stopped_{false};
    std::atomic<bool> disconnected_{false};

    ContextStatsCallback contextStatsCallback_;
};

} // namespace remote
} // namespace agent
} // namespace agentxx
