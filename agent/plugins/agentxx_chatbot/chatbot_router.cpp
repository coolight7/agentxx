#include "chatbot_router.h"

#include "chatbot_core.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <cstdio>

namespace agentxx {
namespace chatbot {

namespace {

constexpr const char* kLogTag = "chatbot";

/// 稳定的 64 位散列 (FNV-1a) → 16 位十六进制文本
///
/// 会话 id 只要求「同一 chatId 永远算出同一个值、不同 chatId 基本不撞」, 因此用
/// 不依赖第三方库的散列即可 (设计文档里写的是 md5 前 16 位, 语义等价)。
std::string stableHash16(std::string_view text) {
    uint64_t hash = 1469598103934665603ULL;
    for (char c : text) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string{buf};
}

} // namespace

std::string ChatbotRouter::titleOf(std::string_view chatName, const ChatKey& key) {
    if (!chatName.empty()) {
        return std::string{chatName};
    }
    const std::string tail = chatIdTail(key.chatId, 8);
    return std::string{chatTypeName(key.type)} + "-" + tail;
}

void ChatbotRouter::build(const ChatbotConfig& config, int32_t maxChatsPerPersona, Hooks hooks) {
    config_             = config;
    maxChatsPerPersona_ = std::max(1, maxChatsPerPersona);
    hooks_              = std::move(hooks);
    table_.build(config);
}

RouteResult ChatbotRouter::route(const ChatKey& key) const {
    auto result = table_.route(key);
    if (result.found) {
        result.mode = table_.effectiveMode(key, result);
    }
    return result;
}

std::string ChatbotRouter::sessionIdOf(std::string_view personaName, const ChatKey& key) const {
    // 可读 + 定长 + 跨重启稳定: 同一个群永远同一个会话
    std::string out = "chatbot_";
    out += personaName.empty() ? std::string{"default"} : std::string{personaName};
    out += "_";
    out += key.accountId;
    out += "_";
    out += chatTypeName(key.type);
    out += "_";
    out += stableHash16(key.chatId);
    return out;
}

std::vector<std::string> ChatbotRouter::personaNames() const {
    std::vector<std::string> out;
    out.reserve(config_.personas.size());
    for (const auto& persona : config_.personas) {
        out.push_back(persona.name);
    }
    return out;
}

void ChatbotRouter::setRootId(std::string_view personaName, std::string rootId) {
    if (rootId.empty()) {
        return;
    }
    rootIds_[std::string{personaName}] = std::move(rootId);
}

std::string ChatbotRouter::rootIdOf(std::string_view personaName) const {
    auto it = rootIds_.find(personaName);
    return it == rootIds_.end() ? std::string{} : it->second;
}

void ChatbotRouter::rememberChatName(std::string_view chatKey, std::string_view chatName) {
    if (chatName.empty()) {
        return;
    }
    chatNames_[std::string{chatKey}] = std::string{chatName};
}

std::string ChatbotRouter::chatNameOf(std::string_view chatKey) const {
    auto it = chatNames_.find(chatKey);
    return it == chatNames_.end() ? std::string{} : it->second;
}

int64_t ChatbotRouter::find(std::string_view chatKey) const {
    auto it = entries_.find(chatKey);
    return it == entries_.end() ? 0 : it->second.channelId;
}

std::vector<std::string> ChatbotRouter::chatKeysOf(std::string_view personaName) const {
    return table_.chatKeysOf(personaName);
}

std::string ChatbotRouter::chatKeyOfChannel(int64_t channelId) const {
    auto it = channelToKey_.find(channelId);
    return it == channelToKey_.end() ? std::string{} : it->second;
}

std::string ChatbotRouter::chatKeyOfSession(std::string_view sessionId) const {
    if (sessionId.empty()) {
        return {};
    }
    for (const auto& [key, entry] : entries_) {
        if (entry.sessionId == sessionId) {
            return key;
        }
    }
    return {};
}

ChannelEntry* ChatbotRouter::entry(std::string_view chatKey) {
    auto it = entries_.find(chatKey);
    return it == entries_.end() ? nullptr : &it->second;
}

const ChannelEntry* ChatbotRouter::entry(std::string_view chatKey) const {
    auto it = entries_.find(chatKey);
    return it == entries_.end() ? nullptr : &it->second;
}

void ChatbotRouter::markReady(int64_t channelId, bool ready) {
    auto key = chatKeyOfChannel(channelId);
    if (key.empty()) {
        return;
    }
    if (auto* item = entry(key); item != nullptr) {
        item->ready = ready;
    }
}

void ChatbotRouter::setModeOverride(std::string_view chatKey, ChatMode mode) {
    table_.setModeOverride(chatKey, mode);
    if (auto* item = entry(chatKey); item != nullptr) {
        item->mode = mode;
    }
}

void ChatbotRouter::markBusy(int64_t channelId, bool busy) {    auto key = chatKeyOfChannel(channelId);
    if (key.empty()) {
        return;
    }
    if (auto* item = entry(key); item != nullptr) {
        item->busy = busy;
    }
}

bool ChatbotRouter::isBusy(int64_t channelId) const {
    const auto key = chatKeyOfChannel(channelId);
    if (key.empty()) {
        return false;
    }
    const auto* item = entry(key);
    return item != nullptr && item->busy;
}

bool ChatbotRouter::isReady(int64_t channelId) const {
    const auto key = chatKeyOfChannel(channelId);
    if (key.empty()) {
        return false;
    }
    const auto* item = entry(key);
    return item != nullptr && item->ready;
}

void ChatbotRouter::touch(int64_t channelId, uint64_t seq) {
    const auto key = chatKeyOfChannel(channelId);
    if (key.empty()) {
        return;
    }
    if (auto* item = entry(key); item != nullptr) {
        item->lastActiveSeq = seq;
    }
}

void ChatbotRouter::release(int64_t channelId, std::string_view reason) {
    if (channelId <= 0) {
        return;
    }
    const auto key = chatKeyOfChannel(channelId);
    if (key.empty()) {
        return;
    }
    if (hooks_.close) {
        hooks_.close(channelId);
    }
    channelToKey_.erase(channelId);
    if (auto* item = entry(key); item != nullptr) {
        item->channelId = 0;
        item->ready     = false;
        item->busy      = false;
    }
    XX_LOGI("[{}] channel {} closed ({})", kLogTag, channelId, std::string{reason});
}

size_t ChatbotRouter::releaseAll(std::string_view reason) {
    std::vector<int64_t> channels;
    for (const auto& [channelId, key] : channelToKey_) {
        (void)key;
        channels.push_back(channelId);
    }
    for (auto channelId : channels) {
        release(channelId, reason);
    }
    return channels.size();
}

size_t ChatbotRouter::activeCount(std::string_view personaName) const {
    size_t count = 0;
    for (const auto& [key, item] : entries_) {
        if (item.channelId <= 0) {
            continue;
        }
        auto routeResult = route(parseChatKey(key));
        if (routeResult.found && routeResult.personaName == personaName) {
            ++count;
        }
    }
    return count;
}

bool ChatbotRouter::evictLru(std::string_view personaName, std::string& err) {
    int64_t    victimChannel = 0;
    uint64_t   victimSeq     = 0;
    std::string victimKey;
    bool       haveVictim    = false;
    for (const auto& [key, item] : entries_) {
        if (item.channelId <= 0) {
            continue;
        }
        const ChatKey chatKey = parseChatKey(key);
        auto          route   = this->route(chatKey);
        if (!route.found || route.personaName != personaName) {
            continue;
        }
        if (item.busy && entries_.size() > 1) {
            // 正在跑一轮的会话尽量不动: 关了会打断模型输出
            // (全部都在忙时只能挑最久未活动的那条)
            continue;
        }
        if (!haveVictim || item.lastActiveSeq < victimSeq) {
            haveVictim    = true;
            victimChannel = item.channelId;
            victimSeq     = item.lastActiveSeq;
            victimKey     = key;
        }
    }
    if (!haveVictim) {
        // 全都在忙: 退一步挑最久未活动的, 但不动还没到上限的那个
        for (const auto& [key, item] : entries_) {
            if (item.channelId <= 0) {
                continue;
            }
            if (!haveVictim || item.lastActiveSeq < victimSeq) {
                haveVictim    = true;
                victimChannel = item.channelId;
                victimSeq     = item.lastActiveSeq;
                victimKey     = key;
            }
        }
    }
    if (!haveVictim || victimChannel <= 0) {
        err = "没有可回收的会话通道";
        return false;
    }
    XX_LOGW(
        "[{}] 人设 '{}' 活跃会话达上限 {}, 回收最久未活动的会话 '{}' (通道 {})",
        kLogTag,
        std::string{personaName},
        maxChatsPerPersona_,
        victimKey,
        victimChannel
    );
    release(victimChannel, "lru_evicted");
    err.clear();
    return true;
}

int64_t ChatbotRouter::acquire(
    const ChatKey&     key,
    const RouteResult& routeResult,
    std::string_view   chatTitle,
    uint64_t           seq,
    std::string&       err
) {
    if (!routeResult.found) {
        err = "该会话不属于任何已配置的人设";
        return 0;
    }
    const std::string chatKeyText = key.key();
    if (auto it = entries_.find(chatKeyText); it != entries_.end() && it->second.channelId > 0) {
        it->second.lastActiveSeq = seq;
        it->second.mode          = routeResult.mode;
        return it->second.channelId;
    }
    // 额度: 该人设下的活跃通道数已达上限时先回收一条
    while (activeCount(routeResult.personaName)
           >= static_cast<size_t>(maxChatsPerPersona_)) {
        std::string evictErr;
        if (!evictLru(routeResult.personaName, evictErr)) {
            err = "该人设活跃会话已达上限 " + std::to_string(maxChatsPerPersona_) + " 且"
                  + (evictErr.empty() ? std::string{"无法回收"} : evictErr)
                  + " (可调大 behavior.maxChatsPerPersona)";
            return 0;
        }
    }
    if (!hooks_.open) {
        err = "插件未提供开通道入口";
        return 0;
    }
    const std::string sessionId = sessionIdOf(routeResult.personaName, key);
    const int64_t     channelId = hooks_.open(routeResult.personaName, sessionId, chatTitle);
    if (channelId <= 0) {
        err = "打开会话通道失败 (主程序日志里有原因: 会话被占用或通道预算用尽)";
        return 0;
    }
    ChannelEntry item;
    item.channelId     = channelId;
    item.lastActiveSeq = seq;
    item.mode          = routeResult.mode;
    item.sessionId     = sessionId;
    entries_[chatKeyText] = std::move(item);
    channelToKey_[channelId] = chatKeyText;
    XX_LOGI(
        "[{}] 会话 '{}' 的通道已创建: {} (session '{}', 人设 '{}')",
        kLogTag,
        chatKeyText,
        channelId,
        sessionId,
        routeResult.personaName
    );
    return channelId;
}

} // namespace chatbot
} // namespace agentxx
