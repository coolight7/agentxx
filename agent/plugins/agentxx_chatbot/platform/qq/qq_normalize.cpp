#include "qq_normalize.h"

#include "../../chatbot_core.h"
#include "../../chatbot_time.h"

#include "utilxx_base/log.h"

#include <map>
#include <string>
#include <string_view>

namespace agentxx {
namespace chatbot {

namespace {

constexpr const char* kLogTag = "chatbot";

/// 去首尾空白 (平台给的昵称偶尔带空格)
std::string_view trimView(std::string_view text) {
    while (!text.empty() && static_cast<unsigned char>(text.front()) <= ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && static_cast<unsigned char>(text.back()) <= ' ') {
        text.remove_suffix(1);
    }
    return text;
}

/// 取更可靠的 @ 判定依据 (`EventType` > `Openid` > `Nickname` > `None`)
MentionSelfSource strongerMentionSelf(MentionSelfSource a, MentionSelfSource b) {
    return static_cast<uint8_t>(b) > static_cast<uint8_t>(a) ? b : a;
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

/// 成员 id → 昵称 (正文里的 @ 标记换成昵称时查它)
using MentionNames = std::map<std::string, std::string, std::less<>>;

/// 一条消息的 @ 情况 (`mentions[]` 与正文标记共同填充)
struct MentionScan {
    /// @ 的是不是自己, 以及凭什么认出来的
    MentionSelfSource self = MentionSelfSource::None;
    /// 出现了 `<@all>`
    bool all = false;
    /// 被 @ 的其他成员昵称 (与 `bots` 不重叠)
    std::vector<std::string> others;
    /// 被 @ 的其它机器人昵称
    std::vector<std::string> bots;
    /// 诊断用: 每一项写成 `昵称(人/bot, id 尾号 xxxx)`
    std::vector<std::string> describe;
};

/// 记一条「@ 的是我」的依据 (保留更可靠的那个)
void noteSelf(MentionScan& scan, MentionSelfSource source) {
    scan.self = strongerMentionSelf(scan.self, source);
}

/// 追加不重复的昵称
void appendNameUnique(std::vector<std::string>& target, std::string_view name) {
    if (name.empty()) {
        return;
    }
    for (const auto& one : target) {
        if (one == name) {
            return;
        }
    }
    target.emplace_back(name);
}

/// 读 `mentions[]`: 被 @ 的成员 id / 昵称, 以及“里面有没有自己”
///
/// **不能用「`bot: true`」当「@ 了我」的依据** —— 群里通常还有别的机器人
/// (元宝之类), @ 它的时候平台同样给 `bot: true`, 以前就是这样把“@ 别人”当成
/// “@ 机器人”的。只能靠自身 openid 或机器人昵称。
///
/// - 机器人自己的群 openid 与 `id` 不一定是同一个值, 所以 `id` / `member_openid` /
///   `user_openid` / `union_openid` 都记一份 (正文标记用哪个都能对上)
/// - 昵称到自己的那一项会把 openid 学进对应群 (仅当平台标了 `bot: true`),
///   之后就靠 openid 精确判定
void collectMentions(
    const utilxx_base::Json&  data,
    std::string_view          groupId,
    QqSelfIdentity*           self,
    std::vector<std::string>& mentionIds,
    MentionNames&             names,
    MentionScan&              scan
) {
    if (!data.contains("mentions") || !data["mentions"].is_array()) {
        return;
    }
    for (const auto& item : data["mentions"]) {
        if (!item.is_object()) {
            continue;
        }
        const std::string name  = item.value("username", std::string{});
        const bool        isBot = item.value("bot", false);
        // 一个成员可能同时带多种 id (群 openid / union openid), 全部收下
        std::vector<std::string> ids;
        for (const char* key : {"id", "member_openid", "user_openid", "union_openid"}) {
            const std::string id = item.value(key, std::string{});
            if (!id.empty()) {
                ids.push_back(id);
            }
        }
        // 1) 先按 openid 认自己 (最可靠)
        MentionSelfSource selfSource = MentionSelfSource::None;
        bool              isAll      = false;
        for (const auto& id : ids) {
            if (id == "all") {
                isAll = true;
            }
        }
        if (self != nullptr && !isAll) {
            for (const auto& id : ids) {
                if (self->matches(id)) {
                    selfSource = MentionSelfSource::Openid;
                    break;
                }
            }
            // 2) 认不出来再比昵称 (自身 openid 还没学到时的兜底);
            //    只对 `bot: true` 的那项学 openid, 免得同名成员把自己的 id 印进身份表
            if (selfSource == MentionSelfSource::None && self->matchesName(name)) {
                selfSource = MentionSelfSource::Nickname;
                if (isBot) {
                    for (const auto& id : ids) {
                        self->addGroup(groupId, id);
                    }
                }
            }
        }
        for (const auto& id : ids) {
            if (id == "all") {
                continue;
            }
            mentionIds.push_back(id);
            if (!name.empty()) {
                names.emplace(id, name);
            }
        }
        // 诊断串: `小助手(bot, id 尾号 3f2a, =我)`
        std::string label = name.empty() ? std::string{"无名"} : name;
        label += isBot ? "(bot" : "(人";
        label += ids.empty() ? std::string{", 无 id"}
                             : ", id 尾号 " + chatIdTail(ids.front(), 4);
        if (selfSource != MentionSelfSource::None) {
            label += ", =我:";
            label += mentionSelfSourceName(selfSource);
        }
        label += ")";
        scan.describe.push_back(std::move(label));

        if (isAll) {
            // `mentions[]` 里出现 all: 与正文 `<@all>` 同义, 只当信息, 不算 @ 我
            scan.all = true;
            continue;
        }
        if (selfSource != MentionSelfSource::None) {
            noteSelf(scan, selfSource);
            continue;
        }
        if (name.empty()) {
            // 没昵称就只留 id (正文里会渲染成 `@其他成员`), 不给展示列表添乱
            continue;
        }
        if (isBot) {
            appendNameUnique(scan.bots, name);
        } else {
            appendNameUnique(scan.others, name);
        }
    }
}

/// 正文里的 @ 标记渲染结果
struct MentionMarkup {
    std::string text;        ///< 渲染后的正文 (`<@openid>` → `@昵称`)
    bool        all  = false; ///< 出现了 `<@all>` (@全体成员)
    bool        self = false; ///< 出现了机器人自己的 openid
};

/// 把正文里的 `<@openid>` / `<@all>` 换成可读的 `@昵称`
///
/// 全量消息的正文把 @ 写成 `<@` + 成员 openid + `>` (官方文档 group_message_create)。
/// 原样交给模型会变成“@7A3B9C1D…”这种 openid, 模型会当成正文念出来, 所以先换掉。
/// 认不出昵称时写成 `@其他成员` —— 不写“@成员”那种含糊的说法, 免得模型把它当成自己。
MentionMarkup renderMentionMarkup(
    std::string_view      text,
    const QqSelfIdentity* self,
    const MentionNames&   names
) {
    MentionMarkup out;
    std::string   result;
    result.reserve(text.size());
    size_t pos     = 0;
    bool   changed = false;
    while (true) {
        const size_t open = text.find("<@", pos);
        if (open == std::string_view::npos) {
            break;
        }
        const size_t close = text.find('>', open + 2);
        if (close == std::string_view::npos) {
            break;
        }
        result.append(text.substr(pos, open - pos));
        // 少数客户端会写成 `<@!openid>`; 前导 `!` 不是 id 的一部分
        std::string_view id = text.substr(open + 2, close - open - 2);
        if (!id.empty() && id.front() == '!') {
            id.remove_prefix(1);
        }
        changed = true;
        if (id == "all") {
            out.all = true;
            result += "@全体成员";
        } else {
            const bool isSelf = self != nullptr && self->matches(id);
            if (isSelf) {
                out.self = true;
            }
            result += "@";
            const auto it = names.find(std::string{id});
            if (it != names.end() && !it->second.empty()) {
                result += it->second;
            } else if (isSelf && self != nullptr && !self->username.empty()) {
                // 认得出是自己但没昵称: 用 READY 给的名字 (总比 `@其他成员` 强)
                result += self->username;
            } else if (isSelf) {
                // 连名字都不知道但确定是自己: 写 `@你`, 不能写成别人
                result += "你";
            } else {
                result += "其他成员";
            }
        }
        pos = close + 1;
    }
    if (!changed) {
        out.text.assign(text);
        return out;
    }
    result.append(text.substr(pos));
    out.text = std::move(result);
    return out;
}

} // namespace

bool QqSelfIdentity::matchesName(std::string_view name) const {
    if (username.empty() || name.empty()) {
        return false;
    }
    return trimView(name) == trimView(username);
}

bool QqSelfIdentity::addGroup(std::string_view groupId, std::string_view id) {
    if (id.empty()) {
        return false;
    }
    bool learned = add(id);
    if (!groupId.empty()) {
        // 同一个群只有一个自己; 已有值不覆盖 (先学到的那个就是对的)
        const auto [it, inserted]
            = groupOpenids.try_emplace(std::string{groupId}, std::string{id});
        (void)it;
        learned = learned || inserted;
    }
    return learned;
}

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
    InboundEvent&            out,
    QqSelfIdentity*          self
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
    // @ 判定: 「@机器人」事件与单聊天然算 @ (最可靠); 全量消息事件本身不带 @ 语义,
    // 要到下面从 `mentions[]` 与正文标记里补判 (官方文档: GROUP_MESSAGE_CREATE
    // 是群里每条消息都推, content 里的 @ 写成 `<@成员openid>`)
    out.mentionSelf = (eventName == "GROUP_AT_MESSAGE_CREATE"
                       || eventName == "C2C_MESSAGE_CREATE")
                          ? MentionSelfSource::EventType
                          : MentionSelfSource::None;
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

