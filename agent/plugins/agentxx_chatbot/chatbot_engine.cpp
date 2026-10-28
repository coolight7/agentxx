#include "chatbot_engine.h"

#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace agentxx {
namespace chatbot {

namespace {

constexpr const char* kLogTag = "chatbot";

using utilxx_base::Json;

/// 一轮输入里单条原文的截断说明 (原文仍在历史里, 模型可经 chatbot_history 取)
constexpr const char* kTruncatedNotice = "…（本条内容过长已截断）";

std::string formatClockText(int64_t timestampMs) {
    if (timestampMs <= 0) {
        return {};
    }
    const std::time_t seconds = static_cast<std::time_t>(timestampMs / 1000);
    std::tm           tmBuf{};
#if XX_IS_WIN_D
    if (localtime_s(&tmBuf, &seconds) != 0) {
        return {};
    }
#else
    if (localtime_r(&seconds, &tmBuf) == nullptr) {
        return {};
    }
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%m-%d %H:%M", &tmBuf) == 0) {
        return {};
    }
    return std::string{buf};
}

} // namespace

int64_t ChatbotEngine::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

void ChatbotEngine::init(
    asio::any_io_executor ex,
    ChatbotConfig         config,
    ChatbotRouter*        router,
    ChatbotOutbound*      outbound,
    Runtime               runtime
) {
    ex_       = std::move(ex);
    config_   = std::move(config);
    router_   = router;
    outbound_ = outbound;
    runtime_  = std::move(runtime);
    stopped_  = false;
    {
        const std::time_t seconds = std::time(nullptr);
        std::tm           tmBuf{};
#if XX_IS_WIN_D
        localtime_s(&tmBuf, &seconds);
#else
        localtime_r(&seconds, &tmBuf);
#endif
        char buf[32];
        if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf) != 0) {
            startupTimeText_ = std::string{buf};
        }
    }
}

void ChatbotEngine::buildRoutes() {
    if (router_ == nullptr) {
        return;
    }
    ChatbotRouter::Hooks hooks;
    hooks.open  = [this](
                     std::string_view personaName,
                     std::string_view sessionId,
                     std::string_view title
                 ) -> int64_t {
        if (!runtime_.openChannel) {
            return 0;
        }
        return runtime_.openChannel(personaName, sessionId, title);
    };
    hooks.close = [this](int64_t channelId) {
        if (runtime_.closeChannel) {
            runtime_.closeChannel(channelId);
        }
    };
    router_->build(config_, config_.behavior.maxChatsPerPersona, std::move(hooks));
}

ChatbotEngine::SessionState& ChatbotEngine::stateOf(const std::string& chatKey) {
    return sessions_[chatKey];
}

void ChatbotEngine::shutdown() {
    stopped_ = true;
    for (auto& [key, state] : sessions_) {
        (void)key;
        if (state.timer) {
            state.timer->cancel();
        }
        state.items.clear();
        state.chars = 0;
    }
}

/* ==================== 平台事件 ==================== */

void ChatbotEngine::onPlatformState(
    std::string_view accountId,
    PlatformState    state,
    std::string_view reason
) {
    Json item = Json::object();
    item["state"]  = platformStateName(state);
    item["reason"] = std::string{reason};
    setAccountStatus(accountId, item.dump());
    XX_LOGI(
        "[{}] 账号 '{}' 连接状态: {}{}{}",
        kLogTag,
        std::string{accountId},
        platformStateName(state),
        reason.empty() ? "" : " (",
        std::string{reason}
    );
}

void ChatbotEngine::setAccountStatus(std::string_view accountId, std::string json) {
    accountStatus_[std::string{accountId}] = std::move(json);
}

void ChatbotEngine::prepareAttachments(const std::string& chatKey, InboundEvent& event) {
    (void)chatKey;
    const std::string policy = config_.behavior.attachmentsFetch;
    for (auto& attachment : event.attachments) {
        if (attachment.localPath.empty() && !attachment.url.empty() && policy == "url") {
            // 直给平台 URL: 不落盘, 由模型自己拉远端
            continue;
        }
        if (!attachment.localPath.empty()) {
            continue;
        }
        // download / auto: 交给入口文件注入的下载实现 (它在插件 io 线程上用
        // HttpClient 协程做, 落盘后回填 `localPath`; 失败时保留 URL 由模型自己拉)
        if (runtime_.downloadAttachment) {
            runtime_.downloadAttachment(event.accountId, attachment);
        }
    }
}

void ChatbotEngine::appendHistory(const std::string& chatKey, const InboundEvent& event) {
    auto& state = stateOf(chatKey);
    HistoryItem item;
    item.index           = ++state.historyIndex;
    item.timestampMs     = event.timestampMs;
    item.senderName      = displayNameOf(event.senderName, event.senderId, 64);
    item.senderRole      = event.senderRole;
    item.text            = event.text;
    item.mentionedMe     = event.mentionedMe;
    item.attachmentCount = static_cast<int>(event.attachments.size());
    item.delivered       = false;
    state.history.push_back(std::move(item));
    const size_t keep = static_cast<size_t>(std::max(0, config_.behavior.historyKeep));
    while (state.history.size() > keep && keep > 0) {
        state.history.pop_front();
    }
    if (keep == 0) {
        state.history.clear();
    }
}

