#include "chatbot_engine.h"

#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace agentxx {
namespace chatbot {

namespace {

constexpr const char* kLogTag = "chatbot";

namespace fs = std::filesystem;

using utilxx_base::Json;

/// 一轮输入里单条原文的截断说明 (原文仍在历史里, 模型可经 chatbot_history 取)
constexpr const char* kTruncatedNotice = "…（本条内容过长已截断）";

/// 账号上线后多久仍没收到群全量消息就提示“去开平台权限”
constexpr int64_t kFullModeHintDelayMs = 180000;


/// 小文本文件读取: 成功把内容写进 `out` 并返回 true
///
/// - 只读**已经落盘**的本地路径 (远端 URL 不读)
/// - 上限由 `behavior.attachments.inlineTextMaxBytes` 给 (0 = 关)
/// - 判文本: 不含 NUL 字节且是合法 UTF-8 (二进制文件塞进输入会把后面的内容搅乱)
/// - 读失败 (权限/文件没了) 一律当"不能内联", 调用方退回只给路径
bool inlineTextFile(const MediaRef& attachment, std::string& out) {
    if (attachment.localPath.empty() || attachment.sizeBytes == 0) {
        return false;
    }
    const fs::path path = utilxx_base::utf8ToPath(attachment.localPath);
    std::error_code ec;
    const auto      size = fs::file_size(path, ec);
    if (ec || size == 0) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string content(static_cast<size_t>(size), '\0');
    in.read(content.data(), static_cast<std::streamsize>(size));
    const auto read = static_cast<size_t>(in.gcount());
    content.resize(read);
    if (content.empty()) {
        return false;
    }
    // 含 NUL 的一律当二进制 (文本可能是任意编码, 但不会是 UTF-8 + NUL)
    if (content.find('\0') != std::string::npos) {
        return false;
    }
    if (!utilxx_base::utf8IsAvail(content)) {
        return false;
    }
    out = std::move(content);
    return true;
}

/// 该附件是否值得内联 (大小与配置允许)
bool shouldInlineTextFile(const MediaRef& attachment, int32_t limitBytes) {
    if (limitBytes <= 0) {
        return false;
    }
    if (attachment.localPath.empty()) {
        return false;
    }
    if (attachment.sizeBytes > static_cast<uint64_t>(limitBytes)) {
        return false;
    }
    return true;
}

/// 取提示词的第一段非空行 (侧边栏面板只显示一行摘录)
std::string firstLineOf(std::string_view text, size_t maxCodePoints = 60) {
    std::string line;
    size_t      begin = 0;
    while (begin <= text.size()) {
        const size_t end  = text.find('\n', begin);
        std::string  part = trimCopy(text.substr(
            begin,
            end == std::string_view::npos ? std::string_view::npos : end - begin
        ));
        if (!part.empty()) {
            line = std::move(part);
            break;
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    if (line.empty()) {
        return {};
    }
    if (utf8CodePointCount(line) > maxCodePoints) {
        return std::string{utf8Truncate(line, maxCodePoints)} + "…";
    }
    return line;
}

/// 去掉输入 JSON 里的 `attachments` (投递被拒时的重试形态)
///
/// 主程序对附件有条数与单文件大小上限, 超限时把**整条输入**判为参数错误 (连正文
/// 一起丢掉)。重试只去掉附件即可: 渲染好的输入块里已经有文件名/大小/路径, 模型
/// 仍能知道收到了什么。解析失败时原样返回 (宁可报错也不要改坏报文)。
std::string withoutAttachments(const std::string& inputJson) {
    try {
        auto input = Json::parse(inputJson);
        if (!input.is_object()) {
            return inputJson;
        }
        input.erase("attachments");
        return input.dump();
    } catch (const std::exception&) {
        return inputJson;
    }
}

} // namespace

int64_t ChatbotEngine::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

std::string ChatbotEngine::messageClockText(
    int64_t timestampMs,
    int32_t timestampOffsetMinutes
) const {
    return formatTimeInZone(
        config_.behavior.timezoneSpec,
        timestampMs,
        timestampOffsetMinutes,
        "%m-%d %H:%M"
    );
}

std::string ChatbotEngine::currentTimeText(std::string_view pattern) const {
    return formatTimeInZone(
        config_.behavior.timezoneSpec,
        systemNowMs(),
        kUnknownTzOffset,
        pattern
    );
}

std::string ChatbotEngine::timezoneText() const {
    return timezoneLabel(config_.behavior.timezoneSpec, systemNowMs());
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
    // 启动时间也按配置的时区显示: 命令模板里给人看的“启动于”与消息时间用同一个时区
    startupTimeText_ = currentTimeText("%Y-%m-%d %H:%M:%S");
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
            // 连对象一起放掉: 插件 io 线程随后会停, 定时器不能留到它的 io_context
            // 析构之后 (那时 service 已失效, 再析构就是读已释放内存)
            state.timer.reset();
        }
        state.items.clear();
        state.chars = 0;
    }
    // 自检定时器同样不能留到插件 io 线程停止之后
    for (auto& [accountId, timer] : fullModeHintTimers_) {
        (void)accountId;
        if (timer) {
            timer->cancel();
            timer.reset();
        }
    }
    fullModeHintTimers_.clear();
}

/* ==================== 状态变化通知 ==================== */

void ChatbotEngine::setChangeHook(std::function<void()> hook) {
    changeHook_ = std::move(hook);
}

void ChatbotEngine::notifyChanged() {
    if (changeHook_) {
        changeHook_();
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
    // 面板上的账号状态随之变化
    notifyChanged();
    XX_LOGI(
        "[{}] 账号 '{}' 连接状态: {}{}{}",
        kLogTag,
        std::string{accountId},
        platformStateName(state),
        reason.empty() ? "" : " (",
        std::string{reason}
    );
    // 上线后起一次自检: 群全量消息一直不来就提示去开平台权限
    if (state == PlatformState::Online) {
        scheduleFullModeHint(accountId);
    }
}

bool ChatbotEngine::hasAlwaysGroupBind() const {
    for (const auto& persona : config_.personas) {
        for (const auto& bind : persona.binds) {
            if (bind.type == ChatType::Group && bind.mode == ChatMode::Always) {
                return true;
            }
        }
    }
    return false;
}

void ChatbotEngine::scheduleFullModeHint(std::string_view accountId) {
    // 只对“群聊绑定成 always”的配置提示: 其它模式本来就不靠全量消息
    if (!hasAlwaysGroupBind() || !ex_) {
        return;
    }
    const std::string id{accountId};
    const auto        seen = accountFullModeSeen_.find(id);
    if (seen != accountFullModeSeen_.end() && seen->second) {
        return; // 已经收到过全量消息, 平台权限是开的
    }
    if (fullModeHintTimers_.count(id) != 0) {
        return; // 一次会话只提示一次
    }
    auto timer = std::make_shared<asio::steady_timer>(ex_);
    timer->expires_after(std::chrono::milliseconds(kFullModeHintDelayMs));
    fullModeHintTimers_[id] = timer;
    timer->async_wait([this, id](const auto& ec) {
        if (ec || stopped_) {
            return;
        }
        const auto it = accountFullModeSeen_.find(id);
        if (it != accountFullModeSeen_.end() && it->second) {
            return;
        }
        XX_LOGW(
            "[{}] 账号 '{}' 上线 {} 分钟后仍未收到任何群全量消息, 但配置里有群聊绑定为 "
            "always: 平台没给机器人「接收所有消息」能力时, 群里只会推 @ 机器人的消息, "
            "always 在群里实际等同 mention。这个开关要由**群主**操作: 群聊右上角 -> "
            "群管理 -> 群成员 -> 点机器人头像 -> 资料页右上角「设置」->「机器人可获取的"
            "群聊消息范围」设为「获取群内全部消息」; 若没有这一行就把机器人移出群再重新"
            "添加 (开发者控制台里没有这一项), 授权后重连插件。用 chatbot_status 看 "
            "accounts[].state.fullModeMessage / atMessage 可以确认平台到底推了什么",
            kLogTag,
            id,
            kFullModeHintDelayMs / 60000
        );
    });
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
    item.index                  = ++state.historyIndex;
    item.timestampMs            = event.timestampMs;
    item.timestampOffsetMinutes = event.timestampOffsetMinutes;
    item.senderName             = displayNameOf(event.senderName, event.senderId, 64);
    item.senderRole             = event.senderRole;
    item.text                   = event.text;
    item.platformMessageId      = event.platformMessageId;
    item.mentionedMe            = event.mentionedMe();
    item.attachmentCount        = static_cast<int>(event.attachments.size());
    item.delivered              = false;
    state.history.push_back(std::move(item));
    const size_t keep = static_cast<size_t>(std::max(0, config_.behavior.historyKeep));
    while (state.history.size() > keep && keep > 0) {
        state.history.pop_front();
    }
    if (keep == 0) {
        state.history.clear();
    }
}

bool ChatbotEngine::mergeDuplicateInto(
    const std::string&  chatKeyText,
    SessionState&       state,
    const InboundEvent& event
) {
    bool upgraded = false;
    // 1) 还在缓冲里: 合并 (补空 + @ 取更可靠的来源)
    for (auto it = state.items.rbegin(); it != state.items.rend(); ++it) {
        if (it->platformMessageId != event.platformMessageId) {
            continue;
        }
        const size_t beforeChars = utf8CodePointCount(it->text);
        upgraded                 = mergeSameMessage(*it, event);
        const size_t afterChars  = utf8CodePointCount(it->text);
        if (afterChars > beforeChars) {
            state.chars += afterChars - beforeChars;
        }
        break;
    }
    // 2) 历史里那条也补一下 (`chatbot_history` 看到的是它)
    for (auto it = state.history.rbegin(); it != state.history.rend(); ++it) {
        if (it->platformMessageId != event.platformMessageId) {
            continue;
        }
        it->mentionedMe = it->mentionedMe || event.mentionedMe();
        if (it->text.empty()) {
            it->text = event.text;
        }
        break;
    }
    XX_LOGD(
        "[{}] 重复消息已合并 (会话 '{}', msgId={}, @判定={}, 立刻唤醒={})",
        kLogTag,
        chatKeyText,
        event.platformMessageId,
        mentionSelfSourceName(event.mentionSelf),
        upgraded
    );
    return upgraded;
}

void ChatbotEngine::onInbound(const InboundEvent& rawEvent) {
    if (stopped_) {
        return;
    }
    InboundEvent event = rawEvent;
    if (event.accountId.empty() || event.chatId.empty()) {
        return;
    }
    if (event.timestampMs <= 0) {
        // 平台没给时间 (调试注入也常不填): 用本机时钟顶上, 否则这条消息既不能参与
        // 排序, 也拿不到显示用的钟点
        event.timestampMs = systemNowMs();
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
    if (event.fromFullMode) {
        // 群「全量消息」事件: 平台没开「接收所有消息」时这种事件恒为 0
        ++state.counters.fullMode;
        ++globalCounters_.fullMode;
        accountFullModeSeen_[event.accountId] = true;
    }

    // ---- 去重 (同一 msg_id 可能重复推送) ----
    if (!event.platformMessageId.empty()) {
        auto [it, inserted] = dedupe_.try_emplace(
            event.accountId,
            DedupeRing(static_cast<size_t>(std::max(16, config_.behavior.dedupeCapacity)))
        );
        if (it->second.seenOrAdd(event.platformMessageId)) {
            ++state.counters.duplicated;
            ++globalCounters_.duplicated;
            // 全量模式下「@ 了我」的消息会按 `GROUP_MESSAGE_CREATE` 与
            // `GROUP_AT_MESSAGE_CREATE` 各推一次: 两份携带的信息不一样 (一份有正文与附件,
            // 另一份一定带 @ 语义)。直接丢掉后到的那份会让「该唤醒的没唤醒」,
            // 所以把新信息并进缓冲里那条, 若因此变成「@ 了我」就立刻唤醒。
            if (mergeDuplicateInto(chatKeyText, state, event)) {
                flush(chatKeyText, "mentioned");
            }
            return;
        }
    }

    // ---- 路由 ----
    auto route = router_ != nullptr ? router_->route(key) : RouteResult{};
    if (!route.found || route.mode == ChatMode::Off) {
        ++state.counters.ignored;
        ++globalCounters_.ignored;
        // 第一次碰到这个会话就打成 info 并给出可抄的配置行: 拿到平台 id 本身不容易
        // (群号 / QQ 号都不对, 要用 openid), 这条日志里的 chatId 就是要填的值。
        // 之后降为 debug, 免得没人管的群刷屏。
        if (state.counters.ignored == 1) {
            XX_LOGI(
                "[{}] 会话 '{}' 没有匹配的 bind, 消息已丢弃; 要接它就在 personas[].bind.chats "
                "里加一项: type={}, id=\"{}\" (mode 自选 mention/always)",
                kLogTag,
                chatKeyText,
                chatTypeName(key.type),
                key.chatId
            );
        } else {
            XX_LOGD(
                "[{}] 消息不属于任何启用的人设, 已丢弃 (会话 '{}')",
                kLogTag,
                chatKeyText
            );
        }
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
    state.chars        += chars;
    state.lastActiveMs  = nowMs();
    if (state.items.size() > 1) {
        ++state.counters.aggregated;
        ++globalCounters_.aggregated;
    }
    // 排队条数变了: 侧边栏面板要重画
    notifyChanged();

    // 私聊里 `mention` 等同 `always` (单聊消息本来就是直接发给机器人的)
    ChatMode mode = route.mode;
    if (key.type == ChatType::Private && mode == ChatMode::Mention) {
        mode = ChatMode::Always;
    }
    const bool mentioned = state.items.back().mentionedMe();

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
    const std::string&               chatKeyText,
    const std::vector<InboundEvent>& batch,
    std::string_view                 reason,
    std::string_view                 delivery,
    bool*                            hasAttachments
) {
    if (hasAttachments != nullptr) {
        *hasAttachments = false;
    }
    const ChatKey key        = parseChatKey(chatKeyText);
    auto&         state      = stateOf(chatKeyText);
    const auto    route      = router_ != nullptr ? router_->route(key) : RouteResult{};
    // 中途插入 (`next-step` 且本轮就在跑): 编号接着上一段排, 本轮记账不复位 ——
    // 模型看到的是同一轮里又插进来一段, `replyTo` 仍按本轮统一编号取
    const bool midTurn = delivery == "next-step" && state.turn.active;

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
    // 当前时间 + 时区一起告诉模型: 消息前的 `[MM-DD HH:MM]` 与它是什么关系,
    // 否则模型会拿自己假设的时区去对时间 (总觉得与所在地区对不上)
    {
        const std::string nowClock = currentTimeText("%Y-%m-%d %H:%M");
        if (!nowClock.empty()) {
            const bool followPlatform
                = config_.behavior.timezoneSpec.kind == TimezoneSpec::Kind::Auto;
            blockOpt.currentTimeNote = "当前时间 " + nowClock + " (" + timezoneText()
                                       + "); 每条消息前的 [MM-DD HH:MM] 是平台给的发送时间"
                                       + (followPlatform ? ", 时区取自平台时间戳"
                                                        : ", 与它同一时区");
        }
    }
    if (!state.pendingNotice.empty()) {
        blockOpt.hintExtra = state.pendingNotice;
        state.pendingNotice.clear();
    }
    if (midTurn) {
        // 让模型知道这段是插进来的: 不要因为"上一句已经答完了"就忽略它
        if (!blockOpt.hintExtra.empty()) {
            blockOpt.hintExtra += "\n";
        }
        blockOpt.hintExtra += "这一轮还在执行中, 下面这段是中途插进来的新消息, 现在就可以看到; "
                              "回复时用到的编号与本轮前面那段是同一套";
    }

    std::vector<InputMessageView> views;
    std::vector<TurnMessage>      turnMessages;
    Json                          attachments = Json::array();
    size_t                        attachmentBudget
        = static_cast<size_t>(std::max(0, config_.behavior.attachmentsMaxPerMessage));
    std::string lastPassiveMsgId;
    std::string lastMentionMsgId;
    bool        hadMention = false;
    /// 本轮里 @ 了机器人的消息编号 (`[#n]` 的 n), 用来写「唤醒原因」
    std::vector<int> mentionIndexes;

    int index = 0;
    if (midTurn) {
        // 接着上一段的编号往下排: 模型手上的 `[#n]` 与 `replyTo` 用同一套
        for (const auto& existing : state.turn.messages) {
            index = std::max(index, existing.index);
        }
    }
    for (const auto& item : ordered) {
        InputMessageView view;
        view.index         = ++index;
        view.senderName    = displayNameOf(item.senderName, item.senderId, 64);
        view.senderRole    = item.senderRole;
        view.mentionedMe   = item.mentionedMe();
        view.mentionedOthers = item.mentionOthers;
        view.mentionedBots   = item.mentionBots;
        view.hasQuote      = !item.quotedText.empty();
        view.quotedSender  = item.quotedSender;
        view.text          = item.text;
        if (!item.quotedText.empty()) {
            const std::string quoted = utf8TruncateWithNotice(item.quotedText, 200);
            view.text = "(回复 " + displayNameOf(item.quotedSender, std::string{}, 64) + ": "
                        + quoted + ") " + view.text;
        }
        const std::string clock = messageClockText(item.timestampMs, item.timestampOffsetMinutes);
        if (!clock.empty()) {
            view.text = "[" + clock + "] " + view.text;
        }

        TurnMessage turnMessage;
        turnMessage.index             = view.index;
        turnMessage.platformMessageId = item.platformMessageId;
        turnMessage.replyRef          = item.replyRef;
        turnMessage.senderMentionId   = item.senderMentionId;
        turnMessage.senderName        = view.senderName;
        turnMessage.mentionedMe       = item.mentionedMe();
        turnMessage.timestampMs       = item.timestampMs;
        turnMessages.push_back(turnMessage);

        if (!item.platformMessageId.empty()) {
            lastPassiveMsgId = item.platformMessageId;
            if (item.mentionedMe()) {
                lastMentionMsgId = item.platformMessageId;
            }
        }
        if (item.mentionedMe()) {
            hadMention = true;
            mentionIndexes.push_back(view.index);
        }

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
                // 普通文件: 能内联就把内容给模型, 否则只给路径
                //
                // 只给路径时模型要靠文件系统工具才能读到, 人设根没挂那个插件就
                // 彻底读不到 (实测: 群里引用一个 README 问内容, 模型答"看不到文件")。
                // 小文本文件直接内联, 不依赖任何工具。
                std::string inlined;
                const bool  canInline = shouldInlineTextFile(
                    attachment,
                    config_.behavior.attachmentsInlineTextMaxBytes
                );
                if (canInline && inlineTextFile(attachment, inlined)) {
                    line.note = "内容已内联在下面";
                    view.attachments.push_back(std::move(line));
                    view.attachments.back().inlineText = std::move(inlined);
                } else {
                    line.note = canInline
                                    ? std::string{"普通文件, 可用 agentxx_filesystem_read 读取"}
                                    : std::string{"文件较大或不是文本, 可用 "
                                                  "agentxx_filesystem_read 读取"};
                    view.attachments.push_back(std::move(line));
                }
            }
        }
        views.push_back(std::move(view));
    }

    Json input = Json::object();
    // 「本轮为什么开始」写进输入块: 模型据此知道该不该接话
    // (被 @ / always 窗口到期 / 缓冲攒满), 而不是一概把最后一句话当问题回答
    blockOpt.wakeReason = wakeReasonNote(reason, ordered.size(), mentionIndexes);
    input["text"]       = renderInputBlock(blockOpt, views);
    input["delivery"]   = std::string{delivery};
    if (!attachments.empty()) {
        input["attachments"] = std::move(attachments);
        if (hasAttachments != nullptr) {
            *hasAttachments = true;
        }
    }

    // 一轮记账: 工具据此定位被动回复 id 与「回哪条」
    auto& turn       = state.turn;
    turn.active      = true;
    turn.wakeReason  = std::string{reason};
    turn.chatKey     = chatKeyText;
    turn.personaName = route.personaName;
    // 兜底策略 `mention` 也要看本轮模式: 私聊的 `mention` 已按 `always` 算
    turn.mode = (key.type == ChatType::Private && route.mode == ChatMode::Mention)
                    ? ChatMode::Always
                    : route.mode;
    if (midTurn) {
        // 同一轮里又插进来一段: 额度、是否调过工具、开始时间都不动, 编号接着排
        turn.turnSerial = ++activitySeq_;
        turn.messages.insert(turn.messages.end(), turnMessages.begin(), turnMessages.end());
        turn.hadMention = turn.hadMention || hadMention;
        const std::string newest
            = lastMentionMsgId.empty() ? lastPassiveMsgId : lastMentionMsgId;
        if (!newest.empty()) {
            turn.lastPassiveMsgId = newest;
        }
        ++state.counters.injected;
        ++globalCounters_.injected;
    } else {
        turn.turnSerial   = ++activitySeq_;
        turn.sentCount    = 0;
        turn.modelSent    = false;
        turn.messages     = std::move(turnMessages);
        turn.hadMention   = hadMention;
        turn.lastAssistantText.clear();
        turn.startedAtMs      = nowMs();
        turn.lastPassiveMsgId = lastMentionMsgId.empty() ? lastPassiveMsgId : lastMentionMsgId;
    }

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
        notifyChanged();
        return;
    }

    // 通道正在跑一轮: **不**攒到 TURN_END, 按 `next-step` 插进本轮 ——
    // 主程序在下一个 modelcall 边界把它写进上下文 (drainPendingSessionInputs),
    // 模型不用等这一轮跑完就能知道群里又说了什么 (见 README §2.1)
    const int64_t existingChannel = router_ != nullptr ? router_->find(chatKeyText) : 0;
    const bool    running = router_ != nullptr && existingChannel > 0
                            && router_->isBusy(existingChannel);
    if (running) {
        XX_LOGD(
            "[{}] 会话 '{}' 正在跑一轮, {} 条消息按 next-step 插进本轮 (原因: {})",
            kLogTag,
            chatKeyText,
            state.items.size(),
            std::string{reason}
        );
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
        notifyChanged();
        return;
    }

    std::vector<InboundEvent> batch(state.items.begin(), state.items.end());
    state.items.clear();
    state.chars = 0;
    state.stale = false;
    ++state.counters.woken;
    ++globalCounters_.woken;
    if (reason == "mentioned") {
        ++state.counters.wakeMentioned;
        ++globalCounters_.wakeMentioned;
    }
    // 排队被取走: 面板上的条数要归零
    notifyChanged();

    bool              hasAttachments = false;
    const std::string inputJson      = assembleInput(
        chatKeyText,
        batch,
        reason,
        // 正在跑一轮 → next-step (插进本轮); 否则排队成新一轮
        running ? "next-step" : "next-turn",
        &hasAttachments
    );
    int32_t rc = runtime_.sendInput ? runtime_.sendInput(channelId, inputJson) : -1;
    if (rc != 0 && hasAttachments) {
        // 附件被拒 (超出主程序的单文件大小/条数上限): 去掉附件重投一次
        // - 主程序对这类输入返回参数错误, 整条输入 (含正文) 都不会进上下文;
        //   而渲染好的输入块里已写清文件名/大小/路径, 只丢多模态附件不影响模型
        //   知道“收到了什么”。不重试的话现象是“群里发了视频后机器人再也不出声”
        const std::string retryJson = withoutAttachments(inputJson);
        const int32_t     retryRc
            = runtime_.sendInput ? runtime_.sendInput(channelId, retryJson) : -1;
        XX_LOGW(
            "[{}] 会话 '{}' 带附件的输入被拒 (错误码 {}), {} (错误码 {})",
            kLogTag,
            chatKeyText,
            rc,
            retryRc == 0 ? "已去掉附件重投" : "去掉附件重投仍失败",
            retryRc
        );
        rc = retryRc;
    }
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
        if (running) {
            // 插不进本轮: 放回缓冲队首, 等本轮 TURN_END 再投 —— 内容不能丢
            state.items.insert(state.items.begin(), batch.begin(), batch.end());
            for (const auto& item : batch) {
                state.chars += utf8CodePointCount(item.text);
            }
            state.stale = true;
            notifyChanged();
        }
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
    notifyChanged();
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
    // `now` 是真正的当前时间 (以前给的也是启动时间, “当前”与“启动于”就分不开了)
    vars["now"]        = currentTimeText("%Y-%m-%d %H:%M:%S");
    vars["uptime"]     = startupTimeText_;
    vars["timezone"]   = timezoneText();
    vars["received"]   = std::to_string(counters.received);
    vars["sent"]       = std::to_string(counters.sent);
    vars["dropped"]    = std::to_string(counters.dropped);
    vars["failed"]     = std::to_string(counters.failed);
    vars["duplicated"] = std::to_string(counters.duplicated);
    if (router_ != nullptr) {
        const int64_t channelId = router_->find(chatKeyText);
        // 没有通道时不要显示一个孤立的 `-`: 用户看不出这是"会话还没建"还是"取不到"。
        // 本地命令不建通道, 所以首条命令必然是未建状态。
        vars["channelId"] = channelId > 0 ? std::to_string(channelId) : std::string{"未建"};
        if (!route.personaName.empty()) {
            const auto* persona = config_.findPersona(route.personaName);
            vars["model"]       = persona != nullptr ? persona->model : std::string{};
        }
    }
    std::string commandList;
    for (const auto& command : config_.commands) {
        // 根命令本身等同 help, 列表里只列带子命令的写法 (不然会重复一项)
        if (command.name == kCommandRoot) {
            continue;
        }
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
        notifyChanged();
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
            notifyChanged();
            return true;
        }
        if (def->action == "task") {
            // 后台任务: `submit <要做的事>` / `list` / `status [id]` / `cancel [id|all]`
            std::string reply;
            if (runtime_.taskCommand) {
                reply = runtime_.taskCommand(chatKeyText, command.args);
            } else {
                reply = "后台任务不可用 (当前主程序没有 async work 入口)";
            }
            OutboundMessage message;
            message.accountId      = key.accountId;
            message.type           = key.type;
            message.chatId         = key.chatId;
            message.text           = std::move(reply);
            message.passiveReplyTo = event.platformMessageId;
            message.replyToRef     = event.replyRef;
            if (outbound_ != nullptr && !message.text.empty()) {
                outbound_->enqueueDirect(key, std::move(message));
            }
            notifyChanged();
            return true;
        }
        // clear (本期未实现: 只提示)
        XX_LOGW("[{}] 命令动作 '{}' 本期未实现", kLogTag, def->action);
        notifyChanged();
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
    turn.wakeReason       = "command";
    turn.mode             = (key.type == ChatType::Private && route.mode == ChatMode::Mention)
                                ? ChatMode::Always
                                : route.mode;
    turn.startedAtMs      = nowMs();
    turn.lastPassiveMsgId = event.platformMessageId;
    turn.messages.clear();
    TurnMessage commandMessage;
    commandMessage.index             = 1;
    commandMessage.platformMessageId = event.platformMessageId;
    commandMessage.replyRef          = event.replyRef;
    commandMessage.senderMentionId   = event.senderMentionId;
    commandMessage.senderName        = event.senderName;
    commandMessage.mentionedMe       = true;
    commandMessage.timestampMs       = event.timestampMs;
    turn.messages.push_back(std::move(commandMessage));
    notifyChanged();
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
        notifyChanged();
        return;
    }
    // 通道就绪时如果有积压, 立刻投 (例如首条消息在 root init 期间到达)
    auto& state = stateOf(chatKey);
    if (!state.items.empty()) {
        flush(chatKey, "channel_ready");
    }
    notifyChanged();
}

