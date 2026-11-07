#pragma once

/// 入站流水线 + 调度 (平台消息 → 会话输入)
///
/// 一条消息的完整路径 (见 `resource/history/chatbot/plan.md` §7.1):
/// 归一化 → 去重 → 路由 → 关键词命令 → 聚合缓冲 → 组装输入 → `send_input`
///
/// 聚合是这里最核心的一条行为 (§7.3): 一轮耗时期间积累的多条新消息, 在下一轮
/// **全部带上**。实现是「每会话一个缓冲 + 一个静默窗口」:
/// - 被 @ 立刻唤醒 (不等窗口);
/// - `always` 模式每来一条重置窗口, 到期唤醒;
/// - 通道正在忙时不投, 缓冲继续积累, 等本轮 `TURN_END` 再投;
/// - `mention` 模式下未被 @ 的消息只进缓冲, 跟着下一次唤醒一起带上去。
///
/// 线程: 全部入口只在插件 io 线程调用 (通道事件回调会把工作 post 到这里)。
#include "chatbot_config.h"
#include "chatbot_core.h"
#include "chatbot_outbound.h"
#include "chatbot_panel.h"
#include "chatbot_router.h"
#include "chatbot_types.h"
#include "platform/platform.h"

#include "asio/any_io_executor.hpp"
#include "asio/steady_timer.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace chatbot {

/// 一个会话的历史条目 (供 `chatbot_history`)
struct HistoryItem {
    int64_t     index = 0;
    int64_t     timestampMs = 0;
    /// 平台时间戳自带的 UTC 偏移 (分钟; 见 [InboundEvent::timestampOffsetMinutes])
    int32_t     timestampOffsetMinutes = kUnknownTzOffset;
    std::string senderName;
    std::string senderRole;
    std::string text;
    /// 平台消息 id (同一条消息两种事件都推时用来在历史里找到它)
    std::string platformMessageId;
    bool        mentionedMe = false;
    int         attachmentCount = 0;
    /// 是否真的投给了模型 (没有被路由过滤掉的标记)
    bool        delivered = false;
};

/// 一轮输入里的一条消息 (程序侧记账)
struct TurnMessage {
    /// 本轮编号 (1 起, 就是 `[#n]` 里的 n)
    int         index = 1;
    /// 平台消息 id (被动回复用)
    std::string platformMessageId;
    /// 平台引用句柄 (引用回复用)
    std::string replyRef;
    /// @ 这条消息的说话人时平台要的 id (被动回复过期后改发主动消息用)
    std::string senderMentionId;
    /// 说话人显示名 (同上; 平台不支持真 @ 时写成文本前缀)
    std::string senderName;
    bool        mentionedMe = false;
    int64_t     timestampMs = 0;
};

/// 一轮的记账 (本轮结束后清空)
struct TurnContext {
    bool                     active = false;
    uint64_t                 turnSerial = 0;
    std::string              chatKey;
    std::string              personaName;
    /// 已发几条 (`maxRepliesPerTurn` 的依据)
    int32_t                  sentCount = 0;
    /// 模型是否调过 `chatbot_send_message`
    bool                     modelSent = false;
    std::vector<TurnMessage> messages;
    /// 本轮输入里的最后一条 (兜底与默认被动回复 id 都用它)
    std::string              lastPassiveMsgId;
    /// 本轮是否被 @ (兜底策略 `mention` 用)
    bool                     hadMention = false;
    /// 本轮的唤醒原因码 (`mentioned` / `window_expired` / `buffer_full` ...)
    std::string              wakeReason;
    /// 本轮按什么模式跑 (`mention` / `always`)
    ///
    /// 兜底策略 `mention` 不只看 `hadMention`: `always` 会话本来就不需要 @,
    /// 只看 @ 会把那一轮的正文直接丢掉 (群里表现为“@ 了才回”)。
    ChatMode                 mode = ChatMode::Mention;
    /// 工具栏是否已经产出正文 (`fallbackReply` 用)
    std::string              lastAssistantText;
    int64_t                  startedAtMs = 0;
};

