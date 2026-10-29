#include "qq_normalize.h"

#include "utilxx_base/log.h"

#include <chrono>
#include <ctime>

namespace agentxx {
namespace chatbot {

namespace {

constexpr const char* kLogTag = "chatbot";

int64_t unixNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()
    )
        .count();
}

/// 从 `text[pos, pos+len)` 读十进制数字 (不足 len 位或含非数字返回 -1)
int parseFixedDigits(std::string_view text, size_t pos, size_t len) {
    if (pos + len > text.size()) {
        return -1;
    }
    int value = 0;
    for (size_t i = 0; i < len; ++i) {
        const char c = text[pos + i];
        if (c < '0' || c > '9') {
            return -1;
        }
        value = value * 10 + (c - '0');
    }
    return value;
}

/// RFC3339 时间 ("2026-10-29T07:03:09+08:00") → 毫秒 (解析失败返回 0)
/// - 只认前 19 个字符的 `YYYY-MM-DDTHH:MM:SS`, 时区后缀一律按 UTC 处理
///   (QQ 推送的时间只用于消息排序与去重, 差几个时区不影响结果)
int64_t parseRfc3339Ms(std::string_view text) {
    if (text.size() < 19) {
        return 0;
    }
    const int year   = parseFixedDigits(text, 0, 4);
    const int month  = parseFixedDigits(text, 5, 2);
    const int day    = parseFixedDigits(text, 8, 2);
    const int hour   = parseFixedDigits(text, 11, 2);
    const int minute = parseFixedDigits(text, 14, 2);
    const int second = parseFixedDigits(text, 17, 2);
    if (year < 0 || month < 0 || day < 0 || hour < 0 || minute < 0 || second < 0) {
        return 0;
    }
    std::tm tmBuf{};
    tmBuf.tm_year = year - 1900;
    tmBuf.tm_mon  = month - 1;
    tmBuf.tm_mday = day;
    tmBuf.tm_hour = hour;
    tmBuf.tm_min  = minute;
    tmBuf.tm_sec  = second;
#if XX_IS_WIN_D
    const std::time_t seconds = _mkgmtime(&tmBuf);
#else
    const std::time_t seconds = timegm(&tmBuf);
#endif
    if (seconds <= 0) {
        return 0;
    }
    return static_cast<int64_t>(seconds) * 1000;
}

/// `message_scene.ext` 里的 `key=value` 串解析 (`msg_idx=REFIDX_...\tref_msg_idx=...`)
std::string sceneExtValue(std::string_view ext, std::string_view key) {
    size_t pos = 0;
    while (pos < ext.size()) {
        auto       end = ext.find_first_of("\t;&", pos);
        const auto item = ext.substr(
            pos,
            end == std::string_view::npos ? std::string_view::npos : end - pos
        );
        const auto eq = item.find('=');
        if (eq != std::string_view::npos && item.substr(0, eq) == key) {
            return std::string{item.substr(eq + 1)};
        }
        if (end == std::string_view::npos) {
            break;
        }
        pos = end + 1;
    }
    return {};
}

/// `message_scene.ext` 归一化成 `key=value` 串
///
/// 实测有两种形态 (同一账号的不同事件都出现过):
/// - 单个字符串: `"msg_idx=REFIDX_...\tref_msg_idx=REFIDX_..."`
/// - 字符串数组: `["ref_msg_idx=REFIDX_...", "msg_idx=REFIDX_...", "auth_token=..."]`
/// 数组形态曾被当成字符串取, 结果取到空值 —— 引用回复因此一直没生效。
std::string sceneExtText(const utilxx_base::Json& scene) {
    if (!scene.is_object() || !scene.contains("ext")) {
        return {};
    }
    const auto& ext = scene["ext"];
    if (ext.is_string()) {
        return ext.get<std::string>();
    }
    std::string out;
    if (ext.is_array()) {
        for (const auto& item : ext) {
            if (!item.is_string()) {
                continue;
            }
            if (!out.empty()) {
                out.push_back('\t');
            }
            out += item.get<std::string>();
        }
    }
    return out;
}

/// 群成员角色 (`member_role`); 平台不提供返回空
std::string roleOf(const utilxx_base::Json& author) {
    if (!author.is_object()) {
        return {};
    }
    const auto role = author.value("member_role", std::string{});
    if (role == "owner" || role == "admin" || role == "member") {
        return role;
    }
    return {};
}

/// 把一层 `attachments` 数组解析成附件列表
///
/// - 顶层 `attachments` 是本条消息自己的附件; `msg_elements[].attachments` 是
///   **被引用消息**的附件 (群里引用一个文件问"这是什么"时, 文件只在这里出现)
/// - 语音的识别文本 (平台已经转好) 经 `asrOut` 带回给调用方, 由它拼进正文
void appendAttachments(
    const utilxx_base::Json& list,
    std::vector<MediaRef>&   target,
    std::string*             asrOut
) {
    if (!list.is_array()) {
        return;
    }
    for (const auto& item : list) {
        if (!item.is_object()) {
            continue;
        }
        MediaRef   ref;
        const auto contentType = item.value("content_type", std::string{});
        ref.url       = item.value("url", std::string{});
        ref.fileName  = item.value("filename", std::string{});
        ref.mimeType  = contentType;
        ref.asrText   = item.value("asr_refer_text", std::string{});
        ref.sizeBytes = static_cast<uint64_t>(item.value("size", static_cast<int64_t>(0)));
        if (contentType.rfind("image/", 0) == 0) {
            ref.kind = MediaKind::Image;
        } else if (contentType.rfind("video/", 0) == 0) {
            ref.kind = MediaKind::Video;
        } else if (contentType == "voice" || contentType.rfind("audio/", 0) == 0) {
            ref.kind           = MediaKind::Audio;
            const auto wavUrl = item.value("voice_wav_url", std::string{});
            if (!wavUrl.empty()) {
                ref.url = wavUrl;
            }
            // 语音优先用平台识别文本 (不必让模型听音频)
            if (asrOut != nullptr && !ref.asrText.empty()) {
                if (!asrOut->empty()) {
                    *asrOut += " ";
                }
                *asrOut += ref.asrText;
            }
        } else {
            // `file` 与任何未识别的类型都按普通文件处理
            ref.kind = MediaKind::File;
        }
        if (ref.fileName.empty()) {
            ref.fileName = qqFileNameOfUrl(ref.url);
        }
        if (!ref.url.empty()) {
            target.push_back(std::move(ref));
        }
    }
}

/// 附件的一句话描述 (被引用消息没有正文时用它当引用内容)
///
/// 例如 `[文件] README.md` / `[图片]`; 让模型知道"被引用的是一条带附件的消息"。
std::string describeAttachments(const std::vector<MediaRef>& list) {
    std::string out;
    for (const auto& ref : list) {
        const char* label = "附件";
        switch (ref.kind) {
            case MediaKind::Image:
                label = "图片";
                break;
            case MediaKind::Audio:
                label = "语音";
                break;
            case MediaKind::Video:
                label = "视频";
                break;
            case MediaKind::File:
            default:
                label = "文件";
                break;
        }
        if (!out.empty()) {
            out += " ";
        }
        out += "[";
        out += label;
        out += "]";
        // 图片/语音的 fileName 往往是平台散列名, 没必要念给模型
        if (ref.kind == MediaKind::File && !ref.fileName.empty()) {
            out += " ";
            out += ref.fileName;
        }
    }
    return out;
}

} // namespace