void ChatbotEngine::onChannelTurnStart(int64_t channelId) {
    if (router_ == nullptr) {
        return;
    }
    router_->markBusy(channelId, true);
    const auto chatKey = router_->chatKeyOfChannel(channelId);
    if (chatKey.empty()) {
        notifyChanged();
        return;
    }
    auto& state      = stateOf(chatKey);
    state.turn.active = true;
    state.lastActiveMs = nowMs();
    if (state.turn.startedAtMs == 0) {
        state.turn.startedAtMs = nowMs();
    }
    ++state.counters.turns;
    ++globalCounters_.turns;
    // 面板左侧的加载动画靠这次变化启动
    notifyChanged();
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
    // 一轮结束: 面板左侧的加载动画停下 (flush 内部也会通知一次, 由入口合并)
    notifyChanged();
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
    notifyChanged();
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
                                       ? R"_({"values":{"decision":"true","remember":false,"fullAuth":false}})_"
                                       : R"_({"values":{"decision":"false","remember":false,"fullAuth":false}})_";
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
        runtime_.respondInterrupt(channelId, interruptId, R"_({"values":{}})_");
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
    // `mention`: 本轮被 @ 时兜底。`always` 会话本来就不需要 @, 所以模式也要算进来
    // —— 只看 `hadMention` 会把 always 会话里“没被 @ 的那一轮”整段丢掉:
    // 模型把回答写在正文里而没调工具时, 群里一个字都发不出去 (表现为“@ 了才回”)。
    if (policy == "mention" && !turn.hadMention && turn.mode != ChatMode::Always) {
        return;
    }
    const std::string text = trimCopy(turn.lastAssistantText);
    if (text.empty()) {
        return;
    }
    // 模型这轮明确表示不说话 (正文只有「保持安静」这类标记): 兜底也不能把它发出去
    if (isSilenceReply(text)) {
        XX_LOGI("[{}] 本轮正文是静默标记, 不兜底发送 (会话 '{}'): {}", kLogTag, chatKey, text);
        return;
    }
    const ChatKey key = parseChatKey(chatKey);
    OutboundRequest request;
    request.text         = text;
    request.passiveMsgId = defaultPassiveMsgId(chatKey);
    // 被动回复窗口过期时改发主动消息要 @ 原说话人
    if (!request.passiveMsgId.empty()) {
        mentionTargetOf(
            chatKey,
            request.passiveMsgId,
            request.mentionOpenId,
            request.mentionName
        );
    }
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
        notifyChanged();
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
    notifyChanged();
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
    notifyChanged();
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

bool ChatbotEngine::mentionTargetOf(
    std::string_view chatKey,
    std::string_view platformMessageId,
    std::string&     openId,
    std::string&     name
) const {
    if (platformMessageId.empty()) {
        return false;
    }
    auto it = sessions_.find(std::string{chatKey});
    if (it == sessions_.end()) {
        return false;
    }
    // 从新往旧找: 同一个 msgId 正常只有一条, 重复到达的也已合并
    const auto& messages = it->second.turn.messages;
    for (auto r = messages.rbegin(); r != messages.rend(); ++r) {
        if (r->platformMessageId != platformMessageId) {
            continue;
        }
        openId = r->senderMentionId;
        name   = r->senderName;
        return true;
    }
    return false;
}

void ChatbotEngine::noteAssistantText(std::string_view chatKey, std::string_view text) {
    auto it = sessions_.find(std::string{chatKey});
    if (it == sessions_.end()) {
        return;
    }
    it->second.turn.lastAssistantText.assign(text);
}

bool ChatbotEngine::deliverTaskResult(std::string_view chatKey, std::string_view text) {
    if (stopped_ || text.empty() || router_ == nullptr || !runtime_.sendInput) {
        return false;
    }
    const int64_t channelId = router_->find(chatKey);
    if (channelId <= 0) {
        // 会话通道还没建 / 已被回收: 由调用方改用 outbound 直接发
        return false;
    }
    Json input        = Json::object();
    input["text"]     = std::string{text};
    input["delivery"] = "next-turn";
    const int32_t rc  = runtime_.sendInput(channelId, input.dump());
    if (rc != 0) {
        XX_LOGW(
            "[{}] 后台任务结果投递被拒 (会话 '{}', 通道 {}, 错误码 {})",
            kLogTag,
            chatKey,
            channelId,
            rc
        );
        return false;
    }
    XX_LOGI("[{}] 后台任务结果已投回会话 '{}' (通道 {})", kLogTag, chatKey, channelId);
    notifyChanged();
    return true;
}

/* ==================== 侧边栏面板快照 ==================== */

bool ChatbotEngine::hasRunningSession() const {
    for (const auto& [chatKeyText, state] : sessions_) {
        if (state.turn.active) {
            return true;
        }
        if (router_ == nullptr) {
            continue;
        }
        const auto* entry = router_->entry(chatKeyText);
        if (entry != nullptr && entry->busy) {
            return true;
        }
    }
    return false;
}

PanelSnapshot ChatbotEngine::panelSnapshot() const {
    PanelSnapshot snapshot;
    snapshot.startupTime = startupTimeText_;
    snapshot.received    = globalCounters_.received;
    snapshot.sent        = globalCounters_.sent;
    snapshot.failed      = globalCounters_.failed;

    // ---- 账号连接状态 ----
    for (const auto& account : config_.accounts) {
        PanelAccountView view;
        view.id   = account.id;
        view.type = account.type;
        auto it   = accountStatus_.find(account.id);
        if (it == accountStatus_.end()) {
            view.state = "unknown";
        } else {
            try {
                const auto parsed = Json::parse(it->second);
                view.state        = parsed.value("state", std::string{"unknown"});
                view.reason       = parsed.value("reason", std::string{});
            } catch (const std::exception&) {
                view.state = "unknown";
            }
        }
        snapshot.accounts.push_back(std::move(view));
    }

    // ---- 人设 ----
    for (const auto& persona : config_.personas) {
        PanelPersonaView view;
        view.name   = persona.name;
        view.model  = persona.model;
        view.rootId = router_ != nullptr ? router_->rootIdOf(persona.name) : std::string{};
        view.mounted = !view.rootId.empty();
        view.chats      = router_ != nullptr ? router_->activeCount(persona.name) : 0;
        view.chatBudget = static_cast<size_t>(std::max(0, config_.behavior.maxChatsPerPersona));
        view.binds      = persona.binds.size();
        view.tools      = persona.tools.size();
        view.promptHead = firstLineOf(persona.prompt.systemPrompt);
        snapshot.personas.push_back(std::move(view));
    }

    // ---- 会话 ----
    std::vector<PanelSessionView> sessions;
    sessions.reserve(sessions_.size());
    for (const auto& [chatKeyText, state] : sessions_) {
        const ChannelEntry* entry = router_ != nullptr ? router_->entry(chatKeyText) : nullptr;
        const int64_t       channelId = entry != nullptr ? entry->channelId : 0;
        // 通道没建起来、也没收过消息的条目不算「已创建的会话」
        if (channelId <= 0 && state.counters.received == 0) {
            continue;
        }
        const ChatKey key   = parseChatKey(chatKeyText);
        const auto    route = router_ != nullptr ? router_->route(key) : RouteResult{};

        PanelSessionView view;
        view.chatKey = chatKeyText;
        view.title   = ChatbotRouter::titleOf(
            router_ != nullptr ? router_->chatNameOf(chatKeyText) : std::string{},
            key
        );
        view.persona   = route.personaName;
        view.account   = key.accountId;
        view.chatType  = chatTypeName(key.type);
        view.mode      = chatModeName(route.mode);
        view.sessionId = entry != nullptr ? entry->sessionId : std::string{};
        view.channelId = channelId;
        view.ready     = entry != nullptr && entry->ready;
        view.running   = state.turn.active || (entry != nullptr && entry->busy);
        view.queued    = state.items.size();
        view.turns     = state.counters.turns;
        view.sent      = state.counters.sent;
        view.lastTurnMs   = state.counters.lastTurnMs;
        view.lastActiveMs = state.lastActiveMs;
        sessions.push_back(std::move(view));
    }
    // 正在执行的排最前 (用户最关心), 其余按最近活动时间倒序
    std::sort(
        sessions.begin(),
        sessions.end(),
        [](const PanelSessionView& a, const PanelSessionView& b) {
            if (a.running != b.running) {
                return a.running;
            }
            if (a.lastActiveMs != b.lastActiveMs) {
                return a.lastActiveMs > b.lastActiveMs;
            }
            return a.chatKey < b.chatKey;
        }
    );
    snapshot.sessions = std::move(sessions);
    return snapshot;
}

/* ==================== 状态输出 ==================== */

std::string ChatbotEngine::statusJson() const {
    Json out = Json::object();
    out["startupTime"] = startupTimeText_;
    // 消息时间按哪个时区显示 (排查“时间对不上”先看这两项)
    out["timezone"] = timezoneText();
    out["configTimezone"] = config_.behavior.timezone;
    out["now"] = currentTimeText("%Y-%m-%d %H:%M:%S");

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
    // 被动回复窗口过期 → 改发 @ 的主动消息的条数
    outboundJson["passiveFallback"] = static_cast<int64_t>(counters.passiveFallback);
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
    countersJson["wakeMentioned"] = static_cast<int64_t>(globalCounters_.wakeMentioned);
    countersJson["aggregated"] = static_cast<int64_t>(globalCounters_.aggregated);
    countersJson["turns"]      = static_cast<int64_t>(globalCounters_.turns);
    // 会话正在执行一轮时按 next-step 插进本轮的条数
    countersJson["injected"]   = static_cast<int64_t>(globalCounters_.injected);
    // 群里“非 @ 消息”的条数: 一直为 0 就说明平台没给「接收所有消息」权限
    countersJson["fullMode"]   = static_cast<int64_t>(globalCounters_.fullMode);
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
    out["timezone"]   = timezoneText();
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
    countersJson["wakeMentioned"] = static_cast<int64_t>(counters.wakeMentioned);
    countersJson["turns"]      = static_cast<int64_t>(counters.turns);
    countersJson["injected"]   = static_cast<int64_t>(counters.injected);
    countersJson["lastTurnMs"] = counters.lastTurnMs;
    // 本会话里来自群「全量消息」事件的条数 (`always` 会话靠它判断平台权限)
    countersJson["fullMode"]   = static_cast<int64_t>(counters.fullMode);
    out["counters"]            = std::move(countersJson);
    auto it = sessions_.find(std::string{chatKey});
    out["buffered"] = it == sessions_.end()
                          ? 0
                          : static_cast<int64_t>(it->second.items.size());
    // 上一轮为什么开始 (`mentioned` / `window_expired` / `buffer_full` ...)
    out["lastWakeReason"] = it == sessions_.end() ? std::string{} : it->second.turn.wakeReason;
    return out.dump();
}

} // namespace chatbot
} // namespace agentxx