void ChatbotEngine::onInbound(const InboundEvent& rawEvent) {
    if (stopped_) {
        return;
    }
    InboundEvent event = rawEvent;
    if (event.accountId.empty() || event.chatId.empty()) {
        return;
    }
    ++activitySeq_;

    ChatKey key;
    key.accountId = event.accountId;
    key.type      = event.type;
    key.chatId    = event.chatId;
    const std::string chatKeyText = key.key();
    auto&             state       = stateOf(chatKeyText);
    ++state.counters.received;
    ++globalCounters_.received;

    // ---- 去重 (同一 msg_id 可能重复推送) ----
    if (!event.platformMessageId.empty()) {
        auto [it, inserted] = dedupe_.try_emplace(
            event.accountId,
            DedupeRing(static_cast<size_t>(std::max(16, config_.behavior.dedupeCapacity)))
        );
        if (it->second.seenOrAdd(event.platformMessageId)) {
            ++state.counters.duplicated;
            ++globalCounters_.duplicated;
            XX_LOGD(
                "[{}] 重复消息已忽略 (会话 '{}', msgId={})",
                kLogTag,
                chatKeyText,
                event.platformMessageId
            );
            return;
        }
    }

    // ---- 路由 ----
    auto route = router_ != nullptr ? router_->route(key) : RouteResult{};
    if (!route.found || route.mode == ChatMode::Off) {
        ++state.counters.ignored;
        ++globalCounters_.ignored;
        XX_LOGD(
            "[{}] 消息不属于任何启用的人设, 已丢弃 (会话 '{}')",
            kLogTag,
            chatKeyText
        );
        return;
    }
    ++state.counters.routed;
    ++globalCounters_.routed;

    if (!event.chatName.empty() && router_ != nullptr) {
        router_->rememberChatName(chatKeyText, event.chatName);
    }

    // ---- 原文截断 (原文仍留在历史里) ----
    const size_t inboundLimit
        = static_cast<size_t>(std::max(100, config_.behavior.inboundMaxChars));
    if (utf8CodePointCount(event.text) > inboundLimit) {
        event.text = std::string{utf8Truncate(event.text, inboundLimit)} + kTruncatedNotice;
    }

    appendHistory(chatKeyText, event);

    // ---- 关键词命令 (不进聚合缓冲) ----
    const ParsedCommand command = parseCommand(event.text, config_.commands);
    if (command.matched) {
        if (handleCommand(chatKeyText, event, command)) {
            return;
        }
    }

    // ---- 聚合缓冲 ----
    prepareAttachments(chatKeyText, event);
    const size_t chars = utf8CodePointCount(event.text);
    state.items.push_back(std::move(event));
    state.chars += chars;
    if (state.items.size() > 1) {
        ++state.counters.aggregated;
        ++globalCounters_.aggregated;
    }

    // 私聊里 `mention` 等同 `always` (单聊消息本来就是直接发给机器人的)
    ChatMode mode = route.mode;
    if (key.type == ChatType::Private && mode == ChatMode::Mention) {
        mode = ChatMode::Always;
    }
    const bool mentioned = state.items.back().mentionedMe;

    // 背压: 达到上限先投再开新窗口
    const size_t maxItems = static_cast<size_t>(std::max(1, config_.behavior.aggregateMaxItems));
    const size_t maxChars = static_cast<size_t>(std::max(100, config_.behavior.aggregateMaxChars));
    if (state.items.size() >= maxItems || state.chars >= maxChars) {
        XX_LOGW(
            "[{}] 会话 '{}' 聚合缓冲达上限 ({} 条 / {} 字符), 先投递一批",
            kLogTag,
            chatKeyText,
            state.items.size(),
            state.chars
        );
        flush(chatKeyText, "buffer_full");
        return;
    }

    if (mentioned) {
        // 被 @ 立刻唤醒 (不等窗口)
        flush(chatKeyText, "mentioned");
        return;
    }
    if (mode == ChatMode::Always) {
        // 每来一条重置窗口, 到期唤醒
        const int32_t windowMs = std::max(0, config_.behavior.aggregateWindowMs);
        if (windowMs <= 0) {
            flush(chatKeyText, "always_immediate");
            return;
        }
        if (!state.timer) {
            state.timer = std::make_shared<asio::steady_timer>(ex_);
        }
        state.timer->expires_after(std::chrono::milliseconds(windowMs));
        const std::string chatKeyCopy = chatKeyText;
        state.timer->async_wait([this, chatKeyCopy](const auto& ec) {
            if (ec || stopped_) {
                return;
            }
            flush(chatKeyCopy, "window_expired");
        });
    }
    // `mention` 模式下的普通消息: 只进缓冲, 跟下一次被 @ 一起带上去
}