std::string qqFileNameOfUrl(std::string_view url) {
    auto       path  = url;
    const auto query = path.find('?');
    if (query != std::string_view::npos) {
        path = path.substr(0, query);
    }
    const auto slash = path.find_last_of('/');
    if (slash == std::string_view::npos) {
        return path.empty() ? std::string{"attachment"} : std::string{path};
    }
    const auto tail = path.substr(slash + 1);
    // 以 `/` 结尾 (或整条地址就是目录) 时取不到文件名, 给个兜底名
    return tail.empty() ? std::string{"attachment"} : std::string{tail};
}

bool normalizeQqEvent(
    std::string_view         eventName,
    const utilxx_base::Json& data,
    InboundEvent&            out
) {
    if (eventName != "GROUP_AT_MESSAGE_CREATE" && eventName != "GROUP_MESSAGE_CREATE"
        && eventName != "C2C_MESSAGE_CREATE") {
        // 其它事件 (成员增删/消息接收回执等) 只记 debug
        XX_LOGD("[{}] 忽略事件 {}", kLogTag, std::string{eventName});
        return false;
    }
    if (!data.is_object()) {
        return false;
    }
    out = InboundEvent{};
    // @ 判定只看事件类型 (设计文档 §7.2): 全量消息事件不带 @ 语义
    out.mentionedMe = (eventName == "GROUP_AT_MESSAGE_CREATE"
                       || eventName == "C2C_MESSAGE_CREATE");
    out.fromFullMode = (eventName == "GROUP_MESSAGE_CREATE");
    out.type = (eventName == "C2C_MESSAGE_CREATE") ? ChatType::Private : ChatType::Group;

    out.platformMessageId = data.value("id", std::string{});
    const auto& author
        = data.contains("author") ? data["author"] : utilxx_base::Json::object();
    if (author.is_object()) {
        out.senderId = author.value("id", std::string{});
        if (out.senderId.empty()) {
            out.senderId = author.value("member_openid", std::string{});
        }
        if (out.senderId.empty()) {
            out.senderId = author.value("user_openid", std::string{});
        }
        out.senderName = author.value("username", std::string{});
        out.senderRole = roleOf(author);
        if (author.value("bot", false)) {
            // 机器人自己发的消息不再进流程 (平台也可能推回来)
            return false;
        }
    }
    if (out.type == ChatType::Group) {
        out.chatId = data.value("group_openid", std::string{});
    } else {
        out.chatId = out.senderId;
    }
    if (out.chatId.empty()) {
        XX_LOGW("[{}] 事件 {} 缺少会话 id, 已丢弃", kLogTag, std::string{eventName});
        return false;
    }

    out.text = data.value("content", std::string{});

    // 引用消息: `msg_elements[]` 是被引用的那条 (可能带附件), `content` 是引用正文
    //
    // 实测群里"引用一个文件问它是什么"的事件里:
    // - 本条 `content` = 用户打的那句话
    // - `msg_elements[0].content` = 空串 (被引用的是文件消息, 没有正文)
    // - `msg_elements[0].attachments[0]` = `{content_type:"file", filename:"REAME.md",
    //   size, url}` —— 文件只在这里出现, 不看它就永远读不到
    const int messageType = static_cast<int>(data.value("message_type", int64_t{0}));
    if (data.contains("msg_elements") && data["msg_elements"].is_array()
        && !data["msg_elements"].empty()) {
        const auto& quoted = data["msg_elements"][0];
        if (quoted.is_object()) {
            if (quoted.contains("author") && quoted["author"].is_object()) {
                out.quotedSender = quoted["author"].value("username", std::string{});
            }
            out.quotedText = quoted.value("content", std::string{});
            // 被引用消息的附件: 归到本条消息的附件里 (模型才能真正读到文件)
            std::string quotedAsr;
            if (quoted.contains("attachments")) {
                const size_t before = out.attachments.size();
                appendAttachments(quoted["attachments"], out.attachments, &quotedAsr);
                // 引用内容没有正文时用附件描述顶上, 否则模型只看到"(回复 X: )"
                if (out.quotedText.empty() && out.attachments.size() > before) {
                    const std::vector<MediaRef> quotedOnly(
                        out.attachments.begin() + static_cast<std::ptrdiff_t>(before),
                        out.attachments.end()
                    );
                    out.quotedText = describeAttachments(quotedOnly);
                }
            }
            if (!quotedAsr.empty()) {
                if (!out.quotedText.empty()) {
                    out.quotedText += " ";
                }
                out.quotedText += quotedAsr;
            }
        }
    } else if (messageType == 3) {
        // 卡片消息: 用 ark_data 拼一行文本
        const auto& ark
            = data.contains("ark_data") ? data["ark_data"] : utilxx_base::Json{};
        if (ark.is_object()) {
            std::string joined = ark.value("prompt", std::string{});
            if (ark.contains("fields") && ark["fields"].is_array()) {
                for (const auto& field : ark["fields"]) {
                    if (!field.is_object()) {
                        continue;
                    }
                    const auto title = field.value("title", std::string{});
                    const auto desc  = field.value("desc", std::string{});
                    if (!title.empty()) {
                        joined += (joined.empty() ? "" : " | ") + title + ": " + desc;
                    }
                }
            }
            if (!joined.empty()) {
                out.text = joined;
            }
        }
    }

    // `message_scene.ext`: msg_idx 是**本条消息自己**的引用索引 (回复它时要用),
    // ref_msg_idx 是它引用的那条的索引 —— 两者别弄反
    if (data.contains("message_scene")) {
        const std::string ext = sceneExtText(data["message_scene"]);
        if (!ext.empty()) {
            out.replyRef = sceneExtValue(ext, "msg_idx");
        }
    }

    // 本条消息自己的附件 (图片/语音/文件都走这里)
    if (data.contains("attachments")) {
        appendAttachments(data["attachments"], out.attachments, &out.text);
    }

    out.timestampMs = parseRfc3339Ms(data.value("timestamp", std::string{}));
    if (out.timestampMs <= 0) {
        out.timestampMs = unixNowMs();
    }
    if (out.text.empty() && out.attachments.empty()) {
        // 纯空消息 (表情/系统提示): 不值得叫醒模型
        return false;
    }
    return true;
}

} // namespace chatbot
} // namespace agentxx
