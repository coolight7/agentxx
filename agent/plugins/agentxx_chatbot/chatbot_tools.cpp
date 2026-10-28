#include "chatbot_tools.h"

#include "chatbot_core.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <algorithm>

namespace agentxx {
namespace chatbot {

namespace {

constexpr const char* kLogTag = "chatbot";

using utilxx_base::Json;

Json parseArgsOrThrow(std::string_view argsJson) {
    if (argsJson.empty()) {
        return Json::object();
    }
    try {
        auto parsed = Json::parse(std::string{argsJson});
        if (!parsed.is_object()) {
            throw std::invalid_argument("参数必须是 JSON 对象");
        }
        return parsed;
    } catch (const std::invalid_argument&) {
        throw;
    } catch (const std::exception& e) {
        throw std::invalid_argument(std::string{"参数 JSON 解析失败: "} + e.what());
    }
}

std::string stringArg(const Json& args, std::string_view key, std::string_view fallback = {}) {
    if (!args.contains(key)) {
        return std::string{fallback};
    }
    const auto& value = args[std::string{key}];
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return std::to_string(value.get<long long>());
    }
    return std::string{fallback};
}

bool boolArg(const Json& args, std::string_view key, bool fallback) {
    if (!args.contains(key)) {
        return fallback;
    }
    const auto& value = args[std::string{key}];
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        return text == "true" || text == "1" || text == "yes";
    }
    return fallback;
}

int64_t intArg(const Json& args, std::string_view key, int64_t fallback) {
    if (!args.contains(key)) {
        return fallback;
    }
    const auto& value = args[std::string{key}];
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return value.get<long long>();
    }
    if (value.is_string()) {
        try {
            return std::stoll(value.get<std::string>());
        } catch (const std::exception&) {
            return fallback;
        }
    }
    return fallback;
}

/// 附件参数 → `MediaRef` 列表 (字符串 = 本地路径或 URL)
std::vector<MediaRef> attachmentsArg(const Json& args) {
    std::vector<MediaRef> out;
    if (!args.contains("attachments") || !args["attachments"].is_array()) {
        return out;
    }
    for (const auto& item : args["attachments"]) {
        if (!item.is_string()) {
            continue;
        }
        const auto text = item.get<std::string>();
        if (text.empty()) {
            continue;
        }
        MediaRef ref;
        const bool isRemote = text.rfind("http://", 0) == 0 || text.rfind("https://", 0) == 0;
        if (isRemote) {
            ref.url = text;
        } else {
            ref.localPath = text;
        }
        const auto slash = text.find_last_of("/\\");
        ref.fileName     = slash == std::string::npos ? text : text.substr(slash + 1);
        out.push_back(std::move(ref));
    }
    return out;
}

/// 该会话里可用的其它会话清单 (错误提示里给出前 10 个)
std::string chatListHint(const ChatbotToolDeps& deps, std::string_view personaName) {
    if (deps.router == nullptr) {
        return {};
    }
    const auto keys = deps.router->chatKeysOf(personaName);
    std::string out;
    size_t      count = 0;
    for (const auto& key : keys) {
        if (count >= 10) {
            out += " …";
            break;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += key;
        ++count;
    }
    return out;
}

} // namespace
std::string resolveChat(
    const ChatbotToolDeps& deps,
    std::string_view       threadId,
    std::string_view       explicitChat,
    std::string&           err
) {
    err.clear();
    if (deps.engine == nullptr || deps.router == nullptr) {
        err = "chatbot 插件状态不可用";
        return {};
    }
    std::string chatKey;
    if (!explicitChat.empty()) {
        const ChatKey parsed = parseChatKey(explicitChat);
        if (!parsed.valid()) {
            err = "chat 参数格式非法 (期望 `account|group|chatId` 或 `account|private|chatId`)";
            return {};
        }
        if (!deps.router->route(parsed).found) {
            err = "chat '" + std::string{explicitChat} + "' 不属于任何已配置的人设";
            return {};
        }
        chatKey = parsed.key();
    } else {
        chatKey = deps.engine->resolveChatKey(threadId);
        if (chatKey.empty()) {
            err = "定位不到当前会话: 这不是 chatbot 会话 (或该会话未启用); "
                  "从其它会话调用时必须用 chat 参数显式指定, 例如 "
                  "`qq_main|group|<群 openid>`";
            return {};
        }
    }
    // 跨人设保护: 工具挂在人设 root 上, 只能操作同一个人设下的会话
    const ChatKey key      = parseChatKey(chatKey);
    const auto    route    = deps.router->route(key);
    if (!route.found) {
        err = "会话 '" + chatKey + "' 不属于任何已配置的人设";
        return {};
    }
    if (!threadId.empty() && deps.router->rootIdOf(route.personaName).empty()) {
        // 人设 root 尚未挂好 (极少见): 明确提示而不是静默失败
        err = "人设 '" + route.personaName + "' 的根还未就绪";
        return {};
    }
    return chatKey;
}