/* ==================== 组装与投递 ==================== */

std::string ChatbotEngine::assembleInput(
    const std::string&                 chatKeyText,
    const std::vector<InboundEvent>&   batch
) {
    const ChatKey key        = parseChatKey(chatKeyText);
    auto&         state      = stateOf(chatKeyText);
    const auto    route      = router_ != nullptr ? router_->route(key) : RouteResult{};

    // 缓冲内按平台时间排序 (同一时间保持到达顺序), 再重新编号
    std::vector<InboundEvent> ordered = batch;
    std::stable_sort(
        ordered.begin(),
        ordered.end(),
        [](const InboundEvent& a, const InboundEvent& b) {
            return a.timestampMs < b.timestampMs;
        }
    );

    InputBlockOptions blockOpt;
    blockOpt.type     = key.type;
    blockOpt.chatName = state.items.empty() ? std::string{} : std::string{};
    for (const auto& item : ordered) {
        if (!item.chatName.empty()) {
            blockOpt.chatName = item.chatName;
            break;
        }
    }
    blockOpt.chatId             = key.chatId;
    blockOpt.includeContextHint = config_.behavior.injectContextHint;
    blockOpt.platformLabel      = "QQ";
    if (!state.pendingNotice.empty()) {
        blockOpt.hintExtra = state.pendingNotice;
        state.pendingNotice.clear();
    }

    std::vector<InputMessageView> views;
    std::vector<TurnMessage>      turnMessages;
    Json                          attachments = Json::array();
    size_t                        attachmentBudget
        = static_cast<size_t>(std::max(0, config_.behavior.attachmentsMaxPerMessage));
    std::string lastPassiveMsgId;
    std::string lastMentionMsgId;
    bool        hadMention = false;

    int index = 0;
    for (const auto& item : ordered) {
        InputMessageView view;
        view.index         = ++index;
        view.senderName    = displayNameOf(item.senderName, item.senderId, 64);
        view.senderRole    = item.senderRole;
        view.mentionedMe   = item.mentionedMe;
        view.hasQuote      = !item.quotedText.empty();
        view.quotedSender  = item.quotedSender;
        view.text          = item.text;
        if (!item.quotedText.empty()) {
            const std::string quoted = utf8TruncateWithNotice(item.quotedText, 200);
            view.text = "(回复 " + displayNameOf(item.quotedSender, std::string{}, 64) + ": "
                        + quoted + ") " + view.text;
        }
        const std::string clock = formatClockText(item.timestampMs);
        if (!clock.empty()) {
            view.text = "[" + clock + "] " + view.text;
        }

        TurnMessage turnMessage;
        turnMessage.index             = view.index;
        turnMessage.platformMessageId = item.platformMessageId;
        turnMessage.replyRef          = item.replyRef;
        turnMessage.mentionedMe       = item.mentionedMe;
        turnMessage.timestampMs       = item.timestampMs;
        turnMessages.push_back(turnMessage);

        if (!item.platformMessageId.empty()) {
            lastPassiveMsgId = item.platformMessageId;
            if (item.mentionedMe) {
                lastMentionMsgId = item.platformMessageId;
            }
        }
        hadMention = hadMention || item.mentionedMe;

        for (const auto& attachment : item.attachments) {
            AttachmentLine line;
            line.fileName = attachment.fileName.empty()
                                ? std::string{"附件"}
                                : attachment.fileName;
            const bool hasLocal = !attachment.localPath.empty();
            line.local          = hasLocal;
            line.location       = hasLocal ? attachment.localPath : attachment.url;

            const bool wantsAttachment = attachment.kind != MediaKind::File;
            if (wantsAttachment && attachmentBudget > 0) {
                Json one = Json::object();
                one["type"] = mediaKindName(attachment.kind);
                if (!line.fileName.empty()) {
                    one["display_name"] = line.fileName;
                }
                if (!attachment.mimeType.empty()) {
                    one["mime_type"] = attachment.mimeType;
                }
                one["path_or_url"] = line.location;
                if (attachment.sizeBytes > 0) {
                    one["size_bytes"] = static_cast<int64_t>(attachment.sizeBytes);
                }
                attachments.push_back(std::move(one));
                --attachmentBudget;
                view.attachments.push_back(std::move(line));
            } else if (wantsAttachment) {
                line.note = "未随消息传递 (附件数已达上限)";
                view.attachments.push_back(std::move(line));
            } else {
                line.note = "普通文件, 可用 agentxx_filesystem_read 读取";
                view.attachments.push_back(std::move(line));
            }
        }
        views.push_back(std::move(view));
    }

    Json input = Json::object();
    input["text"]    = renderInputBlock(blockOpt, views);
    input["delivery"] = "next-turn";
    if (!attachments.empty()) {
        input["attachments"] = std::move(attachments);
    }

    // 一轮记账: 工具据此定位被动回复 id 与「回哪条」
    auto& turn       = state.turn;
    turn.active      = true;
    turn.turnSerial  = ++activitySeq_;
    turn.chatKey     = chatKeyText;
    turn.personaName = route.personaName;
    turn.sentCount   = 0;
    turn.modelSent   = false;
    turn.messages    = std::move(turnMessages);
    turn.hadMention  = hadMention;
    turn.lastAssistantText.clear();
    turn.startedAtMs = nowMs();
    turn.lastPassiveMsgId = lastMentionMsgId.empty() ? lastPassiveMsgId : lastMentionMsgId;

    return input.dump();
}

