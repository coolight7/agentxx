#include "chatbot_tools.h"

#include "chatbot_core.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <algorithm>

namespace agentxx {
namespace chatbot {

namespace {

constexpr const char* kLogTag = "chatbot";

/// 可撤回的最近消息条数上限
/// - 必须与 [ChatbotOutbound] 内部保留的 `kMaxRecentIds` 一致: 那边只记最近 8 条
///   发出去的消息 id (撤回只能作用于本进程发出的、且仍在平台时限内的消息)
constexpr size_t kRecallLookback = 8;

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

namespace {

/// 解析注入参数里的 `attachments` 数组 (每项形如
/// `{"kind":"image","url":"http://…","localPath":"C:\\…","fileName":"a.png","sizeBytes":12}`)
void applyDebugAttachments(const Json& args, InboundEvent& event) {
    if (!args.contains("attachments") || !args["attachments"].is_array()) {
        return;
    }
    for (const auto& item : args["attachments"]) {
        if (!item.is_object()) {
            continue;
        }
        MediaRef ref;
        const std::string kindText = stringArg(item, "kind", "file");
        if (kindText == "image") {
            ref.kind = MediaKind::Image;
        } else if (kindText == "audio") {
            ref.kind = MediaKind::Audio;
        } else if (kindText == "video") {
            ref.kind = MediaKind::Video;
        } else {
            ref.kind = MediaKind::File;
        }
        ref.url       = stringArg(item, "url");
        ref.localPath = stringArg(item, "localPath");
        ref.fileName  = stringArg(item, "fileName");
        ref.mimeType  = stringArg(item, "mimeType");
        ref.asrText   = stringArg(item, "asrText");
        const auto size = intArg(item, "sizeBytes", 0);
        ref.sizeBytes   = size > 0 ? static_cast<uint64_t>(size) : 0;
        if (ref.fileName.empty() && !ref.localPath.empty()) {
            const auto slash = ref.localPath.find_last_of("/\\");
            ref.fileName = slash == std::string::npos ? ref.localPath
                                                      : ref.localPath.substr(slash + 1);
        }
        event.attachments.push_back(std::move(ref));
    }
}

} // namespace

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
    event.quotedText        = stringArg(args, "quotedText");
    event.quotedSender      = stringArg(args, "quotedSender");
    event.replyRef          = stringArg(args, "replyRef");
    applyDebugAttachments(args, event);
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

std::string toolRecallMessage(
    const ChatbotToolDeps& deps,
    std::string_view       threadId,
    std::string_view       argsJson
) {
    const auto args = parseArgsOrThrow(argsJson);
    if (deps.outbound == nullptr) {
        throw std::runtime_error("chatbot 插件状态不可用");
    }
    std::string       err;
    const std::string chatKey = resolveChat(deps, threadId, stringArg(args, "chat"), err);
    if (chatKey.empty()) {
        throw std::runtime_error(err);
    }
    if (!deps.platformOf) {
        throw std::runtime_error("没有可用的平台连接, 无法撤回");
    }
    const ChatKey key = parseChatKey(chatKey);

    // 最近发过的消息 id (旧→新), 用于按序号撤回: 末尾就是刚发的那条
    const auto recent = deps.outbound->recentMessageIds(chatKey, kRecallLookback);

    // 目标: `messageId` 直接给 id; `target: "-2"` 撤"倒数第 2 条";
    // 否则按 `skip` + `count`: 从最近一条往前跳过 skip 条, 再撤 count 条
    std::string       explicitId = stringArg(args, "messageId");
    const std::string targetArg  = stringArg(args, "target");
    int64_t           skipCount  = intArg(args, "skip", 0);
    std::string       targetErr;
    if (explicitId.empty() && !targetArg.empty()) {
        if (targetArg.front() == '#') {
            // `#N` 表示"本轮输入里的第 N 条", 这里不支持 (那是 send 的 replyTo 语义);
            // 撤回要的是"自己发的第几条", 用 `-#N` 明确区分
            targetErr = "撤回请用 target=\"-2\" 表示倒数第 2 条 (不是 #N; #N 是引用入站消息)";
        } else if (targetArg.front() == '-') {
            // -1 = 最近一条, -2 = 倒数第二条 ...
            const std::string ordinal = targetArg.substr(1);
            bool              digitsOnly = !ordinal.empty();
            for (const char c : ordinal) {
                if (c < '0' || c > '9') {
                    digitsOnly = false;
                    break;
                }
            }
            if (!digitsOnly) {
                targetErr = "target 只接受 \"-1\" / \"-2\" 这类写法 (倒数第几条), 收到的是 '"
                            + targetArg + "'";
            } else {
                skipCount = std::max<int64_t>(0, std::stoll(ordinal) - 1);
            }
        } else {
            explicitId = targetArg; // 兼容直接写 id 的写法
        }
    }

    std::vector<std::string> targets;
    if (!targetErr.empty()) {
        throw std::invalid_argument(targetErr);
    }
    if (!explicitId.empty()) {
        targets.push_back(explicitId);
    } else {
        const int64_t rawCount = intArg(args, "count", 1);
        if (rawCount < 1) {
            targetErr = "count 必须 >= 1";
        } else if (skipCount < 0) {
            targetErr = "skip 必须 >= 0";
        } else if (recent.empty()) {
            targetErr = "本会话没有可撤回的记录 (只能撤回本进程自己发出且已成功的消息)";
        } else if (static_cast<size_t>(skipCount) >= recent.size()) {
            targetErr = "要跳过的条数超出可撤回范围 (本会话只记录了 "
                        + std::to_string(recent.size()) + " 条)";
        } else {
            const size_t avail = recent.size() - static_cast<size_t>(skipCount);
            const size_t take  = std::min<size_t>(static_cast<size_t>(rawCount), avail);
            for (size_t i = 0; i < take; ++i) {
                // `recent` 是旧→新; 末尾往前跳过 skip 条 (最近的那几条不动), 再连着取
                targets.push_back(
                    recent[recent.size() - 1 - static_cast<size_t>(skipCount) - i]
                );
            }
        }
    }
    if (!targetErr.empty()) {
        throw std::invalid_argument(targetErr);
    }

    auto* platform = deps.platformOf(key.accountId);
    if (platform == nullptr) {
        throw std::runtime_error("账号 '" + key.accountId + "' 的平台连接不可用");
    }

    Json   recalled = Json::array();
    Json   failed   = Json::array();
    size_t skipped  = 0;
    for (const auto& id : targets) {
        const auto result = platform->recallMessage(key.accountId, key.type, key.chatId, id);
        if (result.ok) {
            recalled.push_back(id);
            // 撤掉的 id 从可撤回列表移除, 否则同一句会被反复选中
            deps.outbound->forgetMessageId(chatKey, id);
            continue;
        }
        if (result.skipped) {
            ++skipped;
            continue;
        }
        failed.push_back(Json{{"messageId", id}, {"reason", result.reason}});
    }

    if (recalled.empty() && failed.empty() && skipped == targets.size()) {
        throw std::runtime_error("当前平台不支持撤回消息");
    }
    if (recalled.empty() && !failed.empty()) {
        // 全部失败: 把第一条原因抛给模型 (通常是超过 2 分钟或已撤回)
        throw std::runtime_error(failed[0].value("reason", std::string{"撤回失败"}));
    }

    Json out = Json::object();
    out["chat"]     = chatKey;
    out["recalled"] = std::move(recalled);
    if (!failed.empty()) {
        out["failed"] = std::move(failed);
    }
    out["note"] = "撤回只对发送后 2 分钟内的消息有效";
    return out.dump();
}

} // namespace chatbot
} // namespace agentxx
