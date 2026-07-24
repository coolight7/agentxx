#pragma once

#include "agentxx/agent/remote/message_transport.h"
#include "agentxx/agent/remote/session_controller.h"
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace agentxx {
namespace agent {
namespace remote {

/// 服务端 WS 连接泵 (每个 WS 连接一个, 实现 IConnectionSink)
/// - 仅负责 WS 读写与协议分发; 会话驱动逻辑在 SessionController (与连接解耦)
/// - 单写协程串行化发送; 读协程分发 client 消息到 SessionController
/// - hello 鉴权后经 AuthHandler 绑定到对应 threadId 的 SessionController (含增量重放)
class RemoteServerAgentIO : public IConnectionSink,
                            public std::enable_shared_from_this<RemoteServerAgentIO> {
public:

    struct Config {
        std::string              token;             // 鉴权 token (空则不校验, 仅开发用)
        std::vector<std::string> models;            // hello_ack 可用模型列表
        std::string              defaultThreadId = "session";
        size_t                   writeQueueCap   = 4096;
    };

    /// 鉴权回调: 校验通过后绑定到 SessionController 并返回它 (附当前 tailHash); 失败返回 nullptr
    using AuthHandler = std::function<std::shared_ptr<SessionController>(
        const std::string& threadId,
        uint64_t           lastSeq,
        const std::string& tailHash,
        std::string&       outTailHash
    )>;

    RemoteServerAgentIO(
        asio::any_io_executor             ex,
        std::unique_ptr<MessageTransport> transport,
        Config                            config
    );

    ~RemoteServerAgentIO() override;

    // ----- IConnectionSink -----
    void pushMessage(neograph::json msg) override;
    bool alive() const noexcept override;

    void setAuthHandler(AuthHandler handler) {
        authHandler_ = std::move(handler);
    }

    /// 设置切换模型回调 (AgentServer 注入, 调用 agent->selectModel)
    void setSelectModelCallback(std::function<void(const std::string&)> cb) {
        onSelectModel_ = std::move(cb);
    }

    /// 运行连接: 启动读/写协程直至断线; 断线时从 SessionController detach
    asio::awaitable<void> run();

    const std::string& threadId() const noexcept {
        return threadId_;
    }

private:

    using ErrorCode  = boost::system::error_code;
    using WriteQueue = asio::experimental::concurrent_channel<void(ErrorCode, std::string)>;
    using JoinChannel = asio::experimental::concurrent_channel<void(ErrorCode)>;

    asio::awaitable<void> readLoop();
    asio::awaitable<void> writeLoop();

    /// 机会性合并相邻同类 token delta 以降低 WS 帧数 (不增加延迟: 仅合并已入队的)
    /// - 返回 {合并后的消息, 被取出但不可合并的剩余消息(留待下次发送)}
    std::pair<std::string, std::optional<std::string>> coalesceTokenDeltas(std::string first);

    void enqueue(neograph::json msg);
    /// 入队关闭哨兵(空串): 写协程发完已入队消息后关闭传输 (用于鉴权失败等需冲刷响应的场景)
    void enqueueCloseSentinel();
    void requestStop();
    void onDisconnected();

    asio::any_io_executor             ex_;
    std::unique_ptr<MessageTransport> transport_;
    Config                            config_;
    std::string                       threadId_;

    std::shared_ptr<WriteQueue> writeQueue_;
    std::shared_ptr<JoinChannel> joinChannel_;

    std::shared_ptr<SessionController> controller_;
    AuthHandler                        authHandler_;
    std::function<void(const std::string&)> onSelectModel_;

    std::atomic<bool> stopped_{false};
    std::atomic<bool> disconnected_{false};
};

} // namespace remote
} // namespace agent
} // namespace agentxx