/// 会话计数 (供 `chatbot_status` / `/agentxx-cmd info`)
struct ChatCounters {
    uint64_t received   = 0;
    uint64_t duplicated = 0;
    uint64_t routed     = 0;
    uint64_t ignored    = 0;
    uint64_t sent       = 0;
    uint64_t dropped    = 0;
    uint64_t failed     = 0;
    uint64_t woken      = 0;
    /// 其中因为「被 @ 立刻唤醒」的次数 (always 会话里为 0)
    uint64_t wakeMentioned = 0;
    uint64_t aggregated = 0;
    uint64_t turns      = 0;
    /// 其中「会话正在执行一轮时按 next-step 插进本轮」的条数
    uint64_t injected   = 0;
    /// 其中来自群「全量消息」事件的条数 (平台没开「接收所有消息」时恒为 0)
    uint64_t fullMode   = 0;
    int64_t  lastTurnMs = 0;
};

/// 调度层
class ChatbotEngine {
public:

    /// 运行时会话操作的注入点 (由入口文件实现, 转成 SDK 调用)
    struct Runtime {
        /// 投一条输入到通道; 返回运行时错误码 (0 = 成功)
        std::function<int32_t(int64_t channelId, std::string_view inputJson)> sendInput;
        /// 取消通道当前轮次
        std::function<void(int64_t channelId)> cancel;
        /// 应答一个中断
        std::function<void(int64_t channelId, int64_t interruptId, std::string_view values)>
            respondInterrupt;
        /// 通道状态 JSON
        std::function<std::string(int64_t channelId)> channelState;
        /// 运行时清单 JSON (仅 `chatbot_status` 用)
        std::function<std::string()> runtimeList;
        /// 打开一个会话通道 (人设名 + sessionId + 标题 → 通道 id; 失败 0)
        std::function<int64_t(std::string_view, std::string_view, std::string_view)> openChannel;
        /// 关闭一个通道
        std::function<void(int64_t channelId)> closeChannel;
        /// 下载一个入站附件 (平台层实现; 落盘后回填 `localPath`)
        std::function<void(const std::string& accountId, MediaRef& attachment)>
            downloadAttachment;
        /// `/agentxx-cmd task ...` 的后台任务操作 (插件入口实现: 提交/查询/取消/列表)
        ///
        /// - `args` 是命令名后面那段文字 (`submit 帮我查一下…` / `list` / `cancel all`)
        /// - 返回给用户看的回复正文 (不抛异常; 失败也在正文里说清楚)
        std::function<std::string(std::string_view chatKey, std::string_view args)> taskCommand;
    };

    void init(
        asio::any_io_executor ex,
        ChatbotConfig         config,
        ChatbotRouter*        router,
        ChatbotOutbound*      outbound,
        Runtime               runtime
    );

    /// 建路由表 (start 里调; 之后配置不变)
    void buildRoutes();

    /// 平台事件入口 (平台层在插件 io 线程上调用)
    void onInbound(const InboundEvent& event);

    void onPlatformState(
        std::string_view accountId,
        PlatformState    state,
        std::string_view reason
    );

    /// 通道事件 (调度层内部调用; 已在插件 io 线程)
    void onChannelReady(int64_t channelId);
    void onChannelTurnStart(int64_t channelId);
    void onChannelTurnEnd(int64_t channelId, std::string_view text, std::string_view payloadJson);
    void onChannelClosed(int64_t channelId, std::string_view payloadJson);
    void onChannelInterrupt(int64_t channelId, int64_t interruptId, std::string_view payloadJson);
    void onChannelDeltaText(int64_t channelId, std::string_view text);

    /// 工具侧入口: 定位当前会话; 返回会话键 (空 = 定位不到)
    std::string resolveChatKey(std::string_view threadId) const;

    /// 工具侧入口: 当前轮上下文 (按会话键)
    TurnContext*       turnOf(std::string_view chatKey);
    const TurnContext* turnOf(std::string_view chatKey) const;

    /// 工具侧入口: 历史
    std::vector<HistoryItem> historyOf(std::string_view chatKey, size_t limit) const;

    /// 工具侧入口: 会话计数
    ChatCounters countersOf(std::string_view chatKey) const;

