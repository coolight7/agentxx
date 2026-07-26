#pragma once

#include "agentxx/agent/agent_io.h"
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "asio/steady_timer.hpp"
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace neograph::graph {
class CancelToken;
}

namespace agentxx {
namespace agent {

class DeepAgent;
class Session;

/// 按 threadId 持久的会话控制器 (服务端 AgentIOBase 端点)
///
/// 数据流: DeepAgent → SessionController → transport → 客户端 AgentIOBase
///         客户端 AgentIOBase → transport → SessionController → DeepAgent
///
/// - 作为 AgentIOBase 被 DeepAgent 驱动; 驱动循环独立于连接存在
/// - 持有 delta 环形缓冲, 供重连时增量重放 (seq 连续则重放, 否则回退全量 sync)
/// - 通过 transport 与客户端端点通信 (Channel 或 WS)
/// - 线程模型: 驱动循环在 agent executor 上; onDelta 可能来自 engine 线程,
///   故 delta 缓冲加锁保护
class SessionController : public AgentIOBase,
                          public std::enable_shared_from_this<SessionController> {
public:

    struct Config {
        std::string               threadId         = "session";
        std::chrono::milliseconds interruptTimeout = std::chrono::seconds{300};
        /// 断线后保持运行中轮次的宽限期; <=0 表示断线立即取消轮次
        std::chrono::milliseconds gracePeriod = std::chrono::seconds{30};
        /// delta 环形缓冲容量 (按消息数)
        size_t deltaBufferCap = 4096;
    };

    SessionController(asio::any_io_executor ex, std::weak_ptr<DeepAgent> agent, Config config);

    ~SessionController() override;

    // ----- AgentIOBase: 对端推给我的 (DeepAgent 产出, 经 transport 发给客户端) -----
    void onDelta(const Delta& delta) override;
    void onSync(const SyncPayload& payload) override;

    // ----- AgentIOBase: 对端从我这拉取的 (DeepAgent 调用) -----
    asio::awaitable<std::optional<std::string>> getInput() override;
    asio::awaitable<neograph::json>             handleInterrupt(
                    std::string_view threadId,
                    std::string_view interruptNode,
                    std::string_view interruptValue,
                    std::string_view interruptArgJson
                ) override;

    // ----- AgentIOBase: 对端发来的消息分发 -----
    void onPeerMessage(WireMessage msg) override;

    // ----- 生命周期 -----

    /// 驱动循环: 取输入 -> 执行对话轮次 -> 推送结果; 由 AgentServer 创建控制器时 co_spawn 一次
    asio::awaitable<void> run();

    /// 停止驱动循环 (关闭输入 channel/取消轮次/失败 pending)
    void stop();

    // ----- 连接管理 -----

    /// 处理客户端 hello: 按需重放 delta 或全量 sync, 发送 helloAck
    void handleHello(const WireHello& hello, std::vector<std::string> models = {});

    /// 传输断开时调用: 若轮次进行中则启动 grace 定时器
    void onDisconnect();

    // ----- 查询 -----

    std::string_view threadId() const noexcept {
        return config_.threadId;
    }

    bool turnActive() const noexcept {
        return turnActive_.load(std::memory_order_acquire);
    }

    /// 测试辅助: 强制设置轮次活动状态
    void setTurnActiveForTest(bool v) noexcept {
        turnActive_.store(v, std::memory_order_release);
    }

    /// 当前 fullHistory 的链式哈希尾 (供 hello_ack/sync)
    std::string currentTailHash();

private:

    using ErrorCode = neograph_asio_error_code;

    struct PendingInterrupt {
        std::shared_ptr<asio::experimental::concurrent_channel<void(ErrorCode, neograph::json)>> ch;
        std::string node;
        std::string value;
        std::string argJson;
    };

    using RespChannel  = asio::experimental::concurrent_channel<void(ErrorCode, neograph::json)>;
    using InputChannel = asio::experimental::concurrent_channel<void(ErrorCode, std::string)>;

    asio::awaitable<std::optional<std::string>> waitInput();

    /// 取 seq 之后的 delta (调用方须持有 bufferMutex_); nullopt 表示需全量 sync
    std::optional<std::vector<Delta>> deltasSinceLocked(uint64_t seq);

    SyncPayload              buildFullSync();
    std::shared_ptr<Session> session();

    /// 向客户端推送当前上下文统计
    void sendContextStats();

    void startGraceTimer();
    void cancelGraceTimer();
    void failAllPending();

    void resolveInterrupt(int64_t id, neograph::json result);
    void onCancel();

    asio::any_io_executor    ex_;
    std::weak_ptr<DeepAgent> agent_;
    Config                   config_;

    // delta 环形缓冲 (跨线程: onDelta 写, handleHello 读)
    mutable std::mutex bufferMutex_;
    std::deque<Delta>  deltaBuffer_;

    // 输入 channel (onPeerMessage 写, waitInput 读)
    std::shared_ptr<InputChannel> inputChannel_;

    // pending interrupt (pendingMutex_ 保护; 不在持锁期间 co_await)
    std::mutex                          pendingMutex_;
    std::map<int64_t, PendingInterrupt> pending_;
    int64_t                             nextReqId_ = 1;

    // grace 定时器
    std::mutex                          graceMutex_;
    std::shared_ptr<asio::steady_timer> graceTimer_;

    std::atomic<bool> running_{false};
    std::atomic<bool> turnActive_{false};
    std::atomic<bool> stopped_{false};
    bool              firstTurn_ = true;
};

} // namespace agent
} // namespace agentxx
