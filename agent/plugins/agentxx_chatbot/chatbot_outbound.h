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
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
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

    /// 发送一条 (由平台实现落地); 完成时回调结果
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
    SubmitOutcome submit(
        const ChatKey&        key,
        const OutboundRequest& req,
        int32_t               alreadySentThisTurn
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
    std::vector<std::string> recentMessageIds(std::string_view chatKey, size_t limit) const;

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
