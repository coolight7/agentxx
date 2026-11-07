#pragma once

/// chatbot 归一化数据结构 (平台层与调度层之间只交换这些类型)
///
/// 设计见 `resource/history/chatbot/plan.md` §13.1: 加一个新平台只需实现
/// `IPlatform` 并把事件归一化成 [InboundEvent], 把回复收敛成 [OutboundMessage]。
///
/// 注意: 本头不引用任何主程序内部头 (插件边界), 只用标准库。
#include "chatbot_time.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace chatbot {

/// 会话类型 (群聊 / 单聊)
enum class ChatType : uint8_t {
    Group   = 0,
    Private = 1,
};

/// 触发模式
/// - `Mention`: 只处理 @ 到机器人的消息 (全量消息仍进缓冲作为上下文)
/// - `Always`: 任何新消息都投递
/// - `Off`: 该会话完全不处理
enum class ChatMode : uint8_t {
    Mention = 0,
    Always  = 1,
    Off     = 2,
};

/// 「这条消息 @ 了谁」里，判定「@ 的是机器人自己」的依据
///
/// 值越大越可靠: 合并两份同 msgId 的事件 ([mergeSameMessage]) 时取较大的那个。
/// 只认这三种来源 —— 「`mentions[]` 里有人 `bot: true`」**不是**依据: 群里通常还有
/// 别的机器人 (元宝之类), @ 它的时候平台同样报 `bot: true`。
enum class MentionSelfSource : uint8_t {
    /// 没 @ 机器人
    None = 0,
    /// `mentions[]` 里的昵称等于机器人昵称 (自身 openid 还没学到时的兜底)
    Nickname = 1,
    /// `mentions[]` / 正文 `<@openid>` 里的 id 命中已知的自身 openid
    Openid = 2,
    /// 平台「@机器人」事件本身 (`GROUP_AT_MESSAGE_CREATE` / 单聊)
    EventType = 3,
};

/// 附件大类
/// - 主程序的多模态类型只有 image / audio / video, 普通文件不作为附件投递,
///   只把本地路径写进文本 (见 plan.md §11.1)
enum class MediaKind : uint8_t {
    Image = 0,
    Audio = 1,
    Video = 2,
    File  = 3,
};

/// 平台连接状态
enum class PlatformState : uint8_t {
    Connecting = 0,
    Online     = 1,
    Reconnecting = 2,
    Disabled     = 3,
};

const char* chatTypeName(ChatType type) noexcept;
const char* chatModeName(ChatMode mode) noexcept;
const char* mentionSelfSourceName(MentionSelfSource source) noexcept;
const char* mediaKindName(MediaKind kind) noexcept;
const char* platformStateName(PlatformState state) noexcept;

/// 文本转枚举 (不认识返回 false, 不修改 out)
bool chatTypeFromString(std::string_view text, ChatType& out) noexcept;
bool chatModeFromString(std::string_view text, ChatMode& out) noexcept;

/// 一条附件 (收与发共用)
struct MediaRef {
    /// 平台给的下载地址 (入站) 或调用方给的 http(s) 地址 (出站)
    std::string url;
    /// 本地落盘路径 (入站下载完成后 / 出站模型给的本地文件)
    std::string localPath;
    std::string fileName;
    std::string mimeType;
    MediaKind   kind      = MediaKind::File;
    uint64_t    sizeBytes = 0;
    /// 语音的平台识别文本 (可空; 有它就不必再让模型听音频)
    std::string asrText;
};

/// 平台归一化后的一条入站消息
struct InboundEvent {
    std::string accountId;
    ChatType    type = ChatType::Group;
    std::string chatId;
    /// 群名 / 用户昵称 (平台可能给不出来, 空则用 id 前 8 位展示)
    std::string chatName;
    std::string senderId;
    std::string senderName;
    /// 出站时 @ 这个人要用的 id
    ///
    /// 与 [senderId] 不一定相同: QQ 群里 @ 用户要 `member_openid`, 单聊要
    /// `user_openid`。被动回复窗口过期后改发主动消息时用它把人喊回来
    /// (见 [OutboundMessage::mentionOpenId])。拿不到时留空, 出站退化成文本 `@昵称`。
    std::string senderMentionId;
    /// `owner` / `admin` / `member`; 平台不提供时为空
    std::string senderRole;
    std::string text;
    std::vector<MediaRef> attachments;
    /// 被引用的内容 (已展开成文本)
    std::string quotedText;
    std::string quotedSender;
    /// 平台侧「引用哪条」的句柄 (QQ 是 `message_scene.ext` 里的 `msg_idx`)
    std::string replyRef;
    /// 平台消息 id (被动回复与去重都用它)
    std::string platformMessageId;
    /// 消息发送时间 (epoch 毫秒)
    int64_t     timestampMs = 0;
    /// `timestampMs` 自带的 UTC 偏移 (分钟)
    ///
    /// 平台时间戳通常带时区 (`2026-10-29T12:03:09+08:00`), 解析时既得到时间点,
    /// 也记住这个偏移: `behavior.timezone: auto` 用它把时间点显示成用户所在地区的钟点,
    /// 服务器本身跑在哪个时区都不影响。平台没给时区 = [kUnknownTzOffset]。
    int32_t     timestampOffsetMinutes = kUnknownTzOffset;
    /// 「@ 的是不是机器人自己」及其判定依据
    ///
    /// - 平台给「@机器人」单独事件时由事件类型决定 (最可靠);
    /// - 全量消息事件没有 @ 语义, 由平台层从 `mentions[]` 与正文里的 @ 标记补判
    ///   (见 `qq_normalize.cpp` 的 `collectMentions` / `renderMentionMarkup`)。
    MentionSelfSource mentionSelf = MentionSelfSource::None;
    /// 来自「全量消息」事件 (群里每条消息都推, 不限于 @ 机器人)
    bool fromFullMode = false;
    /// @ 了全体成员 (`<@all>`)
    ///
    /// 只作为信息给模型看, **不算**「@ 了我」(沉默的大群会天天被 @全体成员 叫醒)。
    bool mentionAll = false;
    /// 本条消息里被 @ 的成员 id (不含 `all`; 含机器人自己时同样列出)
    std::vector<std::string> mentionIds;
    /// 被 @ 的其他成员 (昵称; 不含机器人、不含机器人自己、不含 `all`)
    ///
    /// 给模型看「这条 @ 的是谁」, 免得它把「@某某」当成 @ 自己。
    std::vector<std::string> mentionOthers;
    /// 被 @ 的其它机器人 (昵称; 与 [mentionOthers] 不重叠, 单独一份便于诊断)
    std::vector<std::string> mentionBots;

