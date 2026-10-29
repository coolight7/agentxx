#include "fake_platform.h"

#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <chrono>

namespace agentxx {
namespace chatbot {

namespace {

constexpr const char* kLogTag = "chatbot";

} // namespace

FakePlatform::FakePlatform(asio::any_io_executor ex) :
    ex_(std::move(ex)) {}

PlatformCaps FakePlatform::caps() const {
    PlatformCaps caps;
    caps.markdown        = false;
    caps.passiveReply    = true;
    caps.quoteReply      = true;
    caps.multiReply      = true;
    caps.textLimit       = 0;
    caps.maxAttachments  = 5;
    caps.mentionByEventType = true;
    return caps;
}

bool FakePlatform::start(const AccountConfig& cfg, PlatformEventSink& sink) {
    std::lock_guard lock(mutex_);
    config_  = cfg;
    sink_    = &sink;
    stopped_ = false;
    online_  = true;
    if (sink.onStateChanged) {
        sink.onStateChanged(PlatformState::Online, "fake platform ready");
    }
    XX_LOGI("[{}] 假平台账号 '{}' 已就绪", kLogTag, cfg.id);
    return true;
}

void FakePlatform::stop() {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    online_  = false;
    sink_    = nullptr;
}

void FakePlatform::inject(InboundEvent event) {
    PlatformEventSink* sink = nullptr;
    {
        std::lock_guard lock(mutex_);
        sink = sink_;
    }
    if (sink == nullptr || !sink->onInbound) {
        return;
    }
    sink->onInbound(std::move(event));
}

void FakePlatform::injectText(
    ChatType    type,
    std::string chatId,
    std::string senderName,
    std::string text,
    bool        mentionedMe,
    std::string msgId,
    int64_t     timestampMs
) {
    InboundEvent event;
    {
        std::lock_guard lock(mutex_);
        event.accountId = config_.id;
    }
    event.type      = type;
    event.chatId    = std::move(chatId);
    event.chatName  = event.chatId;
    event.senderId  = senderName;
    event.senderName = std::move(senderName);
    event.senderRole = "member";
    event.text      = std::move(text);
    event.mentionedMe = mentionedMe;
    event.fromFullMode = !mentionedMe;
    event.timestampMs = timestampMs;
    {
        std::lock_guard lock(mutex_);
        event.platformMessageId
            = msgId.empty() ? "fake-msg-" + std::to_string(++nextId_) : std::move(msgId);
        if (event.chatName.empty()) {
            event.chatName = event.chatId;
        }
    }
    inject(std::move(event));
}

void FakePlatform::send(OutboundMessage msg, SendResultFn done) {
    int32_t     delay = 0;
    bool        fail  = false;
    std::string messageId;
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || !online_) {
            fail = true;
        } else if (failCount_ > 0) {
            --failCount_;
            fail = true;
        } else {
            delay = sendDelayMs_;
        }
    }
    if (fail) {
        if (done) {
            SendResult result;
            result.ok     = false;
            result.reason = "fake platform: send failed (脚本配置的失败)";
            done(result);
        }
        return;
    }

    auto deliver = [this, msg = std::move(msg), done = std::move(done)]() mutable {
        std::string id;
        std::string ref;
        {
            std::lock_guard lock(mutex_);
            id  = "fake-" + std::to_string(++nextId_);
            ref = "REFIDX_" + id;
            FakeSentRecord record;
            record.message           = msg;
            record.platformMessageId = id;
            record.refIdx            = ref;
            sent_.push_back(std::move(record));
        }
        if (done) {
            SendResult result;
            result.ok                = true;
            result.platformMessageId = std::move(id);
            result.refIdx            = std::move(ref);
            done(result);
        }
    };

    if (delay <= 0) {
        deliver();
        return;
    }
    auto timer = std::make_shared<asio::steady_timer>(ex_);
    timer->expires_after(std::chrono::milliseconds(delay));
    timer->async_wait([timer, deliver](const auto& ec) mutable {
        if (ec) {
            return;
        }
        deliver();
    });
}

std::string FakePlatform::statusJson() const {
    std::lock_guard lock(mutex_);
    utilxx_base::Json out = utilxx_base::Json::object();
    out["type"]   = "fake";
    out["online"] = online_;
    out["sent"]   = static_cast<int64_t>(sent_.size());
    return out.dump();
}

std::vector<FakeSentRecord> FakePlatform::sent() const {
    std::lock_guard lock(mutex_);
    return sent_;
}

void FakePlatform::clearSent() {
    std::lock_guard lock(mutex_);
    sent_.clear();
}

void FakePlatform::setSendDelayMs(int32_t delayMs) {
    std::lock_guard lock(mutex_);
    sendDelayMs_ = delayMs;
}

void FakePlatform::setFailCount(int32_t count) {
    std::lock_guard lock(mutex_);
    failCount_ = count;
}

bool FakePlatform::online() const {
    std::lock_guard lock(mutex_);
    return online_;
}

void FakePlatform::setOnline(bool online) {
    PlatformEventSink* sink = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (online_ == online) {
            return;
        }
        online_ = online;
        sink    = sink_;
    }
    if (sink != nullptr && sink->onStateChanged) {
        sink->onStateChanged(
            online ? PlatformState::Online : PlatformState::Reconnecting,
            "test state change"
        );
    }
}

std::string FakePlatform::debugOutbox(size_t limit, bool clear) {
    std::vector<FakeSentRecord> taken;
    {
        std::lock_guard lock(mutex_);
        if (limit == 0 || limit >= sent_.size()) {
            taken = sent_;
        } else {
            taken.assign(sent_.end() - static_cast<std::ptrdiff_t>(limit), sent_.end());
        }
        if (clear) {
            sent_.clear();
        }
    }
    utilxx_base::Json out = utilxx_base::Json::array();
    for (const auto& record : taken) {
        utilxx_base::Json item = utilxx_base::Json::object();
        item["account"]    = record.message.accountId;
        item["type"]       = chatTypeName(record.message.type);
        item["chatId"]     = record.message.chatId;
        item["text"]       = record.message.text;
        item["passiveTo"]  = record.message.passiveReplyTo;
        item["passiveSeq"] = record.message.passiveSeq;
        item["replyToRef"] = record.message.replyToRef;
        item["markdown"]   = record.message.markdown;
        item["messageId"]  = record.platformMessageId;
        item["refIdx"]     = record.refIdx;
        item["attachments"] = static_cast<int64_t>(record.message.attachments.size());
        out.push_back(std::move(item));
    }
    return out.dump();
}

RecallResult FakePlatform::recallMessage(
    const std::string& accountId,
    ChatType           type,
    const std::string& chatId,
    const std::string& messageId
) {
    (void)accountId;
    (void)type;
    (void)chatId;
    RecallResult result;
    if (messageId.empty()) {
        result.reason = "撤回需要消息 id";
        return result;
    }
    std::lock_guard lock(mutex_);
    // 只认真实发过的 id; 找不到就当平台拒绝 (与真平台行为一致)
    for (auto& record : sent_) {
        if (record.platformMessageId == messageId) {
            result.ok = true;
            return result;
        }
    }
    result.reason = "平台上找不到该消息 (可能已超过撤回时限)";
    return result;
}

std::shared_ptr<FakePlatform> makeFakePlatform(asio::any_io_executor ex) {
    return std::make_shared<FakePlatform>(std::move(ex));
}

} // namespace chatbot
} // namespace agentxx