std::string toolSendMessage(
    const ChatbotToolDeps& deps,
    std::string_view       threadId,
    std::string_view       argsJson
) {
    const auto args = parseArgsOrThrow(argsJson);
    const auto text = stringArg(args, "text");
    auto       attachments = attachmentsArg(args);
    if (trimCopy(text).empty() && attachments.empty()) {
        throw std::invalid_argument("text 与 attachments 至少要有一个非空");
    }
    std::string err;
    const std::string chatKey = resolveChat(deps, threadId, stringArg(args, "chat"), err);
    if (chatKey.empty()) {
        throw std::runtime_error(err);
    }
    if (deps.engine == nullptr || deps.outbound == nullptr) {
        throw std::runtime_error("chatbot 插件状态不可用");
    }
    const ChatKey key = parseChatKey(chatKey);

    // 引用哪条: `replyTo` 是本轮输入里的 `#n`
    OutboundRequest request;
    request.text         = text;
    request.attachments  = std::move(attachments);
    request.markdown     = boolArg(args, "markdown", false);
    request.passiveMsgId = deps.engine->defaultPassiveMsgId(chatKey);
    const int replyIndex = parseReplyTo(stringArg(args, "replyTo"));
    if (replyIndex > 0) {
        const auto* turn = deps.engine->turnOf(chatKey);
        if (turn == nullptr) {
            throw std::runtime_error("当前会话没有进行中的一轮, 无法按编号引用 (去掉 replyTo 再发)");
        }
        const TurnMessage* matched = nullptr;
        for (const auto& message : turn->messages) {
            if (message.index == replyIndex) {
                matched = &message;
                break;
            }
        }
        if (matched == nullptr) {
            throw std::invalid_argument(
                "replyTo=" + std::to_string(replyIndex) + " 超出本轮消息范围 (本轮共 "
                + std::to_string(turn->messages.size()) + " 条, 编号从 1 开始)"
            );
        }
        request.replyToRef   = matched->replyRef;
        if (!matched->platformMessageId.empty()) {
            request.passiveMsgId = matched->platformMessageId;
        }
    }

    const auto* turn     = deps.engine->turnOf(chatKey);
    const int32_t sent   = turn != nullptr ? turn->sentCount : 0;
    const auto outcome   = deps.outbound->submit(key, request, sent);
    if (!outcome.ok) {
        throw std::runtime_error(outcome.reason.empty() ? std::string{"发送被拒绝"} : outcome.reason);
    }
    deps.engine->noteToolSend(chatKey);

    Json result = Json::object();
    result["queued"] = static_cast<int64_t>(outcome.queued);
    result["parts"]  = static_cast<int64_t>(outcome.parts);
    result["chat"]   = chatKey;
    if (!outcome.reason.empty()) {
        result["note"] = outcome.reason;
    }
    return result.dump();
}

std::string toolHistory(
    const ChatbotToolDeps& deps,
    std::string_view       threadId,
    std::string_view       argsJson
) {
    const auto args = parseArgsOrThrow(argsJson);
    std::string err;
    const std::string chatKey = resolveChat(deps, threadId, stringArg(args, "chat"), err);
    if (chatKey.empty()) {
        throw std::runtime_error(err);
    }
    const auto limit = static_cast<size_t>(std::max<int64_t>(1, intArg(args, "limit", 20)));
    const auto items = deps.engine->historyOf(chatKey, limit);
    if (items.empty()) {
        return "（这个会话还没有收到过消息）";
    }
    std::string out;
    out += "会话 " + chatKey + " 最近 " + std::to_string(items.size()) + " 条原始消息:\n";
    for (const auto& item : items) {
        std::string line = "[" + std::to_string(item.index) + "] ";
        line += item.senderName.empty() ? std::string{"(未知)"} : item.senderName;
        if (!item.senderRole.empty()) {
            line += "(" + item.senderRole + ")";
        }
        if (item.mentionedMe) {
            line += " @机器人";
        }
        line += ": ";
        line += item.text;
        if (item.attachmentCount > 0) {
            line += " [附件 " + std::to_string(item.attachmentCount) + " 个]";
        }
        out += line;
        out += '\n';
    }
    return trimCopy(out);
}

std::string toolChatInfo(
    const ChatbotToolDeps& deps,
    std::string_view       threadId,
    std::string_view       argsJson
) {
    const auto args = parseArgsOrThrow(argsJson);
    std::string err;
    const std::string chatKey = resolveChat(deps, threadId, stringArg(args, "chat"), err);
    if (chatKey.empty()) {
        throw std::runtime_error(err);
    }
    return deps.engine->chatInfoJson(chatKey);
}