void ChatbotEngine::flush(const std::string& chatKeyText, std::string_view reason) {
    if (stopped_) {
        return;
    }
    auto sessionIt = sessions_.find(chatKeyText);
    if (sessionIt == sessions_.end()) {
        return;
    }
    auto& state = sessionIt->second;
    if (state.items.empty()) {
        return;
    }
    if (state.timer) {
        state.timer->cancel();
    }

    const ChatKey key      = parseChatKey(chatKeyText);
    const auto    route    = router_ != nullptr ? router_->route(key) : RouteResult{};
    if (!route.found || route.mode == ChatMode::Off) {
        state.items.clear();
        state.chars = 0;
        return;
    }

    // 通道是否在忙: 忙就不投, 缓冲继续积累, 等本轮 TURN_END
    const int64_t existingChannel = router_ != nullptr ? router_->find(chatKeyText) : 0;
    if (existingChannel > 0 && router_ != nullptr && router_->isBusy(existingChannel)) {
        XX_LOGD(
            "[{}] 会话 '{}' 正在跑一轮, {} 条消息继续缓冲 (原因: {})",
            kLogTag,
            chatKeyText,
            state.items.size(),
            std::string{reason}
        );
        state.stale = true;
        return;
    }

    std::string err;
    const std::string title = ChatbotRouter::titleOf(
        state.items.back().chatName,
        key
    );
    const int64_t channelId = router_ != nullptr
                                  ? router_->acquire(key, route, title, ++activitySeq_, err)
                                  : 0;
    if (channelId <= 0) {
        ++state.counters.failed;
        ++globalCounters_.failed;
        state.pendingNotice = "上一条消息投递失败: " + err;
        XX_LOGW("[{}] 会话 '{}' 投递失败: {}", kLogTag, chatKeyText, err);
        // 保留缓冲 (下次有消息时一起投), 但别无限增长
        const size_t maxItems
            = static_cast<size_t>(std::max(1, config_.behavior.aggregateMaxItems));
        while (state.items.size() > maxItems) {
            state.chars -= utf8CodePointCount(state.items.front().text);
            state.items.pop_front();
            ++state.counters.dropped;
            ++globalCounters_.dropped;
        }
        if (state.items.empty()) {
            state.chars = 0;
        }
        return;
    }

    std::vector<InboundEvent> batch(state.items.begin(), state.items.end());
    state.items.clear();
    state.chars = 0;
    state.stale = false;
    ++state.counters.woken;
    ++globalCounters_.woken;

    const std::string inputJson = assembleInput(chatKeyText, batch);
    const int32_t     rc        = runtime_.sendInput ? runtime_.sendInput(channelId, inputJson) : -1;
    if (rc != 0) {
        // 投递失败: 把原文标进下一轮元信息, 并计数 (不重投, 避免重复上下文)
        state.pendingNotice = "上一条消息投递失败 (运行时错误码 " + std::to_string(rc) + ")";
        ++state.counters.failed;
        ++globalCounters_.failed;
        XX_LOGW(
            "[{}] 会话 '{}' 投递失败 (通道 {}, 错误码 {})",
            kLogTag,
            chatKeyText,
            channelId,
            rc
        );
        return;
    }
    XX_LOGI(
        "[{}] 投递 {} 条消息到会话 '{}' (通道 {}, 原因: {})",
        kLogTag,
        batch.size(),
        chatKeyText,
        channelId,
        std::string{reason}
    );
}

/* ==================== 命令 ==================== */

std::map<std::string, std::string, std::less<>> ChatbotEngine::commandVars(
    const std::string&  chatKeyText,
    const InboundEvent& event
) const {
    std::map<std::string, std::string, std::less<>> vars;
    const ChatKey key   = parseChatKey(chatKeyText);
    const auto    route = router_ != nullptr ? router_->route(key) : RouteResult{};
    auto          stateIt = sessions_.find(chatKeyText);
    const ChatCounters counters
        = stateIt == sessions_.end() ? ChatCounters{} : stateIt->second.counters;

    vars["platform"]   = "QQ";
    vars["account"]    = key.accountId;
    vars["chat"]       = key.chatId;
    vars["chatType"]   = chatTypeName(key.type);
    vars["chatName"]   = displayNameOf(event.chatName, key.chatId);
    vars["persona"]    = route.personaName;
    vars["mode"]       = chatModeName(route.mode);
    vars["sender"]     = displayNameOf(event.senderName, event.senderId, 64);
    vars["senderRole"] = event.senderRole;
    vars["text"]       = event.text;
    vars["now"]        = startupTimeText_;
    vars["uptime"]     = startupTimeText_;
    vars["received"]   = std::to_string(counters.received);
    vars["sent"]       = std::to_string(counters.sent);
    vars["dropped"]    = std::to_string(counters.dropped);
    vars["failed"]     = std::to_string(counters.failed);
    vars["duplicated"] = std::to_string(counters.duplicated);
    if (router_ != nullptr) {
        const int64_t channelId = router_->find(chatKeyText);
        vars["channelId"]       = channelId > 0 ? std::to_string(channelId) : std::string{"-"};
        if (!route.personaName.empty()) {
            const auto* persona = config_.findPersona(route.personaName);
            vars["model"]       = persona != nullptr ? persona->model : std::string{};
        }
    }
    std::string commandList;
    for (const auto& command : config_.commands) {
        if (!commandList.empty()) {
            commandList += ", ";
        }
        commandList += command.name;
    }
    vars["commandList"] = commandList;
    return vars;
}