    /// 是否明确 @ 了机器人 (等价于 `mentionSelf != MentionSelfSource::None`)
    bool mentionedMe() const noexcept {
        return mentionSelf != MentionSelfSource::None;
    }
};

/// 一次出站请求 (切分之后的一条)
struct OutboundMessage {
    std::string accountId;
    ChatType    type = ChatType::Group;
    std::string chatId;
    /// 已按平台纯文本形态准备好的正文
    std::string text;
    /// 引用哪条 (平台句柄; 空 = 不引用)
    std::string replyToRef;
    /// 被动回复 id (空 = 主动消息)
    std::string passiveReplyTo;
    /// 被动回复序号 (QQ 的 `msg_seq`; 从 1 开始)
    int32_t passiveSeq = 0;
    /// 主动消息里要 @ 的人 (平台句柄; 空 = 不 @)
    ///
    /// 只在被动回复过期、改发主动消息时填 (见 `ChatbotOutbound::pump`):
    /// 那条 `msg_id` 对应的说话人, 让他在群里被真的 @ 到。
    std::string mentionOpenId;
    /// 被 @ 的人的显示名 (平台不支持真 @ 时写成正文前缀)
    std::string mentionName;
    std::vector<MediaRef> attachments;
    /// 用 markdown 形态发送 (平台能力允许时)
    bool markdown = false;
};

/// 一次出站的结果
struct SendResult {
    bool        ok = false;
    /// 失败原因 (可直接给模型看)
    std::string reason;
    /// 平台消息 id (撤回 / 后续引用用)
    std::string platformMessageId;
    /// 平台给的「引用本条」句柄 (QQ: `ext_info.ref_idx`)
    std::string refIdx;
    /// 平台判定「被动回复窗口已过期 / `msg_id` 无效」
    ///
    /// 这类失败重发主动消息能成功 (窗口过期与序号用尽不同: 后者出站层自己就能算出来)。
    /// 平台认得出来就置位; 出站层另有一份按失败文本判定的兜底, 别的平台也能降级。
    bool passiveExpired = false;
};

/// 出站结果回调
using SendResultFn = std::function<void(const SendResult&)>;

/// 一次撤回的结果
struct RecallResult {
    bool ok = false;
    /// 失败原因 (可直接给模型看)
    std::string reason;
    /// 平台不提供该能力 (与"调用失败"区分: 前者不必重试)
    bool skipped = false;
};

/// 撤回结果回调
using RecallResultFn = std::function<void(const RecallResult&)>;

/// 一个会话的唯一身份
///
/// `key()` 是路由表 / 去重 / 会话 id 的原料, 形如 `qq_main|group|B2C3…`。
struct ChatKey {
    std::string accountId;
    ChatType    type = ChatType::Group;
    std::string chatId;

    std::string key() const;
    bool        valid() const noexcept {
        return !accountId.empty() && !chatId.empty();
    }
    bool operator==(const ChatKey& other) const {
        return accountId == other.accountId && type == other.type && chatId == other.chatId;
    }
};

/// 解析 `qq_main|group|B2C3…` 形态的会话键 (非法返回空 ChatKey)
ChatKey parseChatKey(std::string_view text);

/// 平台能力表 (差异走数据, 不走 if)
struct PlatformCaps {
    /// 是否支持 markdown 渲染
    bool markdown = false;
    /// 是否有「被动回复窗口」(QQ 有: 群 5 分钟 / 单聊 60 分钟)
    bool passiveReply = true;
    /// 是否支持引用回复
    bool quoteReply = false;
    /// 同一条消息能否回复多次 (QQ: 群 5 次 / 单聊 4 次)
    bool multiReply = true;
    /// 单条文本上限 (0 = 未知, 用 behavior.messageSplitLimit)
    size_t textLimit = 0;
    /// 单条消息最多几个附件
    size_t maxAttachments = 0;
    /// 能在出站正文里 @ 指定用户
    ///
    /// QQ 群只有 markdown 消息支持 (`<qqbot-at-user id="member_openid" />`), 由平台实现
    /// 自己选消息类型; 为 false 时出站层清掉 @ 目标, 只留显示名让平台写成文本前缀。
    bool mentionInContent = false;
    /// 被 @ 判定是否只能靠事件类型
    ///
    /// `true` = 平台只有「@机器人」事件带 @ 语义, 全量消息里判不出来;
    /// QQ 在全量消息里还能从 `mentions[]` / 正文 `<@openid>` 标记补判, 所以是 `false`。
    bool mentionByEventType = true;
};

} // namespace chatbot
} // namespace agentxx