std::string toolStatus(
    const ChatbotToolDeps& deps,
    std::string_view       threadId,
    std::string_view       argsJson
) {
    (void)threadId;
    (void)argsJson;
    if (deps.engine == nullptr) {
        throw std::runtime_error("chatbot 插件状态不可用");
    }
    Json out = Json::object();
    try {
        out = Json::parse(deps.engine->statusJson());
    } catch (const std::exception&) {
        out = Json::object();
    }
    // 平台侧状态 (连接/重连/收发计数) 一并给出
    Json platforms = Json::array();
    if (deps.platformOf && deps.router != nullptr) {
        for (const auto& account : deps.engine->config().accounts) {
            if (auto* platform = deps.platformOf(account.id); platform != nullptr) {
                try {
                    platforms.push_back(Json::parse(platform->statusJson()));
                } catch (const std::exception&) {
                    platforms.push_back(Json{{"account", account.id}});
                }
            }
        }
    }
    out["platforms"] = std::move(platforms);
    return out.dump();
}

std::string toolSetMode(
    const ChatbotToolDeps& deps,
    std::string_view       threadId,
    std::string_view       argsJson
) {
    const auto args = parseArgsOrThrow(argsJson);
    const auto modeText = stringArg(args, "mode");
    ChatMode   mode     = ChatMode::Mention;
    if (!chatModeFromString(modeText, mode)) {
        throw std::invalid_argument("mode 只能是 mention / always / off");
    }
    std::string err;
    const std::string chatKey = resolveChat(deps, threadId, stringArg(args, "chat"), err);
    if (chatKey.empty()) {
        throw std::runtime_error(err);
    }
    if (!deps.engine->setMode(chatKey, mode, err)) {
        throw std::runtime_error(err);
    }
    Json out = Json::object();
    out["chat"] = chatKey;
    out["mode"] = chatModeName(mode);
    out["note"] = "运行期生效, 不写回配置文件";
    return out.dump();
}

std::string toolDebugInject(const ChatbotToolDeps& deps, std::string_view argsJson) {
    const auto args = parseArgsOrThrow(argsJson);
    if (deps.engine == nullptr) {
        throw std::runtime_error("chatbot 插件状态不可用");
    }
    InboundEvent event;
    event.accountId         = stringArg(args, "account");
    event.chatId            = stringArg(args, "chatId");
    event.chatName          = stringArg(args, "chatName", event.chatId);
    event.senderName        = stringArg(args, "sender", "tester");
    event.senderId          = event.senderName;
    event.senderRole        = stringArg(args, "role", "member");
    event.text              = stringArg(args, "text");
    event.platformMessageId = stringArg(args, "msgId");
    event.mentionedMe       = boolArg(args, "mentionedMe", false);
    event.timestampMs       = intArg(args, "timestampMs", 0);
    const std::string typeText = stringArg(args, "type", "group");
    if (!chatTypeFromString(typeText, event.type)) {
        throw std::invalid_argument("type 只能是 group / private");
    }
    if (event.accountId.empty() || event.chatId.empty()) {
        throw std::invalid_argument("account 与 chatId 必填");
    }
    deps.engine->onInbound(event);
    Json out = Json::object();
    out["accepted"] = true;
    out["chat"]     = ChatKey{event.accountId, event.type, event.chatId}.key();
    return out.dump();
}

std::string toolDebugOutbox(const ChatbotToolDeps& deps, std::string_view argsJson) {
    const auto args       = parseArgsOrThrow(argsJson);
    const auto limit      = static_cast<size_t>(std::max<int64_t>(0, intArg(args, "limit", 50)));
    const bool clear      = boolArg(args, "clear", false);
    const auto accountId  = stringArg(args, "account");
    if (!deps.platformOf) {
        throw std::runtime_error("chatbot 插件状态不可用");
    }
    if (!accountId.empty()) {
        auto* platform = deps.platformOf(accountId);
        if (platform == nullptr) {
            throw std::runtime_error("账号 '" + accountId + "' 没有可用连接");
        }
        return platform->debugOutbox(limit, clear);
    }
    Json all = Json::array();
    if (deps.engine != nullptr) {
        for (const auto& account : deps.engine->config().accounts) {
            auto* platform = deps.platformOf(account.id);
            if (platform == nullptr) {
                continue;
            }
            try {
                Json one = Json::parse(platform->debugOutbox(limit, clear));
                for (auto& item : one) {
                    all.push_back(std::move(item));
                }
            } catch (const std::exception&) {
            }
        }
    }
    return all.dump();
}

} // namespace chatbot
} // namespace agentxx