bool ChatbotEngine::handleCommand(
    const std::string&   chatKeyText,
    const InboundEvent&  event,
    const ParsedCommand& command
) {
    const CommandDef* def = nullptr;
    for (const auto& item : config_.commands) {
        if (item.name == command.name) {
            def = &item;
            break;
        }
    }
    if (def == nullptr) {
        return false;
    }
    auto  vars = commandVars(chatKeyText, event);
    vars["args"] = command.args;
    auto& state  = stateOf(chatKeyText);
    ++state.counters.routed;
    XX_LOGI(
        "[{}] 命中命令 '{}' (mode={}, 会话 '{}')",
        kLogTag,
        def->name,
        commandModeName(def->mode),
        chatKeyText
    );

    const ChatKey key = parseChatKey(chatKeyText);

    if (def->mode == CommandMode::Local) {
        OutboundMessage message;
        message.accountId      = key.accountId;
        message.type           = key.type;
        message.chatId         = key.chatId;
        message.text           = renderTemplate(def->reply, vars);
        message.passiveReplyTo = event.platformMessageId;
        message.replyToRef     = event.replyRef;
        if (outbound_ != nullptr) {
            outbound_->enqueueDirect(key, std::move(message));
        }
        return true;
    }

    if (def->mode == CommandMode::Action) {
        if (def->action == "cancel") {
            const int64_t channelId = router_ != nullptr ? router_->find(chatKeyText) : 0;
            if (channelId > 0 && runtime_.cancel) {
                runtime_.cancel(channelId);
            }
            if (!def->reply.empty()) {
                OutboundMessage message;
                message.accountId      = key.accountId;
                message.type           = key.type;
                message.chatId         = key.chatId;
                message.text           = renderTemplate(def->reply, vars);
                message.passiveReplyTo = event.platformMessageId;
                if (outbound_ != nullptr) {
                    outbound_->enqueueDirect(key, std::move(message));
                }
            }
            return true;
        }
        // clear (本期未实现: 只提示)
        XX_LOGW("[{}] 命令动作 '{}' 本期未实现", kLogTag, def->action);
        return true;
    }

    // model: 把渲染后的提示词作为一条单独输入投给会话 (需要通道)
    const auto route = router_ != nullptr ? router_->route(key) : RouteResult{};
    std::string err;
    const int64_t channelId = router_ != nullptr
                                  ? router_->acquire(
                                        key,
                                        route,
                                        ChatbotRouter::titleOf(event.chatName, key),
                                        ++activitySeq_,
                                        err
                                    )
                                  : 0;
    if (channelId <= 0) {
        XX_LOGW("[{}] model 命令投递失败: {}", kLogTag, err);
        return true;
    }
    if (router_ != nullptr && router_->isBusy(channelId)) {
        // 正在跑一轮: 这条命令作为下一条输入排队即可 (不打断当前轮)
        XX_LOGD("[{}] model 命令在通道忙时排队 (会话 '{}')", kLogTag, chatKeyText);
    }
    Json input = Json::object();
    input["text"]     = renderTemplate(def->prompt, vars);
    input["delivery"] = "next-turn";
    if (runtime_.sendInput) {
        const int32_t rc = runtime_.sendInput(channelId, input.dump());
        if (rc != 0) {
            ++state.counters.failed;
            ++globalCounters_.failed;
            XX_LOGW("[{}] model 命令投递失败 (错误码 {})", kLogTag, rc);
        }
    }
    // 这也算一轮: 让工具能拿到被动回复 id
    auto& turn            = state.turn;
    turn.active           = true;
    turn.chatKey          = chatKeyText;
    turn.personaName      = route.personaName;
    turn.sentCount        = 0;
    turn.modelSent        = false;
    turn.hadMention       = true;
    turn.startedAtMs      = nowMs();
    turn.lastPassiveMsgId = event.platformMessageId;
    turn.messages.clear();
    turn.messages.push_back(TurnMessage{
        1,
        event.platformMessageId,
        event.replyRef,
        true,
        event.timestampMs,
    });
    return true;
}

