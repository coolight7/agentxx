#pragma once

/// 出站: 切分 / 引用 / 多条 / 频控 / 被动回复→主动降级
///
/// 一趟出站的流程 (见 `resource/history/chatbot/plan.md` §10.1):
/// 文本规整 → 切分 → 入该会话的 FIFO → 逐条发送 (选被动/主动 → 带 `msg_id` 与
/// `msg_seq` → 失败重试 → 窗口过期后降级)
///
/// 顺序保证: **每会话 FIFO** (同一个群内严格按序, 两条之间至少 `sendIntervalMs`),
/// 不同会话之间可以并行。
///
/// 线程: 全部入口只在插件 io 线程调用。
#include "chatbot_config.h"
#include "chatbot_types.h"

#include "asio/any_io_executor.hpp"
#include "asio/steady_timer.hpp"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace asio = ::boost::asio;

namespace agentxx {
namespace chatbot {

/// 一次提交的参数 (由工具/命令/兜底构造)
struct OutboundRequest {
    std::string text;
    std::vector<MediaRef> attachments;
    /// 引用哪条 (平台句柄; 空 = 不引用)
    std::string replyToRef;
    /// 被动回复 id (空 = 主动消息)
    std::string passiveMsgId;
    /// 被动回复过期后改发主动消息时 @ 谁 (平台句柄; 空 = 不 @)
    std::string mentionOpenId;
    /// 被 @ 的人的显示名 (平台不支持真 @ 时写进正文)
    std::string mentionName;
    /// 是否允许 markdown (平台能力会再收一次)
    bool markdown = false;
};

/// 一次提交的结果
struct SubmitOutcome {
    bool        ok = false;
    /// 失败原因 (可直接给模型看)
    std::string reason;
    /// 切成了几条 (含因限额被放弃的)
    size_t parts = 0;
    /// 实际入队的条数
    size_t queued = 0;
};

/// 计数 (按会话与全局各一份)
struct OutboundCounters {
    uint64_t sent         = 0;
    uint64_t sentParts    = 0;
    uint64_t fallback     = 0;
    uint64_t dropped      = 0;
    uint64_t failed       = 0;
    uint64_t activeBlocked = 0;
    uint64_t droppedOutOfWindow = 0;
    /// 被动回复窗口过期, 改发 @ 的主动消息的条数
    uint64_t passiveFallback = 0;
};

/// 一次 `submit` 的发送结果等待器 (可选)
///
/// 为什么要它: `chatbot_send_message` 得把「到底发出去没有、有没有降级」告诉模型,
/// 而真实发送是异步的 (平台 HTTP 往返)。工具跑在工作线程上等它, 插件 io 线程照常
/// 推进发送, 两边都不阻塞对方。
///
/// 结束条件 = **第一个有结论的结果**:
/// - 成功 → `ok = true` (+ 平台消息 id);
/// - 最终失败 → `ok = false` + 失败原因;
/// - 被动回复过期改发主动消息 → 只记 `warning`, 继续等重发的那次结果。
///
/// 线程: 写入方是插件 io 线程, 读取方是工具的工作线程 (mutex + condition_variable)。
class OutboundWait {
public:

    OutboundWait() = default;
    OutboundWait(const OutboundWait&)            = delete;
    OutboundWait& operator=(const OutboundWait&) = delete;

    /// 记一次成功 (带平台消息 id)
    void resolveSuccess(std::string platformMessageId);
    /// 记「按模型意愿什么都没发」(静默标记): 成功, 但没有消息出去
    void resolveSkipped();
    /// 记一次最终失败 (含原因)
    void resolveFailure(std::string reason);
    /// 记一条降级提示 (不结束等待)
    void noteWarning(std::string warning);
    /// 等工作线程用: 等到有结论返回 true, 超时返回 false
    bool waitFor(int32_t timeoutMs);

    bool        ok() const;
    int32_t     sent() const;
    std::string warning() const;
    std::string failure() const;
    std::string platformMessageId() const;

private:

    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    bool                    done_ = false;
    bool                    ok_   = false;
    int32_t                 sent_ = 0;
    std::string             warning_;
    std::string             failure_;
    std::string             platformMessageId_;
};

/// 每个会话的被动回复计数 (同一个 `msg_id` 最多能回几次)
struct PassiveUsage {
    std::string msgId;
    int32_t     used = 0;
    uint64_t    touchedSeq = 0;
};

/// 出站管理器
class ChatbotOutbound {
public:

