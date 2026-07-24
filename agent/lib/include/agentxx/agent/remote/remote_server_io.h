#pragma once

#include "agentxx/agent/agent_io.h"
#include "agentxx/agent/remote/message_transport.h"
#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "asio/any_io_executor.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agentxx {
namespace agent {

class DeepAgent;

namespace remote {

/// 服务端 AgentIO (每个 WS 连接一个)
/// - 由 DeepAgent 驱动: onDelta/onSync 入写队列; getInput/handleInterrupt 经 WS 请求-响应
/// - 单 io_context/单线程多协程: 读协程 + 单写协程 + 驱动协程
///   - onDelta 可能来自 engine 线程, 故写队列为线程安全 concurrent_channel
///   - pending_/输入分发仅在 agent executor 上访问, 无需加锁
/// - 断线: 读协程检测错误 -> 失败所有 pending -> 取消当前轮次 -> 关闭连接
class RemoteServerAgentIO : public AgentIOBase,
                            public std::enable_shared_from_this<RemoteServerAgentIO> {
public:

    struct Config {
        std::string              threadId         = "session";
        std::string              token;                       // 鉴权 token (hello 校验)
        std::vector<std::string> models;                      // hello_ack 可用模型列表
        std::chrono::seconds     authTimeout      = std::chrono::seconds{15};
        std::chrono::seconds     interruptTimeout = std::chrono::seconds{300};
        size_t                   writeQueueCap    = 4096;
    };

    RemoteServerAgentIO(
        asio::any_io_executor            ex,
        std::unique_ptr<MessageTransport> transport,
        Config                           config
    );

    ~RemoteServerAgentIO() override;

    // ----- AgentIOBase -----
    void onDelta(const Delta& delta) override;
    void onSync(const SyncPayload& payload) override;
    asio::awaitable<std::optional<std::string>> getInput() override;
    asio::awaitable<neograph::json> handleInterrupt(
        const std::string& threadId,
        const std::string& interruptNode,
        const std::string& interruptValue,
        const std::string& interruptArgJson
    ) override;

    // ----- 生命周期 -----

    /// 运行整个连接: 启动读/写协程 -> 等待鉴权 -> 驱动对话轮次 -> 关闭并等待协程退出
    asio::awaitable<void> run(const std::shared_ptr<DeepAgent>& agent);

    /// 取消当前轮次 (经 onCancel_ 回调 -> session cancelToken)
    void cancelCurrentTurn();

    bool disconnected() const noexcept {
        return disconnected_.load(std::memory_order_acquire);
    }

    const std::string& threadId() const noexcept {
        return config_.threadId;
    }

    /// 设置取消当前轮次的回调 (AgentServer 注入, 调用 session cancelToken->cancel())
    void setCancelCallback(std::function<void()> cb) {
        onCancel_ = std::move(cb);
    }

    /// 设置切换模型的回调 (AgentServer 注入, 调用 agent->selectModel)
    void setSelectModelCallback(std::function<void(const std::string&)> cb) {
        onSelectModel_ = std::move(cb);
    }

private:

    using ErrorCode = boost::system::error_code;

    using WriteQueue   = asio::experimental::concurrent_channel<void(ErrorCode, std::string)>;
    using InputChannel = asio::experimental::concurrent_channel<void(ErrorCode, std::string)>;
    using AuthChannel  = asio::experimental::concurrent_channel<void(ErrorCode, bool)>;
    using RespChannel  = asio::experimental::concurrent_channel<void(ErrorCode, neograph::json)>;
    using JoinChannel  = asio::experimental::concurrent_channel<void(ErrorCode)>;

    asio::awaitable<void> readLoop();
    asio::awaitable<void> writeLoop();

    /// 线程安全入队 (onDelta 可能来自 engine 线程)
    void enqueue(neograph::json msg);

    /// 读协程检测到断线/关闭时调用 (仅在 agent executor 上)
    void onDisconnected();

    /// 失败所有挂起的 interrupt 请求 (关闭其 resp channel)
    void failAllPending();

    /// 请求停止: 关闭写队列/输入/传输 (线程安全)
    void requestStop();

    /// 关闭并等待读写协程退出 (驱动协程末尾调用)
    asio::awaitable<void> shutdown();

    asio::any_io_executor             ex_;
    std::unique_ptr<MessageTransport> transport_;
    Config                            config_;

    std::shared_ptr<WriteQueue>   writeQueue_;
    std::shared_ptr<InputChannel> inputChannel_;
    std::shared_ptr<AuthChannel>  authChannel_;
    std::shared_ptr<JoinChannel>  joinChannel_;

    /// 挂起的 interrupt 请求: requestId -> resp channel (仅 agent executor 访问)
    std::map<int64_t, std::shared_ptr<RespChannel>> pending_;
    int64_t                                         nextReqId_ = 1;
    bool                                            authed_    = false;

    std::atomic<bool> stopped_{false};
    std::atomic<bool> disconnected_{false};

    std::function<void()>                      onCancel_;
    std::function<void(const std::string&)>    onSelectModel_;
};

} // namespace remote
} // namespace agent
} // namespace agentxx