/* ==================== 通道事件 ==================== */

void ChatbotEngine::onChannelReady(int64_t channelId) {
    if (router_ == nullptr) {
        return;
    }
    router_->markReady(channelId, true);
    const auto chatKey = router_->chatKeyOfChannel(channelId);
    if (chatKey.empty()) {
        return;
    }
    // 通道就绪时如果有积压, 立刻投 (例如首条消息在 root init 期间到达)
    auto& state = stateOf(chatKey);
    if (!state.items.empty()) {
        flush(chatKey, "channel_ready");
    }
}

void ChatbotEngine::onChannelTurnStart(int64_t channelId) {
    if (router_ == nullptr) {
        return;
    }
    router_->markBusy(channelId, true);
    const auto chatKey = router_->chatKeyOfChannel(channelId);
    if (chatKey.empty()) {
        return;
    }
    auto& state      = stateOf(chatKey);
    state.turn.active = true;
    if (state.turn.startedAtMs == 0) {
        state.turn.startedAtMs = nowMs();
    }
    ++state.counters.turns;
    ++globalCounters_.turns;
}

void ChatbotEngine::onChannelTurnEnd(
    int64_t          channelId,
    std::string_view text,
    std::string_view payloadJson
) {
    if (router_ == nullptr) {
        return;
    }
    router_->markBusy(channelId, false);
    const auto chatKey = router_->chatKeyOfChannel(channelId);
    if (chatKey.empty()) {
        return;
    }
    auto& state = stateOf(chatKey);

    Json payload;
    try {
        payload = payloadJson.empty() ? Json::object() : Json::parse(std::string{payloadJson});
    } catch (const std::exception&) {
        payload = Json::object();
    }
    const bool interrupted = payload.value("interrupted", false);
    const bool hasError    = payload.value("hasError", false);
    const double durationMs = payload.value("durationMs", 0.0);
    state.counters.lastTurnMs = static_cast<int64_t>(durationMs);
    if (!state.turn.active || state.turn.chatKey != chatKey) {
        // 不是本插件发起的一轮 (例如用户从 TUI 直接投了一条): 只更新忙状态
        state.turn.active = false;
    }

    // 本轮正文先落到记账里: 兜底发送要用它 (`text` 是本轮最后一段正文)
    if (!text.empty()) {
        state.turn.lastAssistantText.assign(text);
    }
    // 兜底发送: 模型一次都没调工具时按 fallbackReply 发最后一段正文
    if (state.turn.active && !state.turn.modelSent && !interrupted && !hasError) {
        fallbackSend(chatKey, state.turn);
    }
    state.turn.active = false;

    // 本轮结束时把积压的消息一次性带走 (要求 5 的核心行为)
    if (!state.items.empty()) {
        flush(chatKey, "turn_end");
    }
}

void ChatbotEngine::onChannelClosed(int64_t channelId, std::string_view payloadJson) {
    if (router_ == nullptr) {
        return;
    }
    const auto chatKey = router_->chatKeyOfChannel(channelId);
    router_->markReady(channelId, false);
    router_->markBusy(channelId, false);
    XX_LOGI(
        "[{}] 通道 {} 已关闭 ({}), 会话 '{}' 的上下文保留在会话库里",
        kLogTag,
        channelId,
        std::string{payloadJson},
        chatKey
    );
}

void ChatbotEngine::onChannelInterrupt(
    int64_t          channelId,
    int64_t          interruptId,
    std::string_view payloadJson
) {
    Json payload;
    try {
        payload = payloadJson.empty() ? Json::object() : Json::parse(std::string{payloadJson});
    } catch (const std::exception&) {
        payload = Json::object();
    }
    const std::string node = payload.value("node", std::string{});
    // 权限询问: 群里没人盯着弹窗, 按配置自动放行/拒绝
    if (node == "permission") {
        const bool allow = config_.behavior.humanInLoop != "auto-deny";
        const std::string values = allow
                                       ? R"({"values":{"decision":"true","remember":false,"fullAuth":false}})"
                                       : R"({"values":{"decision":"false","remember":false,"fullAuth":false}})";
        if (runtime_.respondInterrupt) {
            runtime_.respondInterrupt(channelId, interruptId, values);
        }
        XX_LOGI(
            "[{}] 权限询问已按 {} 自动应答 (通道 {})",
            kLogTag,
            allow ? "auto-allow" : "auto-deny",
            channelId
        );
        return;
    }
    // 其它中断 (未来的表单类 HIL) 一律按取消应答, 避免会话卡住
    if (runtime_.respondInterrupt) {
        runtime_.respondInterrupt(channelId, interruptId, R"({"values":{}})");
    }
    XX_LOGW(
        "[{}] 未知中断类型 '{}' 已按取消应答 (通道 {})",
        kLogTag,
        node,
        channelId
    );
}