    // ---- @ 相关: `mentions[]` (被 @ 的成员) 与正文里的 `<@openid>` 标记 ----
    // 两种形态都要看: 全量模式下 @ 别人写成 `<@id>`, 而 @ 机器人自己既可能在
    // `mentions[]` 里 (按自己的 openid 或机器人昵称认出来), 也可能只在正文标记里。
    // 识别不出“自己”时就当没 @ 我: 宁可漏一次唤醒 (配 `selfOpenid` 可解决),
    // 也不能把“@ 别人”当成“@ 我”——后者会让模型对群里每一句 @ 都抢着回。
    MentionNames             names;
    MentionScan              scan;
    // 正文标记只按**这条消息之前**就已知的身份判定: `collectMentions` 可能会靠昵称
    // 现场学到本群的自身 openid, 拿学完的身份回看正文会让判定依据从 `nickname`
    // 变成 `openid` (统计与日志上就分不清当时到底靠什么认出来的)
    QqSelfIdentity beforeEvent;
    if (self != nullptr) {
        beforeEvent = *self;
    }
    collectMentions(data, out.chatId, self, out.mentionIds, names, scan);
    if (!out.text.empty()) {
        const auto markup = renderMentionMarkup(
            out.text,
            self != nullptr ? &beforeEvent : nullptr,
            names
        );
        out.text          = markup.text;
        out.mentionAll    = markup.all;
        if (markup.self) {
            // 正文里的 openid 命中自身: 与 `mentions[]` 里的 openid 同一个可信度
            noteSelf(scan, MentionSelfSource::Openid);
        }
    }
    out.mentionAll   = out.mentionAll || scan.all;
    out.mentionSelf  = strongerMentionSelf(out.mentionSelf, scan.self);
    out.mentionOthers = std::move(scan.others);
    out.mentionBots   = std::move(scan.bots);
    if (out.fromFullMode) {
        // 全量消息的 @ 判定只能靠推断: 把依据打出来, 群里“@ 别人却回了一句”时
        // 开 debug 日志就能对上到底认成了谁
        std::string described;
        for (size_t i = 0; i < scan.describe.size() && i < 8; ++i) {
            if (!described.empty()) {
                described += ", ";
            }
            described += scan.describe[i];
        }
        XX_LOGD(
            "[{}] 全量消息 @ 判定 (群尾号 {}): {} | mentions=[{}]",
            kLogTag,
            chatIdTail(out.chatId, 4),
            mentionSelfSourceName(out.mentionSelf),
            described
        );
    }

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
            // 被引用内容里的 @ 标记同样换成昵称 (只为了好读, 不算“@ 了我”)
            if (!out.quotedText.empty()) {
                MentionNames  quotedNames;
                MentionScan   quotedScan;
                std::vector<std::string> quotedIds;
                collectMentions(quoted, std::string{}, nullptr, quotedIds, quotedNames, quotedScan);
                out.quotedText = renderMentionMarkup(out.quotedText, nullptr, quotedNames).text;
            }
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

    // 时间戳按平台自带的时区后缀解析成真正的时间点 (以前把 `+08:00` 当 UTC,
    // 消息显示钟点会整体偏一个时区, 模型据此谈时间就对不上了)。
    // 平台没给时间戳时退回本机时钟 (此时时区未知, 显示按配置的时区兜底)。
    if (data.contains("timestamp")) {
        const auto parsed = parseRfc3339Time(data.value("timestamp", std::string{}));
        out.timestampMs            = parsed.ms;
        out.timestampOffsetMinutes = parsed.offsetMinutes;
    }
    if (out.timestampMs <= 0) {
        out.timestampMs            = systemNowMs();
        out.timestampOffsetMinutes = kUnknownTzOffset;
    }
    if (out.text.empty() && out.attachments.empty()) {
        // 纯空消息 (表情/系统提示): 不值得叫醒模型
        return false;
    }
    return true;
}

} // namespace chatbot
} // namespace agentxx
