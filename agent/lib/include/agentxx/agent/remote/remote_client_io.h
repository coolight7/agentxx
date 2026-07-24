#pragma once

#include "agentxx/agent/agent_io.h"
#include "agentxx/agent/remote/message_transport.h"
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace agentxx {
namespace agent {
namespace remote {

/// 客户端 AgentIO: 包裹本地 IO (TUI/stdio) 并经 WS 连接远程 DeepAgent 服务
/// - AgentIOBase 接口转发到 inner_ (本地渲染 IO)
/// - 读协程: 接收 server 的 delta/sync/interrupt_request/turn_result, 转发到 inner_
/// - 单写协程: 串行化 user_input/interrupt_response/ping 的发送 (WsClient 写路径非线程安全)
/// - 心跳: 周期发送应用级 ping, server 回 pong; client 靠 recv 超时检测 server 断线
class RemoteClientAgentIO : public AgentIOBase,
                            public std::enable_shared_from_this<RemoteClientAgentIO> {
public:

    struct TurnResult {
        bool        hasError    = false;
        bool        interrupted = false;
        std::string errorMessage;
    };

    struct Config {
        std::chrono::seconds authTimeout       = std::chrono::seconds{15};
        std::chrono::seconds heartbeatInterval = std::chrono::seconds{20};
        size_t               writeQueueCap     = 4096;
    };

    RemoteClientAgentIO(
        asio::any_io_executor             ex,
        std::unique_ptr<MessageTransport> transport,
        std::shared_ptr<AgentIOBase>      inner,
        Config                            config
    );

    ~RemoteClientAgentIO() override;

    // ----- AgentIOBase (转发到 inner_) -----
    void onDelta(const Delta& delta) override;
    void onSync(const SyncPayload& payload) override;
    asio::awaitable<std::optional<std::string>> getInput() override;
    asio::awaitable<neograph::json> handleInterrupt(
        const std::string& threadId,
        const std::string& interruptNode,
        const std::string& interruptValue,
        const std::string& interruptArgJson
    ) override;

    // ----- 客户端主流程 -----
    // 调用方 (client main) 镜像 runTuiAsync 驱动:
    //   co_await start(threadId, token);
    //   for (;;) { auto in = co_await inner->getInput(); if(!in) break;
    //              sendUserInput(threadId, *in, first, model); first=false;
    //              co_await awaitTurnResult(); }
    //   co_await shutdown();

    /// 启动读/写/心跳协程并握手鉴权; 返回是否连接成功
    asio::awaitable<bool> start(const std::string& threadId, const std::string& token);

    /// 发送一条用户输入 (触发 server 端一轮对话)
    void sendUserInput(
        const std::string& threadId,
        const std::string& text,
        bool               isFirstMsg,
        const std::string& model = ""
    );

    /// 等待当前轮次结束 (server 的 turn_result); 断线时抛异常
    asio::awaitable<TurnResult> awaitTurnResult();

    /// 请求切换模型
    void selectModel(const std::string& threadId, const std::string& model);

    /// 请求取消当前轮次
    void cancel(const std::string& threadId);

    /// 停止读/写/心跳协程并关闭连接
    asio::awaitable<void> shutdown();

    bool disconnected() const noexcept {
        return disconnected_.load(std::memory_order_acquire);
    }

private:

    using ErrorCode = boost::system::error_code;

    using WriteQueue     = asio::experimental::concurrent_channel<void(ErrorCode, std::string)>;
    using AuthChannel    = asio::experimental::concurrent_channel<void(ErrorCode, bool)>;
    using TurnChannel    = asio::experimental::concurrent_channel<void(ErrorCode, TurnResult)>;
    using JoinChannel    = asio::experimental::concurrent_channel<void(ErrorCode)>;

    asio::awaitable<void> readLoop();
    asio::awaitable<void> writeLoop();
    asio::awaitable<void> heartbeat();

    /// 处理 server 的 interrupt_request (独立协程, 经 inner_ 收集用户输入后回应)
    asio::awaitable<void> handleInterruptRequest(
        int64_t            id,
        const std::string& threadId,
        const std::string& node,
        const std::string& value,
        const std::string& argJson
    );

    asio::awaitable<bool> connect(const std::string& threadId, const std::string& token);

    void enqueue(neograph::json msg);
    void requestStop();
    void onDisconnected();

    asio::any_io_executor             ex_;
    std::unique_ptr<MessageTransport> transport_;
    std::shared_ptr<AgentIOBase>      inner_;
    Config                            config_;

    std::shared_ptr<WriteQueue>  writeQueue_;
    std::shared_ptr<AuthChannel> authChannel_;
    std::shared_ptr<TurnChannel> turnChannel_;
    std::shared_ptr<JoinChannel> joinChannel_;

    std::atomic<bool> stopped_{false};
    std::atomic<bool> disconnected_{false};
};

} // namespace remote
} // namespace agent
} // namespace agentxx
