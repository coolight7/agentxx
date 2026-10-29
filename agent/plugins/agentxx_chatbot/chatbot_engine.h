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
    std::string senderName;
    std::string senderRole;
    std::string text;
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
    /// 工具栏是否已经产出正文 (`fallbackReply` 用)
    std::string              lastAssistantText;
    int64_t                  startedAtMs = 0;
};

/// 会话计数 (供 `chatbot_status` / `/axx-info`)
struct ChatCounters {
    uint64_t received   = 0;
    uint64_t duplicated = 0;
    uint64_t routed     = 0;
    uint64_t ignored    = 0;
    uint64_t sent       = 0;
    uint64_t dropped    = 0;
    uint64_t failed     = 0;
    uint64_t woken      = 0;
    uint64_t aggregated = 0;
    uint64_t turns      = 0;
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
    void onInbound(const InboundEvent& event);    void onPlatformState(
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

    /// 工具侧入口: 打一次「工具产出了正文」标记 (兜底发送的依据)
    void noteAssistantText(std::string_view chatKey, std::string_view text);

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

    /// 会话状态摘要 (JSON; `/axx-info` 用)
    std::string chatInfoJson(std::string_view chatKey) const;

    /// 平台状态的 JSON (按账号)
    void setAccountStatus(std::string_view accountId, std::string json);

    const ChatbotConfig& config() const noexcept {
        return config_;
    }

    /// 当前时间毫秒 (与出站同一时钟)
    static int64_t nowMs();

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
    std::string assembleInput(const std::string& chatKey, const std::vector<InboundEvent>& batch);

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
};

} // namespace chatbot
} // namespace agentxx
