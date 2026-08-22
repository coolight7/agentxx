#pragma once

#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
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

/// 按 sessionId 持久的会话控制器 (服务端 AgentIOBase 端点)
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
        std::string sessionId = "session";
        /// 中断/权限等待客户端响应的超时; <=0 表示不限制 (无限等待用户响应)
        std::chrono::milliseconds interruptTimeout = std::chrono::milliseconds{0};
        /// 断线后保持运行中轮次的宽限期; <=0 表示断线立即取消轮次
        std::chrono::milliseconds gracePeriod = std::chrono::seconds{30};
        /// delta 环形缓冲容量 (按消息数)
        size_t deltaBufferCap = 4096;
        /// 首次接入/切换会话时同步的历史消息窗口大小 (历史分页)
        /// - 0 = 全量同步 (旧行为)
        /// - N > 0 = 仅同步末尾 N 条 (fromIndex=窗口起始下标), 客户端
        ///   (TUI) 向上滚动到窗口顶部时经 WireGetViewMessages 分页拉取
        ///   更早历史, 避免长会话恢复时全量传输
        size_t initialSyncTailCount = 0;
    };

    SessionServerAgentIO(asio::any_io_executor ex, std::weak_ptr<BaseAgent> agent, Config config);

    ~SessionServerAgentIO() override;

    // ----- AgentIOBase: 主动发送 (覆写: 新产出的 Delta 先写入重放缓冲再转发客户端) -----
    void sendToPeer(WireMessage msg) override;

    // ----- AgentIOBase: 对端从我这拉取的 (BaseAgent 调用) -----
    asio::awaitable<std::optional<std::string>> getInput() override;
    asio::awaitable<neograph::json>             handleInterrupt(
                    std::string_view sessionId,
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

    std::string_view sessionId() const noexcept {
        return config_.sessionId;
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
    /// - 重新绑定 config_.sessionId 到目标会话 (不存在时由 SessionStore 从持久化恢复创建)
    /// - 清空 delta 重放缓冲 (新会话 delta seq 独立编号)
    /// - 回推新会话的全量 Sync + 模型信息 + 上下文统计, 客户端据此恢复界面
    /// - 仅当无进行中轮次时生效 (客户端已做前置拦截, 此处双重保护)
    void switchSession(std::string newSessionId);

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
    using WakeChannel  = asio::experimental::concurrent_channel<void(ErrorCode, int)>;

    /// 取 seq 之后的 delta; nullopt 表示需全量 sync
    std::optional<std::vector<Delta>> deltasSince(uint64_t seq);

    SyncPayload              buildFullSync();
    /// 构建同步载荷; tailCount>0 时仅取末尾 tailCount 条 (历史分页尾窗)
    /// - fromIndex = 窗口起始绝对下标, totalMessages = 会话总消息数;
    ///   客户端据此展示"上方还有更早消息"并按 WireGetViewMessages 分页拉取
    /// - tailCount==0 时等价 buildFullSync() (全量, fromIndex=0)
    SyncPayload              buildTailSync(size_t tailCount);
    std::shared_ptr<Session> session();

    /// 向客户端推送当前上下文统计
    void sendContextStats();

    /// 向客户端推送当前消息队列更新
    void sendMessageQueueUpdate();
    void pushMessageQueueItem(std::string text, std::string model);
    void interruptAndRunNext();
    void clearMessageQueue();
    void removeQueueItem(std::string_view itemId);

    void startGraceTimer();
    void cancelGraceTimer();
    void failAllPending();

    /// 处理客户端历史分页请求 (WireGetViewMessages → WireViewMessagesPage)
    void handleGetViewMessages(const WireGetViewMessages& req);

    /// 实际清理逻辑 (须在 ex_ 线程执行)
    void stopImpl();

    void resolveInterrupt(int64_t id, neograph::json result);
    void onCancel();

    // ----- 插件事件转发 (仅 ex_ 线程访问) -----

    /// 订阅事件总线 `plugin.` 前缀 (run 开始时调用一次):
    /// - 插件 publish 的事件 (topic 约定 `{插件名}.{事件名}`) 原样转发为
    ///   WirePluginData (plugin/event/data), 宿主不解析载荷语义
    /// - 频率由插件自身控制; 客户端据此判断插件可用性并展示
    void subscribePluginEvents();

    asio::any_io_executor    ex_;
    std::weak_ptr<BaseAgent> agent_;
    Config                   config_;

    // delta 环形缓冲 (仅 ex_ 线程访问: sendToPeer 写, handleHello 读)
    std::deque<Delta> deltaBuffer_;

    // 服务端消息队列 (仅 ex_ 线程访问)
    std::deque<MessageQueueItem> messageQueue_;
    uint64_t                     nextQueueItemId_ = 1;
    bool                         queuePaused_     = false;
    bool                         pendingInsert_   = false;

    // 唤醒 channel (驱动循环等待新输入/事件)
    std::shared_ptr<WakeChannel> wakeChannel_;

    // pending interrupt (仅 ex_ 线程访问: handleInterrupt 写, resolveInterrupt 读)
    std::map<int64_t, PendingInterrupt> pending_;
    int64_t                             nextReqId_ = 1;

    // grace 定时器 (仅 ex_ 线程访问: startGraceTimer/cancelGraceTimer)
    std::shared_ptr<asio::steady_timer> graceTimer_;

    // ----- 插件事件转发状态 (仅 ex_ 线程访问) -----

    /// 是否已注册插件事件订阅 (防止 run() 重复注册覆盖回调)
    bool pluginSubscribed_ = false;
    /// 事件总线前缀订阅 id (0 = 未订阅)
    size_t pluginSubId_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<bool> turnActive_{false};
    std::atomic<bool> stopped_{false};
    bool              firstTurn_ = true;
};

} // namespace agent
} // namespace agentxx
