#include "chatbot_types.h"

#include <algorithm>

namespace agentxx {
namespace chatbot {

namespace {

std::string lowerCopy(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        out.push_back(uc >= 'A' && uc <= 'Z' ? static_cast<char>(uc + ('a' - 'A')) : c);
    }
    return out;
}

} // namespace

const char* chatTypeName(ChatType type) noexcept {
    switch (type) {
        case ChatType::Group:
            return "group";
        case ChatType::Private:
            return "private";
    }
    return "group";
}

const char* chatModeName(ChatMode mode) noexcept {
    switch (mode) {
        case ChatMode::Mention:
            return "mention";
        case ChatMode::Always:
            return "always";
        case ChatMode::Off:
            return "off";
    }
    return "mention";
}

const char* mediaKindName(MediaKind kind) noexcept {
    switch (kind) {
        case MediaKind::Image:
            return "image";
        case MediaKind::Audio:
            return "audio";
        case MediaKind::Video:
            return "video";
        case MediaKind::File:
            return "file";
    }
    return "file";
}

const char* platformStateName(PlatformState state) noexcept {
    switch (state) {
        case PlatformState::Connecting:
            return "connecting";
        case PlatformState::Online:
            return "online";
        case PlatformState::Reconnecting:
            return "reconnecting";
        case PlatformState::Disabled:
            return "disabled";
    }
    return "connecting";
}

bool chatTypeFromString(std::string_view text, ChatType& out) noexcept {
    const auto lower = lowerCopy(text);
    if (lower == "group" || lower == "群" || lower == "群聊") {
        out = ChatType::Group;
        return true;
    }
    if (lower == "private" || lower == "single" || lower == "c2c" || lower == "私聊") {
        out = ChatType::Private;
        return true;
    }
    return false;
}

bool chatModeFromString(std::string_view text, ChatMode& out) noexcept {
    const auto lower = lowerCopy(text);
    if (lower == "mention" || lower == "at") {
        out = ChatMode::Mention;
        return true;
    }
    if (lower == "always" || lower == "all") {
        out = ChatMode::Always;
        return true;
    }
    if (lower == "off" || lower == "none" || lower == "disabled") {
        out = ChatMode::Off;
        return true;
    }
    return false;
}

std::string ChatKey::key() const {
    return accountId + "|" + chatTypeName(type) + "|" + chatId;
}

ChatKey parseChatKey(std::string_view text) {
    ChatKey out;
    const auto first = text.find('|');
    if (first == std::string_view::npos) {
        return out;
    }
    const auto second = text.find('|', first + 1);
    if (second == std::string_view::npos) {
        return out;
    }
    out.accountId = std::string{text.substr(0, first)};
    if (!chatTypeFromString(text.substr(first + 1, second - first - 1), out.type)) {
        return ChatKey{};
    }
    out.chatId = std::string{text.substr(second + 1)};
    if (!out.valid()) {
        return ChatKey{};
    }
    return out;
}

} // namespace chatbot
} // namespace agentxx