void ChatbotEngine::onChannelDeltaText(int64_t channelId, std::string_view text) {
    if (router_ == nullptr || text.empty()) {
        return;
    }
    const auto chatKey = router_->chatKeyOfChannel(channelId);
    if (chatKey.empty()) {
        return;
    }
    auto& state = stateOf(chatKey);
    // 只留最后一段 (兜底发送用它), 不做累积 —— 累积会把整轮思考都发到群里
    state.turn.lastAssistantText.assign(text);
}

/* ==================== 兜底发送 ==================== */

void ChatbotEngine::fallbackSend(const std::string& chatKey, TurnContext& turn) {
    const std::string policy = config_.behavior.fallbackReply;
    if (policy == "never") {
        return;
    }
    if (policy == "mention" && !turn.hadMention) {
        return;
    }
    const std::string text = trimCopy(turn.lastAssistantText);
    if (text.empty()) {
        return;
    }
    const ChatKey key = parseChatKey(chatKey);
    OutboundRequest request;
    request.text         = text;
    request.passiveMsgId = defaultPassiveMsgId(chatKey);
    if (outbound_ == nullptr) {
        return;
    }
    const auto outcome = outbound_->submit(key, request, turn.sentCount);
    if (outcome.ok) {
        ++turn.sentCount;
        turn.modelSent = true;
        auto& state    = stateOf(chatKey);
        ++state.counters.sent;
        ++globalCounters_.sent;
        outbound_->noteFallback(chatKey);
        XX_LOGI("[{}] 模型未调用发送工具, 已兜底发出最后一段正文 (会话 '{}')", kLogTag, chatKey);
    } else if (!outcome.reason.empty()) {
        XX_LOGW("[{}] 兜底发送失败: {}", kLogTag, outcome.reason);
    }
}

/* ==================== 工具侧入口 ==================== */

std::string ChatbotEngine::resolveChatKey(std::string_view threadId) const {
    if (threadId.empty() || router_ == nullptr) {
        return {};
    }
    // 工具的 thread_id 就是会话 id (sessionId); 反查会话键
    const std::string chatKey = router_->chatKeyOfSession(threadId);
    if (!chatKey.empty()) {
        return chatKey;
    }
    // 也接受直接传 `account|type|chatId` (主会话里调试时用)
    const ChatKey key = parseChatKey(threadId);
    if (key.valid() && router_->route(key).found) {
        return key.key();
    }
    return {};
}

TurnContext* ChatbotEngine::turnOf(std::string_view chatKey) {
    auto it = sessions_.find(std::string{chatKey});
    return it == sessions_.end() ? nullptr : &it->second.turn;
}

const TurnContext* ChatbotEngine::turnOf(std::string_view chatKey) const {
    auto it = sessions_.find(std::string{chatKey});
    return it == sessions_.end() ? nullptr : &it->second.turn;
}

std::vector<HistoryItem> ChatbotEngine::historyOf(std::string_view chatKey, size_t limit) const {
    std::vector<HistoryItem> out;
    auto                     it = sessions_.find(std::string{chatKey});
    if (it == sessions_.end()) {
        return out;
    }
    const auto& history = it->second.history;
    const size_t take   = limit == 0 || limit >= history.size() ? history.size() : limit;
    const size_t begin  = history.size() - take;
    for (size_t i = begin; i < history.size(); ++i) {
        out.push_back(history[i]);
    }
    return out;
}

ChatCounters ChatbotEngine::countersOf(std::string_view chatKey) const {
    auto it = sessions_.find(std::string{chatKey});
    return it == sessions_.end() ? ChatCounters{} : it->second.counters;
}

bool ChatbotEngine::setMode(std::string_view chatKey, ChatMode mode, std::string& err) {
    if (router_ == nullptr) {
        err = "路由表未就绪";
        return false;
    }
    const ChatKey key = parseChatKey(chatKey);
    if (!key.valid()) {
        err = "会话键非法 (期望 `account|group|chatId`)";
        return false;
    }
    auto route = router_->route(key);
    if (!route.found) {
        err = "该会话不属于任何已配置的人设";
        return false;
    }
    router_->setModeOverride(chatKey, mode);
    // 改成 `off` 时清空缓冲 (不投旧消息)
    if (mode == ChatMode::Off) {
        auto& state = stateOf(std::string{chatKey});
        state.items.clear();
        state.chars = 0;
        if (state.timer) {
            state.timer->cancel();
        }
    }
    return true;
}

void ChatbotEngine::noteToolSend(std::string_view chatKey) {
    auto it = sessions_.find(std::string{chatKey});
    if (it == sessions_.end()) {
        return;
    }
    it->second.turn.modelSent = true;
    ++it->second.turn.sentCount;
    ++it->second.counters.sent;
    ++globalCounters_.sent;
}

std::string ChatbotEngine::defaultPassiveMsgId(std::string_view chatKey) const {
    auto it = sessions_.find(std::string{chatKey});
    if (it == sessions_.end()) {
        return {};
    }
    if (!it->second.turn.lastPassiveMsgId.empty()) {
        return it->second.turn.lastPassiveMsgId;
    }
    if (!it->second.turn.messages.empty()) {
        return it->second.turn.messages.back().platformMessageId;
    }
    return {};
}

