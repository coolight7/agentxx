#include "chatbot_outbound.h"

#include "chatbot_core.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <chrono>

namespace agentxx {
namespace chatbot {

namespace {

constexpr const char* kLogTag = "chatbot";

/// 同一个 `msg_id` 最多被动回几次 (平台上限; 取群聊的宽松值, 单聊会更早触发降级)
constexpr int32_t kMaxPassivePerMessage = 5;

/// 被动回复用量表最多记几个 `msg_id` (超出丢最久没动的)
constexpr size_t kMaxPassiveEntries = 64;

/// 最近平台消息 id 保留条数
constexpr size_t kMaxRecentIds = 8;

/// 失败原因看起来是「被动回复窗口过期 / `msg_id` 无效」吗
///
/// 平台能自己判定的会置 `SendResult::passiveExpired`; 这里只是兜底 (其它平台/不同
/// 措辞)。判错不会造成重复发送: 被动回复失败本身就说明那条没发出去。
bool looksLikeExpiredPassive(std::string_view reason) {
    if (reason.empty()) {
        return false;
    }
    static constexpr std::string_view kKeys[] = {
        "msg_id", "msgid", "msg_seq", "过期", "失效", "无效", "超时",
        "expired", "invalid", "not exist", "不存在", "被动",
    };
    std::string lower{reason};
    for (auto& ch : lower) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    for (const auto key : kKeys) {
        std::string needle{key};
        for (auto& ch : needle) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        if (lower.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

/* ==================== OutboundWait ==================== */

void OutboundWait::resolveSuccess(std::string platformMessageId) {
    {
        std::lock_guard lock(mutex_);
        if (done_) {
            return;
        }
        done_              = true;
        ok_                = true;
        sent_              = 1;
        platformMessageId_ = std::move(platformMessageId);
    }
    cv_.notify_all();
}

void OutboundWait::resolveSkipped() {
    {
        std::lock_guard lock(mutex_);
        if (done_) {
            return;
        }
        done_ = true;
        ok_   = true;
        sent_ = 0;
    }
    cv_.notify_all();
}

void OutboundWait::resolveFailure(std::string reason) {    {
        std::lock_guard lock(mutex_);
        if (done_) {
            return;
        }
        done_    = true;
        ok_      = false;
        failure_ = std::move(reason);
    }
    cv_.notify_all();
}

void OutboundWait::noteWarning(std::string warning) {
    std::lock_guard lock(mutex_);
    if (warning.empty()) {
        return;
    }
    if (warning_.empty()) {
        warning_ = std::move(warning);
    } else {
        warning_ += "; ";
        warning_ += warning;
    }
}

bool OutboundWait::waitFor(int32_t timeoutMs) {
    std::unique_lock lock(mutex_);
    if (done_) {
        return true;
    }
    if (timeoutMs <= 0) {
        return false;
    }
    return cv_.wait_for(lock, std::chrono::milliseconds{timeoutMs}, [this]() {
        return done_;
    });
}

bool OutboundWait::ok() const {
    std::lock_guard lock(mutex_);
    return ok_;
}

int32_t OutboundWait::sent() const {
    std::lock_guard lock(mutex_);
    return sent_;
}

std::string OutboundWait::warning() const {
    std::lock_guard lock(mutex_);
    return warning_;
}

std::string OutboundWait::failure() const {
    std::lock_guard lock(mutex_);
    return failure_;
}

std::string OutboundWait::platformMessageId() const {
    std::lock_guard lock(mutex_);
    return platformMessageId_;
}

int64_t ChatbotOutbound::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

void ChatbotOutbound::init(
    asio::any_io_executor ex,
    const BehaviorConfig* behavior,
    SendFn                send,
    CapsFn                caps
) {
    ex_       = std::move(ex);
    behavior_ = behavior;
    send_     = std::move(send);
    caps_     = std::move(caps);
    stopped_  = false;
}

ChatbotOutbound::SessionState& ChatbotOutbound::stateOf(const std::string& chatKey) {
    return sessions_[chatKey];
}

size_t ChatbotOutbound::pendingOf(const ChatKey& key) const {
    auto it = sessions_.find(key.key());
    return it == sessions_.end() ? 0 : it->second.queue.size();
}

size_t ChatbotOutbound::pendingTotal() const {
    size_t total = 0;
    for (const auto& [key, state] : sessions_) {
        (void)key;
        total += state.queue.size();
    }
    return total;
}

size_t ChatbotOutbound::dropOldest(const std::string& chatKey, size_t count) {
    auto it = sessions_.find(chatKey);
    if (it == sessions_.end()) {
        return 0;
    }
    size_t dropped = 0;
    while (dropped < count && !it->second.queue.empty()) {
        it->second.queue.pop_front();
        ++dropped;
    }
    it->second.counters.dropped += dropped;
    counters_.dropped += dropped;
    if (dropped > 0) {
        XX_LOGW(
            "[{}] 出站队列溢出, 丢弃 {} 条最旧的消息 (会话 '{}')",
            kLogTag,
            dropped,
            chatKey
        );
    }
    return dropped;
}

void ChatbotOutbound::shutdown() {
    stopped_ = true;
    for (auto& [key, state] : sessions_) {
        (void)key;
        if (state.timer) {
            state.timer->cancel();
            // 同 ChatbotEngine::shutdown: 不把定时器留到 io_context 析构之后
            state.timer.reset();
        }
    }
}

OutboundCounters ChatbotOutbound::countersOf(std::string_view chatKey) const {
    auto it = sessions_.find(std::string{chatKey});
    return it == sessions_.end() ? OutboundCounters{} : it->second.counters;
}

std::vector<std::string>
    ChatbotOutbound::recentMessageIds(std::string_view chatKey, size_t limit) const {
    std::vector<std::string> out;
    auto                     it = sessions_.find(std::string{chatKey});
    if (it == sessions_.end()) {
        return out;
    }
    const auto& ids = it->second.recentIds;
    const size_t begin = ids.size() > limit ? ids.size() - limit : 0;
    for (size_t i = begin; i < ids.size(); ++i) {
        out.push_back(ids[i]);
    }
    return out;
}

void ChatbotOutbound::forgetMessageId(std::string_view chatKey, std::string_view messageId) {
    if (messageId.empty()) {
        return;
    }
    auto it = sessions_.find(std::string{chatKey});
    if (it == sessions_.end()) {
        return;
    }
    auto& ids = it->second.recentIds;
    std::erase(ids, std::string{messageId});
}

std::vector<PassiveUsage> ChatbotOutbound::passiveUsageOf(std::string_view chatKey) const {
    auto it = sessions_.find(std::string{chatKey});
    return it == sessions_.end() ? std::vector<PassiveUsage>{} : it->second.passive;
}

void ChatbotOutbound::noteActiveBlocked(std::string_view chatKey) {
    ++counters_.activeBlocked;
    stateOf(std::string{chatKey}).counters.activeBlocked++;
    XX_LOGW("[{}] 会话 '{}' 的主动消息被用户关闭了 (activeBlocked)", kLogTag, chatKey);
}

void ChatbotOutbound::noteFallback(std::string_view chatKey) {
    ++counters_.fallback;
    stateOf(std::string{chatKey}).counters.fallback++;
}

int32_t ChatbotOutbound::nextPassiveSeq(
    SessionState&      state,
    const std::string& msgId,
    bool&              exhausted
) {
    exhausted = false;
    if (msgId.empty()) {
        return 0;
    }
    for (auto& usage : state.passive) {
        if (usage.msgId == msgId) {
            if (usage.used >= kMaxPassivePerMessage) {
                exhausted = true;
                return 0;
            }
            ++usage.used;
            usage.touchedSeq = ++state.seq;
            return usage.used;
        }
    }
    if (state.passive.size() >= kMaxPassiveEntries) {
        // 丢最久没动的 (环形上限, 避免无限增长)
        auto victim = std::min_element(
            state.passive.begin(),
            state.passive.end(),
            [](const PassiveUsage& a, const PassiveUsage& b) {
                return a.touchedSeq < b.touchedSeq;
            }
        );
        if (victim != state.passive.end()) {
            state.passive.erase(victim);
        }
    }
    state.passive.push_back(PassiveUsage{msgId, 1, ++state.seq});
    return 1;
}

SubmitOutcome ChatbotOutbound::submit(
    const ChatKey&                key,
    const OutboundRequest&        req,
    int32_t                       alreadySentThisTurn,
    std::shared_ptr<OutboundWait> wait
) {
    SubmitOutcome outcome;
    if (stopped_ || behavior_ == nullptr) {
        outcome.reason = "插件已停止, 无法发送";
        if (wait) {
            wait->resolveFailure(outcome.reason);
        }
        return outcome;
    }
    const std::string chatKeyText = key.key();
    auto&             state       = stateOf(chatKeyText);

    SplitOptions splitOpt;
    splitOpt.limit  = static_cast<size_t>(std::max(0, behavior_->messageSplitLimit));
    splitOpt.maxParts = static_cast<size_t>(std::max(1, behavior_->maxSplitParts));
    splitOpt.marker = behavior_->splitMarker;

    auto parts = splitTextToParts(normalizeOutboundText(req.text), splitOpt);
    if (parts.empty() && req.attachments.empty()) {
        outcome.reason = "没有可发送的内容 (文本为空且没有附件)";
        if (wait) {
            wait->resolveFailure(outcome.reason);
        }
        return outcome;
    }
    if (parts.empty()) {
        parts.emplace_back(std::string{}); // 只有附件
    }
    outcome.parts = parts.size();

    // 一轮发几条: 超出部分不排队, 明确告诉模型合并后再发
    const int32_t maxReplies = std::max(1, behavior_->maxRepliesPerTurn);
    const int32_t allowed    = std::max(0, maxReplies - std::max(0, alreadySentThisTurn));
    if (allowed <= 0) {
        outcome.reason = "本轮已发满 " + std::to_string(maxReplies)
                         + " 条; 把想说的合并成一条再发 (或下一轮再说)";
        state.counters.dropped += parts.size();
        counters_.dropped += parts.size();
        if (wait) {
            wait->resolveFailure(outcome.reason);
        }
        return outcome;
    }

    const size_t take = std::min(parts.size(), static_cast<size_t>(allowed));
    const size_t lost = parts.size() - take;

    PlatformCaps caps;
    if (caps_) {
        caps = caps_(key.accountId);
    }
    const bool wantMarkdown = req.markdown && caps.markdown && key.type == ChatType::Group;

    size_t queued = 0;
    for (size_t i = 0; i < take; ++i) {
        Job job;
        job.chatKey              = chatKeyText;
        job.attachmentsReady     = true;
        job.message.accountId    = key.accountId;
        job.message.type         = key.type;
        job.message.chatId       = key.chatId;
        job.message.text         = std::move(parts[i]);
        job.message.replyToRef   = req.replyToRef;
        // 被动回复 id 由调用方给出 (本轮被投递的那条消息); 序号在发送前统一分配
        job.message.passiveReplyTo = req.passiveMsgId;
        // @ 目标只在被动回复过期改发主动消息时用得上 (见 pump)
        job.message.mentionOpenId = req.mentionOpenId;
        job.message.mentionName   = req.mentionName;
        job.message.markdown     = wantMarkdown;
        job.wait                 = wait;
        // 附件只跟最后一条走 (前面的分段只发文本, 附件放最后一条消息上)
        if (i + 1 == take) {
            job.message.attachments = req.attachments;
        }
        state.queue.push_back(std::move(job));
        ++queued;
    }
    if (lost > 0) {
        outcome.reason = "本轮已发满 " + std::to_string(maxReplies) + " 条, 后 "
                         + std::to_string(lost) + " 段没有发; 把想说的合并后再发";
        state.counters.dropped += lost;
        counters_.dropped += lost;
    }

    // 队列上限: 超出丢最旧 (新消息比旧消息有价值)
    const size_t maxQueue = static_cast<size_t>(std::max(1, behavior_->outboundQueueMax));
    if (state.queue.size() > maxQueue) {
        dropOldest(chatKeyText, state.queue.size() - maxQueue);
    }

    outcome.queued = queued;
    outcome.ok     = queued > 0;
    if (!outcome.ok && wait) {
        // 一条都没入队: 等待器不能悬着 (工具会等到超时)
        wait->resolveFailure(outcome.reason.empty() ? std::string{"发送被拒绝"} : outcome.reason);
    }
    pump(chatKeyText);
    return outcome;
}

void ChatbotOutbound::enqueueDirect(const ChatKey& key, OutboundMessage message) {
    if (stopped_) {
        return;
    }
    const std::string chatKeyText = key.key();
    auto&             state       = stateOf(chatKeyText);
    Job               job;
    job.chatKey          = chatKeyText;
    job.message          = std::move(message);
    job.attachmentsReady = true;
    state.queue.push_back(std::move(job));
    if (behavior_ != nullptr) {
        const size_t maxQueue = static_cast<size_t>(std::max(1, behavior_->outboundQueueMax));
        if (state.queue.size() > maxQueue) {
            dropOldest(chatKeyText, state.queue.size() - maxQueue);
        }
    }
    pump(chatKeyText);
}

void ChatbotOutbound::schedulePump(const std::string& chatKey, int64_t delayMs) {
    auto& state = stateOf(chatKey);
    if (!ex_) {
        return;
    }
    if (!state.timer) {
        state.timer = std::make_shared<asio::steady_timer>(ex_);
    }
    state.timer->expires_after(std::chrono::milliseconds(std::max<int64_t>(0, delayMs)));
    state.timer->async_wait([this, chatKey](const auto& ec) {
        if (ec || stopped_) {
            return;
        }
        pump(chatKey);
    });
}

void ChatbotOutbound::pump(const std::string& chatKey) {
    if (stopped_) {
        return;
    }
    auto it = sessions_.find(chatKey);
    if (it == sessions_.end()) {
        return;
    }
    auto& state = it->second;
    if (state.inFlight || state.queue.empty()) {
        return;
    }
    const int64_t now = nowMs();
    if (state.nextAllowedMs > now) {
        schedulePump(chatKey, state.nextAllowedMs - now);
        return;
    }
    if (!send_) {
        return;
    }

    Job job = std::move(state.queue.front());
    state.queue.pop_front();
    state.inFlight = true;

    // 被动回复序号: 只在有 msg_id 且平台支持时给
    PlatformCaps caps;
    if (caps_) {
        caps = caps_(job.message.accountId);
    }
    if (!caps.passiveReply) {
        job.message.passiveReplyTo.clear();
        job.message.passiveSeq = 0;
    } else if (!job.message.passiveReplyTo.empty()) {
        bool exhausted = false;
        const int32_t seq
            = nextPassiveSeq(state, job.message.passiveReplyTo, exhausted);
        if (exhausted) {
            // 这条消息的被动次数用完了: 按 overflowPolicy 处理
            const std::string policy
                = behavior_ != nullptr ? behavior_->overflowPolicy : std::string{"active"};
            if (policy == "truncate") {
                ++state.counters.droppedOutOfWindow;
                ++counters_.droppedOutOfWindow;
                XX_LOGW(
                    "[{}] 会话 '{}': msg_id 的被动回复次数已用尽, 按 truncate 放弃发送",
                    kLogTag,
                    chatKey
                );
                state.inFlight = false;
                pump(chatKey);
                return;
            }
            if (policy == "fail") {
                ++state.counters.failed;
                ++counters_.failed;
                XX_LOGW(
                    "[{}] 会话 '{}': msg_id 的被动回复次数已用尽, 按 fail 丢弃",
                    kLogTag,
                    chatKey
                );
                state.inFlight = false;
                pump(chatKey);
                return;
            }
            // active (默认): 改发主动消息
            job.message.passiveReplyTo.clear();
            job.message.passiveSeq = 0;
            XX_LOGI(
                "[{}] 会话 '{}': 被动回复次数用尽, 降级为主动消息",
                kLogTag,
                chatKey
            );
        } else {
            job.message.passiveSeq = seq;
        }
    }

    XX_LOGD(
        "[{}] 发送 (会话 '{}', 被动={}, seq={}, 附件 {}): {} 字符",
        kLogTag,
        chatKey,
        job.message.passiveReplyTo.empty() ? "否" : job.message.passiveReplyTo,
        job.message.passiveSeq,
        job.message.attachments.size(),
        utf8CodePointCount(job.message.text)
    );

    // 被动回复失败时要原样重发这条 (含 @ 目标): `send_` 拿走的是移动后的值, 先留一份
    // (没有被动 id 的消息不会走降级, 不必拷)
    std::shared_ptr<OutboundMessage> retryMessage;
    if (!job.message.passiveReplyTo.empty()) {
        retryMessage = std::make_shared<OutboundMessage>(job.message);
        // 被动回复本身就是对那条消息的直接回应, 不用再 @ 人;
        // @ 目标只留给"窗口过期后改发的主动消息" (见下面的结果回调)
        job.message.mentionOpenId.clear();
        job.message.mentionName.clear();
    }
    auto wait = job.wait;

    send_(
        std::move(job.message),
        [this,
         chatKey,
         wait         = std::move(wait),
         retryMessage = std::move(retryMessage)](const SendResult& result) {
            auto it2 = sessions_.find(chatKey);
            if (it2 == sessions_.end()) {
                return;
            }
            auto& state2 = it2->second;
            state2.inFlight = false;
            if (behavior_ != nullptr) {
                state2.nextAllowedMs = nowMs() + std::max(0, behavior_->sendIntervalMs);
            }
            if (result.ok) {
                ++state2.counters.sent;
                ++state2.counters.sentParts;
                ++counters_.sent;
                ++counters_.sentParts;
                if (!result.platformMessageId.empty()) {
                    state2.recentIds.push_back(result.platformMessageId);
                    while (state2.recentIds.size() > kMaxRecentIds) {
                        state2.recentIds.pop_front();
                    }
                }
                if (wait) {
                    wait->resolveSuccess(result.platformMessageId);
                }
            } else if (retryMessage != nullptr
                       && (result.passiveExpired || looksLikeExpiredPassive(result.reason))) {
                // 被动回复窗口过期 (模型跑得久, 平台已经不认这条 msg_id 了):
                // 按 overflowPolicy 处理 —— 默认改发主动消息, 并把原说话人 @ 回来
                const std::string policy
                    = behavior_ != nullptr ? behavior_->overflowPolicy : std::string{"active"};
                if (policy == "truncate") {
                    ++state2.counters.droppedOutOfWindow;
                    ++counters_.droppedOutOfWindow;
                    XX_LOGW(
                        "[{}] 会话 '{}': 被动回复窗口已过期, 按 truncate 放弃发送",
                        kLogTag,
                        chatKey
                    );
                    if (wait) {
                        wait->resolveFailure(
                            "被动回复窗口已过期 (msg_id 已失效), 按 overflowPolicy=truncate 放弃发送"
                        );
                    }
                } else if (policy == "fail") {
                    ++state2.counters.failed;
                    ++counters_.failed;
                    XX_LOGW(
                        "[{}] 会话 '{}': 被动回复窗口已过期, 按 fail 丢弃",
                        kLogTag,
                        chatKey
                    );
                    if (wait) {
                        wait->resolveFailure(
                            "被动回复窗口已过期 (msg_id 已失效, 平台返回: " + result.reason + ")"
                        );
                    }
                } else {
                    // active (默认): 清掉被动 id 改发主动消息; 平台能在正文里 @ 人时
                    // 把刚才那条消息的说话人 @ 回来, 否则只留显示名 (平台写成文本前缀)
                    auto message      = *retryMessage;
                    message.passiveReplyTo.clear();
                    message.passiveSeq = 0;
                    PlatformCaps caps;
                    if (caps_) {
                        caps = caps_(message.accountId);
                    }
                    if (!caps.mentionInContent || message.type == ChatType::Private) {
                        // 私聊本来就是对着他说; 平台不支持真 @ 时只留名字
                        message.mentionOpenId.clear();
                    }
                    ++state2.counters.passiveFallback;
                    ++counters_.passiveFallback;

                    std::string warning = "引用的 msg_id 已过期 (平台被动回复窗口已过), "
                                          "已改为主动消息重发";
                    if (!message.mentionOpenId.empty()) {
                        warning += "并 @ 了"
                                   + (message.mentionName.empty() ? std::string{"原说话人"}
                                                                  : message.mentionName);
                    } else if (!message.mentionName.empty()) {
                        warning += ", 正文以 \"@" + message.mentionName
                                   + "\" 开头 (该平台不能真的 @ 人)";
                    }
                    if (wait) {
                        wait->noteWarning(warning);
                    }
                    XX_LOGW(
                        "[{}] 会话 '{}': 被动回复窗口已过期, 改发主动消息 (@{})",
                        kLogTag,
                        chatKey,
                        message.mentionName.empty() ? std::string{"-"} : message.mentionName
                    );

                    // 插回队首: 这句回复不能被后发的消息挤到后面
                    Job retryJob;
                    retryJob.chatKey          = chatKey;
                    retryJob.attachmentsReady = true;
                    retryJob.message          = std::move(message);
                    retryJob.wait             = wait;
                    state2.queue.push_front(std::move(retryJob));
                }
            } else {
                ++state2.counters.failed;
                ++counters_.failed;
                XX_LOGW(
                    "[{}] 发送失败 (会话 '{}'): {}",
                    kLogTag,
                    chatKey,
                    result.reason
                );
                // 主动消息被用户关闭: 计数并留着不发 (下次有人说话时按
                // flushStaleOnNextMessage 决定是否补发)
                if (result.reason.find("主动") != std::string::npos
                    || result.reason.find("no permission") != std::string::npos) {
                    ++state2.counters.activeBlocked;
                    ++counters_.activeBlocked;
                }
                if (wait) {
                    wait->resolveFailure(result.reason);
                }
            }
            pump(chatKey);
        }
    );
}

} // namespace chatbot
} // namespace agentxx