    /// 发送一条 (由平台实现完成); 完成时回调结果
    using SendFn = std::function<void(OutboundMessage, SendResultFn)>;

    /// 平台能力 (决定 markdown / 引用 / 被动回复是否可用)
    using CapsFn = std::function<PlatformCaps(const std::string& accountId)>;

    /// 一条待发 (切分之后)
    struct Job {
        OutboundMessage message;
        /// 提交时用于诊断的会话键
        std::string chatKey;
        /// 附件是否已准备好 (URL 已被下载成本地路径)
        bool attachmentsReady = true;
        /// 本次提交的发送结果等待器 (可空; 同一次提交的各段共用)
        std::shared_ptr<OutboundWait> wait;
    };

    void init(
        asio::any_io_executor ex,
        const BehaviorConfig* behavior,
        SendFn                send,
        CapsFn                caps
    );

    /// 提交一条回复
    ///
    /// - 文本按 `messageSplitLimit` 切分; 超出 `maxRepliesPerTurn` 的部分丢弃并计入
    ///   `dropped` (调用方应把 `reason` 交给模型)
    /// - `wait`: 可空; 非空时由发送结果回调填 (工具据此把真实结果告诉模型)
    SubmitOutcome submit(
        const ChatKey&                key,
        const OutboundRequest&        req,
        int32_t                       alreadySentThisTurn,
        std::shared_ptr<OutboundWait> wait = nullptr
    );

    /// 直接入队一条已切好的消息 (命令的 local 回复走这条, 不占「一轮几条」额度)
    void enqueueDirect(const ChatKey& key, OutboundMessage message);

    /// 该会话队列里还有多少条待发
    size_t pendingOf(const ChatKey& key) const;

    /// 全部会话的待发总数
    size_t pendingTotal() const;

    /// 丢最旧的待发 (队列超限时自动调用); 返回丢弃条数
    size_t dropOldest(const std::string& chatKey, size_t count);

    /// 暂停/恢复 (stop 时不再发)
    void shutdown();

    const OutboundCounters& counters() const noexcept {
        return counters_;
    }

    /// 某会话的计数
    OutboundCounters countersOf(std::string_view chatKey) const;

    /// 该会话最近一次发送的平台消息 id (诊断/撤回用)
    ///
    /// - 按发送顺序 (旧→新) 返回最近 `limit` 条; `limit = 0` 表示全部
    /// - 因此最后一条是自己刚发的那条, 往前翻就是"倒数第二条"
    std::vector<std::string> recentMessageIds(std::string_view chatKey, size_t limit) const;

    /// 撤回成功后调用: 把该 id 从可撤回列表里移除
    ///
    /// 不去掉的话, 同一个 id 会被反复撤回 (平台侧是幂等失败或误报成功),
    /// 撤回工具按 `count` 取"最近 N 条"时会一直选到已撤回的那条。
    void forgetMessageId(std::string_view chatKey, std::string_view messageId);

    /// 该会话的被动回复用量 (诊断)
    std::vector<PassiveUsage> passiveUsageOf(std::string_view chatKey) const;

    /// 平台返回「主动消息被用户关闭」时登记一次 (留给 `chatbot_status`)
    void noteActiveBlocked(std::string_view chatKey);

    /// 登记一次兜底发送 (模型没调工具, 插件代发正文)
    void noteFallback(std::string_view chatKey);

private:

    struct SessionState {
        std::deque<Job>                      queue;
        bool                                 inFlight = false;
        /// 下一次允许发送的时间点 (毫秒时间戳, 单调时钟)
        int64_t                              nextAllowedMs = 0;
        std::shared_ptr<asio::steady_timer>  timer;
        std::vector<PassiveUsage>            passive;
        uint64_t                             seq = 0;
        OutboundCounters                     counters;
        std::deque<std::string>              recentIds;
    };

    SessionState& stateOf(const std::string& chatKey);

    /// 取下一个被动回复序号 (给不出返回 0 = 应发主动消息)
    int32_t nextPassiveSeq(SessionState& state, const std::string& msgId, bool& exhausted);

    void pump(const std::string& chatKey);

    void schedulePump(const std::string& chatKey, int64_t delayMs);

    static int64_t nowMs();

    asio::any_io_executor   ex_;
    const BehaviorConfig*   behavior_ = nullptr;
    SendFn                  send_;
    CapsFn                  caps_;
    bool                    stopped_ = false;
    std::map<std::string, SessionState, std::less<>> sessions_;
    OutboundCounters        counters_;
};

} // namespace chatbot
} // namespace agentxx