    /// 工具侧入口: 把触发模式改成 mention/always/off (运行期生效, 不写回 yaml)
    bool setMode(std::string_view chatKey, ChatMode mode, std::string& err);

    /// 工具侧入口: 记录一次工具发送 (用于「一轮几条」额度与兜底判断)
    void noteToolSend(std::string_view chatKey);

    /// 工具侧入口: 该会话当前的默认被动回复 id (本轮最后一条 / 被 @ 的那条)
    std::string defaultPassiveMsgId(std::string_view chatKey) const;

    /// 工具侧入口: 某个被动回复 id 是谁说的 (被动回复过期后改 @ 他用)
    ///
    /// - `args`:
    ///     - [chatKey] 会话键
    ///     - [platformMessageId] 本轮里那条消息的平台 id
    ///     - [openId] 写回 @ 他要用的平台 id (找不到时不动)
    ///     - [name] 写回显示名
    /// - `return` 本轮里找到这条消息返回 true
    bool mentionTargetOf(
        std::string_view chatKey,
        std::string_view platformMessageId,
        std::string&     openId,
        std::string&     name
    ) const;

    /// 工具侧入口: 打一次「工具产出了正文」标记 (兜底发送的依据)
    void noteAssistantText(std::string_view chatKey, std::string_view text);

    /// 后台任务的结果回到会话 (插件在任务终态调用)
    ///
    /// - 通道存在: 作为一条「系统提示」输入投给会话 (下一轮开始), 由人设组织语言回复
    /// - 通道不存在 (会话已回收/从未创建): 返回 false, 调用方改用 `ChatbotOutbound`
    ///   直接发一条 (被动→主动降级)
    bool deliverTaskResult(std::string_view chatKey, std::string_view text);

    /// 停止: 清空缓冲与定时器
    void shutdown();

    /// 侧边栏「机器人」栏用的快照 (只在插件 io 线程调用)
    ///
    /// - 会话按「正在执行 → 最近有活动」排序;
    /// - 只列真的在用的会话 (有通道, 或收到过消息但通道还没建起来);
    /// - 人设/账号取自配置与当前状态, 与 `chatbot_status` 同源。
    PanelSnapshot panelSnapshot() const;

    /// 是否有会话正在执行一轮 (面板刷新节拍据此决定要不要继续转动加载动画)
    bool hasRunningSession() const;

    /// 状态变化回调 (入口文件据此安排一次面板快照发布)
    ///
    /// 只在可见状态真的会变的地方调 ([notifyChanged]), 进度类高频入口
    /// (`onChannelDeltaText`) 不调; 入口文件自己负责合并短时间内的多次通知。
    void setChangeHook(std::function<void()> hook);

    /// 全局计数与状态 (JSON)
    std::string statusJson() const;

    /// 会话状态摘要 (JSON; `/agentxx-cmd info` 用)
    std::string chatInfoJson(std::string_view chatKey) const;

    /// 平台状态的 JSON (按账号)
    void setAccountStatus(std::string_view accountId, std::string json);

    const ChatbotConfig& config() const noexcept {
        return config_;
    }

    /// 当前时间毫秒 (同 `steady_clock`: 只用来算间隔与“最近活动”, 不是墙上时间)
    static int64_t nowMs();

    /// 一条消息的时间显示 (`MM-DD HH:MM`; 按 `behavior.timezone`, 见 `chatbot_time.h`)
    ///
    /// - `args`:
    ///     - [timestampMs] 消息时间点; <= 0 返回空串
    ///     - [timestampOffsetMinutes] 消息时间戳自带的偏移 ([kUnknownTzOffset] = 没有)
    std::string messageClockText(int64_t timestampMs, int32_t timestampOffsetMinutes) const;

    /// 按配置的时区格式化当前时间 (命令模板的`{now}`)
    ///
    /// - `args`:
    ///     - [pattern] `strftime` 格式串, 如 `%Y-%m-%d %H:%M:%S`
    std::string currentTimeText(std::string_view pattern) const;

    /// 配置的时区展示名 (如 `UTC+08:00` / `Asia/Shanghai UTC+08:00`)
    std::string timezoneText() const;

private:

    /// 一个会话的聚合状态
    struct SessionState {
        std::deque<InboundEvent> items;
        std::shared_ptr<asio::steady_timer> timer;
        /// 缓冲里累积的字符数 (背压依据)
        size_t             chars = 0;
        /// 是否已经有未投递的积压 (用于 `flushStaleOnNextMessage`)
        bool               stale = false;
        ChatCounters       counters;
        std::deque<HistoryItem> history;
        int64_t            historyIndex = 0;
        TurnContext        turn;
        /// 上一条投递失败的原因 (写进下一轮元信息)
        std::string        pendingNotice;
        /// 最近一次活动时间 (本机时钟毫秒; 面板排序用)
        int64_t            lastActiveMs = 0;
    };

    /// 通知状态变化 (面板快照发布)
    void notifyChanged();

    SessionState& stateOf(const std::string& chatKey);

    /// 投递缓冲 (通道空闲时组装并 send_input)
    void flush(const std::string& chatKey, std::string_view reason);

    /// 组装一次输入 JSON
    ///
    /// - `args`:
    ///     - [reason] 唤醒原因码 (写进输入块, 见 [wakeReasonNote])
    ///     - [delivery] 投递模式: `next-turn` = 排队成新一轮; `next-step` = 插进正在跑的这轮
    ///     - [hasAttachments] 非空时写回“本次是否带了多模态附件” (投递被拒时据此重试)
    std::string assembleInput(
        const std::string&               chatKey,
        const std::vector<InboundEvent>& batch,
        std::string_view                 reason,
        std::string_view                 delivery,
        bool*                            hasAttachments = nullptr
    );

    /// 把重复到达的同 msgId 事件合并进缓冲/历史
    ///
    /// - `return` true = 这条消息因此从「没 @ 我」变成「@ 了我」(调用方要立刻唤醒)
    bool mergeDuplicateInto(
        const std::string&  chatKey,
        SessionState&       state,
        const InboundEvent& event
    );

    /// 处理关键词命令; 返回 true = 已被命令消费 (不进模型)
    bool handleCommand(
        const std::string&       chatKey,
        const InboundEvent&      event,
        const ParsedCommand&     command
    );

    /// 命令模板变量
    std::map<std::string, std::string, std::less<>> commandVars(
        const std::string&  chatKey,
        const InboundEvent& event
    ) const;

    /// 兜底发送 (模型没调工具时)
    void fallbackSend(const std::string& chatKey, TurnContext& turn);

    /// 附件接收 (按 behavior.attachments.fetch 决定 URL 直给还是先落盘)
    void prepareAttachments(const std::string& chatKey, InboundEvent& event);

    /// 配置里是否有群聊绑定成 `always` (只有这类会话才需要群全量消息)
    bool hasAlwaysGroupBind() const;

    /// 账号上线后起一个一次性自检: 一直收不到群全量消息就提示去开平台权限
    void scheduleFullModeHint(std::string_view accountId);

    /// 追加历史 (含被过滤掉的)
    void appendHistory(const std::string& chatKey, const InboundEvent& event);

    asio::any_io_executor      ex_;
    ChatbotConfig              config_;
    ChatbotRouter*             router_ = nullptr;
    ChatbotOutbound*           outbound_ = nullptr;
    Runtime                    runtime_;
    std::map<std::string, SessionState, std::less<>> sessions_;
    /// 账号 → 平台状态 JSON
    std::map<std::string, std::string, std::less<>> accountStatus_;
    /// 账号 → 去重环 (同 msg_id 只处理一次)
    std::map<std::string, DedupeRing, std::less<>> dedupe_;
    uint64_t                   activitySeq_ = 0;
    bool                       stopped_     = false;
    std::function<void()>      changeHook_;
    ChatCounters               globalCounters_;
    std::string                startupTimeText_;
    /// 账号 → 是否收到过群「全量消息」事件 (平台是否真的推了非 @ 消息)
    std::map<std::string, bool, std::less<>> accountFullModeSeen_;
    /// 账号 → 上线后的一次性权限自检定时器
    std::map<std::string, std::shared_ptr<asio::steady_timer>, std::less<>>
        fullModeHintTimers_;
};

} // namespace chatbot
} // namespace agentxx
