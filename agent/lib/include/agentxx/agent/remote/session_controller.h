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

namespace remote {
/// 连接下沉接口: SessionController 经此向当前活动连接推送 server->client 消息
/// - pushMessage 必须线程安全 (onDelta 可能来自 engine 线程)
class IConnectionSink {
public:

    virtual ~IConnectionSink() = default;

    /// 线程安全入队一条 server->client JSON 消息
    virtual void pushMessage(neograph::json msg) = 0;

    virtual bool alive() const noexcept = 0;
};

/// 按 threadId 持久的会话控制器 (与连接生命周期解耦)
/// - 作为 AgentIOBase 被 DeepAgent 驱动; 驱动循环独立于连接存在
/// - 持有 delta 环形缓冲, 供重连时增量重放 (seq 连续则重放, 否则回退全量 sync)
/// - 连接 attach/detach; 断线 grace period 内保持运行中的轮次, 重连可重挂
/// - 线程模型: 驱动循环/attach/detach/resolve 在 agent executor 上;
///   onDelta 可能来自 engine 线程, 故 delta 缓冲与活动连接访问加锁保护
class SessionController : public AgentIOBase,
                          public std::enable_shared_from_this<SessionController> {
public:

    struct Config {
        std::string               threadId          = "session";
        std::chrono::milliseconds interruptTimeout  = std::chrono::seconds{300};
        std::chrono::milliseconds permissionTimeout = std::chrono::seconds{300};
        /// 断线后保持运行中轮次的宽限期; <=0 表示断线立即取消轮次
        std::chrono::milliseconds gracePeriod = std::chrono::seconds{30};
        /// delta 环形缓冲容量 (按消息数)
        size_t deltaBufferCap = 4096;
    };

    SessionController(asio::any_io_executor ex, std::weak_ptr<DeepAgent> agent, Config config);

    ~SessionController() override;

    // ----- AgentIOBase (由 DeepAgent 驱动) -----
    void                                        onDelta(const Delta& delta) override;
    void                                        onSync(const SyncPayload& payload) override;
    asio::awaitable<std::optional<std::string>> getInput() override;
    asio::awaitable<neograph::json>             handleInterrupt(
                    const std::string& threadId,
                    const std::string& interruptNode,
                    const std::string& interruptValue,
                    const std::string& interruptArgJson
                ) override;

    // ----- 生命周期 -----

    /// 驱动循环: 取输入 -> 执行对话轮次 -> 推送结果; 由 AgentServer 创建控制器时 co_spawn 一次
    asio::awaitable<void> run();

    /// 停止驱动循环 (关闭输入 channel/取消轮次/失败 pending)
    void stop();

    // ----- 连接管理 (agent executor 上调用) -----

    /// 绑定活动连接并按需重放: lastSeq>0 且缓冲覆盖则增量重放 delta, 否则全量 sync
    void attach(
        const std::shared_ptr<IConnectionSink>& conn,
        uint64_t                                lastSeq,
        const std::string&                      tailHash
    );

    /// 解绑活动连接 (conn 为当前活动时); 轮次进行中则启动 grace period
    void detach(IConnectionSink* conn);

    // ----- 来自连接读循环 (agent executor) -----

    void onUserInput(std::string text);
    void resolveInterrupt(int64_t id, neograph::json result);
    void onCancel();

    const std::string& threadId() const noexcept {
        return config_.threadId;
    }

    bool turnActive() const noexcept {
        return turnActive_.load(std::memory_order_acquire);
    }

    /// 测试辅助: 强制设置轮次活动状态 (用于 grace period 测试)
    void setTurnActiveForTest(bool v) noexcept {
        turnActive_.store(v, std::memory_order_release);
    }

    /// 当前 fullHistory 的链式哈希尾 (供 hello_ack/sync)
    std::string currentTailHash();

private:

    using ErrorCode = boost::system::error_code;

    struct PendingInterrupt {
        std::shared_ptr<asio::experimental::concurrent_channel<void(ErrorCode, neograph::json)>> ch;
        std::string node;
        std::string value;
        std::string argJson;
    };

    using RespChannel  = asio::experimental::concurrent_channel<void(ErrorCode, neograph::json)>;
    using InputChannel = asio::experimental::concurrent_channel<void(ErrorCode, std::string)>;

    asio::awaitable<std::optional<std::string>> waitInput();

    /// 线程安全: 取活动连接并推送
    void pushToActive(neograph::json msg);
    /// 取 seq 之后的 delta (调用方须持有 bufferMutex_); nullopt 表示需全量 sync
    std::optional<std::vector<Delta>> deltasSinceLocked(uint64_t seq);

    SyncPayload              buildFullSync();
    std::shared_ptr<Session> session();

    /// 向活动连接推送当前上下文统计 (轮次结束/重连同步时)
    void sendContextStats();

    void startGraceTimer();
    void cancelGraceTimer();
    void failAllPending();
    void resendPendingInterrupts(const std::shared_ptr<IConnectionSink>& conn);

    asio::any_io_executor    ex_;
    std::weak_ptr<DeepAgent> agent_;
    Config                   config_;

    // delta 环形缓冲 (跨线程)
    mutable std::mutex bufferMutex_;
    std::deque<Delta>  deltaBuffer_;

    // 活动连接 (跨线程: onDelta 读, attach/detach 写)
    mutable std::mutex             connMutex_;
    std::weak_ptr<IConnectionSink> activeConn_;

    // 输入 channel
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

} // namespace remote
} // namespace agent
} // namespace agentxx