void ChatbotEngine::noteAssistantText(std::string_view chatKey, std::string_view text) {
    auto it = sessions_.find(std::string{chatKey});
    if (it == sessions_.end()) {
        return;
    }
    it->second.turn.lastAssistantText.assign(text);
}

/* ==================== 状态输出 ==================== */

std::string ChatbotEngine::statusJson() const {
    Json out = Json::object();
    out["startupTime"] = startupTimeText_;

    Json accounts = Json::array();
    for (const auto& account : config_.accounts) {
        Json item = Json::object();
        item["id"]   = account.id;
        item["type"] = account.type;
        auto it      = accountStatus_.find(account.id);
        if (it != accountStatus_.end()) {
            try {
                item["state"] = Json::parse(it->second);
            } catch (const std::exception&) {
                item["state"] = it->second;
            }
        } else {
            item["state"] = Json{{"state", "unknown"}};
        }
        accounts.push_back(std::move(item));
    }
    out["accounts"] = std::move(accounts);

    Json personas = Json::array();
    for (const auto& persona : config_.personas) {
        Json item = Json::object();
        item["name"] = persona.name;
        item["rootId"] = router_ != nullptr ? router_->rootIdOf(persona.name) : std::string{};
        item["chats"]  = static_cast<int64_t>(
            router_ != nullptr ? router_->activeCount(persona.name) : 0
        );
        item["budget"] = config_.behavior.maxChatsPerPersona;
        personas.push_back(std::move(item));
    }
    out["personas"] = std::move(personas);

    const auto& counters = outbound_ != nullptr ? outbound_->counters() : OutboundCounters{};
    Json        outboundJson = Json::object();
    outboundJson["sent"]      = static_cast<int64_t>(counters.sent);
    outboundJson["sentParts"] = static_cast<int64_t>(counters.sentParts);
    outboundJson["dropped"]   = static_cast<int64_t>(counters.dropped);
    outboundJson["failed"]    = static_cast<int64_t>(counters.failed);
    outboundJson["activeBlocked"] = static_cast<int64_t>(counters.activeBlocked);
    outboundJson["pending"]   = static_cast<int64_t>(
        outbound_ != nullptr ? outbound_->pendingTotal() : 0
    );
    out["outbound"] = std::move(outboundJson);

    Json countersJson = Json::object();
    countersJson["received"]   = static_cast<int64_t>(globalCounters_.received);
    countersJson["duplicated"] = static_cast<int64_t>(globalCounters_.duplicated);
    countersJson["routed"]     = static_cast<int64_t>(globalCounters_.routed);
    countersJson["ignored"]    = static_cast<int64_t>(globalCounters_.ignored);
    countersJson["woken"]      = static_cast<int64_t>(globalCounters_.woken);
    countersJson["aggregated"] = static_cast<int64_t>(globalCounters_.aggregated);
    countersJson["turns"]      = static_cast<int64_t>(globalCounters_.turns);
    out["counters"]            = std::move(countersJson);

    out["channels"] = static_cast<int64_t>(router_ != nullptr ? router_->channelCount() : 0);
    return out.dump();
}

std::string ChatbotEngine::chatInfoJson(std::string_view chatKey) const {
    Json out = Json::object();
    const ChatKey key = parseChatKey(chatKey);
    out["chatKey"]    = std::string{chatKey};
    out["account"]    = key.accountId;
    out["chat"]       = key.chatId;
    out["chatType"]   = chatTypeName(key.type);
    if (router_ == nullptr) {
        return out.dump();
    }
    const auto route = router_->route(key);
    out["persona"]   = route.personaName;
    out["mode"]      = chatModeName(route.mode);
    out["wildcard"]  = route.wildcard;
    const int64_t channelId = router_->find(chatKey);
    out["channelId"] = channelId;
    if (const auto* entry = router_->entry(chatKey); entry != nullptr) {
        out["sessionId"] = entry->sessionId;
        out["ready"]     = entry->ready;
        out["busy"]      = entry->busy;
    }
    const auto counters = countersOf(chatKey);
    Json       countersJson = Json::object();
    countersJson["received"]   = static_cast<int64_t>(counters.received);
    countersJson["sent"]       = static_cast<int64_t>(counters.sent);
    countersJson["dropped"]    = static_cast<int64_t>(counters.dropped);
    countersJson["failed"]     = static_cast<int64_t>(counters.failed);
    countersJson["duplicated"] = static_cast<int64_t>(counters.duplicated);
    countersJson["turns"]      = static_cast<int64_t>(counters.turns);
    countersJson["lastTurnMs"] = counters.lastTurnMs;
    out["counters"]            = std::move(countersJson);
    auto it = sessions_.find(std::string{chatKey});
    out["buffered"] = it == sessions_.end()
                          ? 0
                          : static_cast<int64_t>(it->second.items.size());
    return out.dump();
}

} // namespace chatbot
} // namespace agentxx
