#pragma once

#include "agentxx/agent/io/agent_io.h"
#include "agentxx/expand/get_cpu_gpu_use.h"
#include "agentxx/util/stream.h"
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/experimental/concurrent_channel.hpp"
#include "asio/steady_timer.hpp"
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neograph::graph {
class CancelToken;
}

namespace agentxx {
namespace agent {

class BaseAgent;
class Session;

/// 按 threadId 持久的会话控制器 (服务端 AgentIOBase 端点)
///
/// 数据流: BaseAgent → SessionServerAgentIO → transport → 客户端 AgentIOBase
///         客户端 AgentIOBase → transport → SessionServerAgentIO → BaseAgent
///
/// - 作为 AgentIOBase 被 BaseAgent 驱动; 驱动循环独立于连接存在
/// - 持有 delta 环形缓冲, 供重连时增量重放 (seq 连续则重放, 否则回退全量 sync)
/// - 通过 transport 与客户端端点通信 (Channel 或 WS)
/// - 线程模型: 所有成员状态 (deltaBuffer_/pending_/graceTimer_) 仅在 ex_ 线程访问，
///   无需锁保护; stop() 通过 asio::dispatch(ex_) 保证在 ex_ 线程执行清理
class SessionServerAgentIO : public AgentIOBase,
                             public std::enable_shared_from_this<SessionServerAgentIO> {
public:

    struct Config {
        std::string threadId = "session";
        /// 中断/权限等待客户端响应的超时; <=0 表示不限制 (无限等待用户响应)
        std::chrono::milliseconds interruptTimeout = std::chrono::milliseconds{0};
        /// 断线后保持运行中轮次的宽限期; <=0 表示断线立即取消轮次
        std::chrono::milliseconds gracePeriod = std::chrono::seconds{30};
        /// delta 环形缓冲容量 (按消息数)
        size_t deltaBufferCap = 4096;
    };

    SessionServerAgentIO(asio::any_io_executor ex, std::weak_ptr<BaseAgent> agent, Config config);

    ~SessionServerAgentIO() override;

    // ----- AgentIOBase: 主动发送 (覆写: 新产出的 Delta 先写入重放缓冲再转发客户端) -----
    void sendToPeer(WireMessage msg) override;

    // ----- AgentIOBase: 对端从我这拉取的 (BaseAgent 调用) -----
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

    /// 中断等待超时 (供 BaseAgent 中断请求显式传递, 避免被总线默认超时截断)
    std::chrono::milliseconds interruptTimeout() const noexcept {
        return config_.interruptTimeout;
    }

    bool turnActive() const noexcept {
        return turnActive_.load(std::memory_order_acquire);
    }

    /// 驱动循环 run() 是否仍在运行 (用于停止时等待其退出)
    bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    /// 测试辅助: 强制设置轮次活动状态
    void setTurnActiveForTest(bool v) noexcept {
        turnActive_.store(v, std::memory_order_release);
    }

    /// 当前 viewMessages 的链式哈希尾 (供 hello_ack/sync)
    std::string currentTailHash();

    /// 切换本端点绑定的会话 (会话选择弹窗确认后由客户端经 WireSwitchSession 请求)
    /// - 重新绑定 config_.threadId 到目标会话 (不存在时由 SessionStore 从持久化恢复创建)
    /// - 清空 delta 重放缓冲 (新会话 delta seq 独立编号)
    /// - 回推新会话的全量 Sync + 模型信息 + 上下文统计, 客户端据此恢复界面
    /// - 仅当无进行中轮次时生效 (客户端已做前置拦截, 此处双重保护)
    void switchSession(std::string newThreadId);

protected:

    // ----- AgentIOBase: 被动接收回调 (server 端点不会从 client 收到这些消息,
    //       空实现仅用于满足纯虚契约) -----
    void onDelta(const Delta& delta) override;
    void onSync(const SyncPayload& payload) override;

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

    /// 取 seq 之后的 delta; nullopt 表示需全量 sync
    std::optional<std::vector<Delta>> deltasSince(uint64_t seq);

    SyncPayload              buildFullSync();
    std::shared_ptr<Session> session();

    /// 向客户端推送当前上下文统计
    void sendContextStats();

    void startGraceTimer();
    void cancelGraceTimer();
    void failAllPending();

    /// 实际清理逻辑 (须在 ex_ 线程执行)
    void stopImpl();

    void resolveInterrupt(int64_t id, neograph::json result);
    void onCancel();

    // ----- CodeGraph 索引进度推送 (仅 ex_ 线程访问) -----

    /// 订阅 codegraph 索引进度 (run 开始时调用一次):
    /// - 经 agent->codegraphManager() 获取 CodeGraphManager 并注册进度回调
    /// - codegraph 不可用 (未启用/未初始化) 时不订阅, TUI 不显示
    void subscribeCodegraphProgress();

    /// ex_ 线程: 收到一次索引进度更新 (由 blockingPool 线程回调 post 而来);
    /// 经 3s 节流放行后推送, 限流窗内的更新挂起由尾推定时器补发
    void onCodegraphProgress(WireCodegraphProgress prog);

    /// 尾推定时器回调: 补推限流窗内挂起的最新进度 (计为一次放行)
    void onCodegraphTail();

    /// 启动/复用尾推定时器: 窗末补推 cgPending_ (定时器在跑时 no-op)
    void armCodegraphTailTimer();

    asio::any_io_executor    ex_;
    std::weak_ptr<BaseAgent> agent_;
    Config                   config_;

    // delta 环形缓冲 (仅 ex_ 线程访问: sendToPeer 写, handleHello 读)
    std::deque<Delta> deltaBuffer_;

    // 输入 channel (concurrent_channel 内部线程安全)
    std::shared_ptr<InputChannel> inputChannel_;

    // pending interrupt (仅 ex_ 线程访问: handleInterrupt 写, resolveInterrupt 读)
    std::map<int64_t, PendingInterrupt> pending_;
    int64_t                             nextReqId_ = 1;

    // grace 定时器 (仅 ex_ 线程访问: startGraceTimer/cancelGraceTimer)
    std::shared_ptr<asio::steady_timer> graceTimer_;

    /// 系统资源监控实例 (按需惰性创建, 仅 ex_ 线程访问):
    /// CPU 占用率依赖前后两次采样差值, 客户端 (TUI) 周期请求时须跨请求复用
    /// 同一实例才能得到连续准确的占用率; query() 卸载到 blockingPool 执行,
    /// 不经此协程并发, 实例内部采样状态无竞争
    std::shared_ptr<agentxx::expand::CpuGpuMonitor> sysMonitor_;

    // ----- CodeGraph 索引进度推送状态 (仅 ex_ 线程访问) -----

    /// 是否已注册 codegraph 进度订阅 (防止 run() 重复注册覆盖回调)
    bool cgSubscribed_ = false;
    /// 3s 节流器: 索引进度推送限流 (最短 3 秒一次, 见 util/stream.h Throttle)
    agentxx::util::Throttle cgThrottle_{std::chrono::seconds{3}};
    /// 限流窗内挂起的最新进度 (尾推定时器到点补发, 保证最后一条不丢)
    std::optional<WireCodegraphProgress> cgPending_;
    /// 尾推定时器: 放行后启动, 窗末补推 cgPending_ (非空即表示定时器在跑)
    std::shared_ptr<asio::steady_timer> cgTailTimer_;

    std::atomic<bool> running_{false};
    std::atomic<bool> turnActive_{false};
    std::atomic<bool> stopped_{false};
    bool              firstTurn_ = true;
};

} // namespace agent
} // namespace agentxx
