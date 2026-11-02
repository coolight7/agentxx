#pragma once

#include "agentxx/event/naming.h"
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/steady_timer.hpp"
#include "utilxx_base/json.h"
#include <chrono>
#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace asio = ::boost::asio;

namespace neograph::graph {
class CancelToken;
} // namespace neograph::graph

namespace agentxx {
namespace agent {
class AgentContext;
class SessionInputSink;
} // namespace agent

namespace events {
class EventBus;
}

namespace events {

using utilxx_base::Json;

/// 事件种类
/// - `Single`: 单值事件, 首个值到达后自动结束 (再来的值覆盖并计数)
/// - `Stream`: 流式事件, 多次发布, 由 `endEvent` 结束
enum class EventKind : uint8_t { Single, Stream };

/// 事件记录状态
enum class EventState : uint8_t { Open, Ended, Expired };

/// 等待句柄状态
enum class WaitState : uint8_t { Armed, Satisfied, Canceled, TimedOut };

/// 一次投递的结果 (日志/诊断/测试用)
/// - `Queued`:   已受理 (写入待注入输入, 或作为新一轮输入入队)
/// - `Deferred`: 当前没人取, 已写入待注入输入等下一次轮次开始时被取用
/// - `Dropped`:  丢弃 (关停中 / 无会话 / 背压上限), 一定带原因并计数
enum class DeliverResult : uint8_t { Queued, Deferred, Dropped };

/// 事件定义 (登记一个事件源)
struct EventDefineSpec {
    /// 事件名 (hub 内唯一; 建议 `域.用途`, 如 `build.done`)
    std::string name;
    EventKind   kind = EventKind::Stream;
    /// 来源: `tool` / `plugin:<名>` / `core:turn` / `subagent` / `timer`
    std::string owner;
    /// 一行说明 (`list` 与诊断展示; 插件声明时给模型看的说明也在这)
    std::string description;
    /// 首次登记它的会话 (诊断)
    std::string originSession;
    /// 保留多少个历史值供后注册的等待回放 (0 = 不保留; 单值事件恒为 1)
    size_t historyLimit = 8;
    /// 0 = 不限; 到期仍未结束 → `Expired`
    std::chrono::milliseconds ttl{0};
    /// 结束后保留多久 (供查状态与回放); 0 = 立即回收
    std::chrono::milliseconds linger{60000};
};

/// 事件记录 (查询用快照)
struct EventRecord {
    std::string  name;
    EventKind    kind  = EventKind::Stream;
    EventState   state = EventState::Open;
    std::string  owner, description, originSession;
    /// 最近的值 (受 `historyLimit` 限制)
    std::deque<Json> history;
    uint64_t         published          = 0;
    uint64_t         droppedForHistory  = 0;
    uint64_t         overwritten        = 0;
    /// 当前挂在它上面的等待数
    size_t           waitCount = 0;
    /// 保留的历史值条数 (单值事件恒为 1)
    size_t           historyLimit = 8;
    /// 结束后保留多久 (毫秒; 查询/回放期)
    int64_t          lingerMs = 60000;
    int64_t          createdAtMs = 0, updatedAtMs = 0, endedAtMs = 0;
    /// `endEvent` 时附带的值 (可为空)
    Json summary = Json::object();
};

/// 等待登记 (一次 `watch`)
struct EventWaitSpec {
    /// 事件名 (必填)
    std::string event;
    /// `first` = 要一个值 / `each` = 每条都要 / `all` = 等整条流结束再一起给我
    std::string mode = "first";
    /// `next-step` / `inject` / `next-turn` / `none` (仅留在 hub, 供 status 查)
    std::string delivery = "next-step";
    /// 投递目标会话 (必填)
    std::string originSession;
    /// `tool` / `plugin:<名>` / `runtime`
    std::string owner;
    /// 投递文本里的说明 (给人看的一句话)
    std::string label;
    /// 关联 id (如 tool_call_id)
    std::string ref;
    /// 等待超时 (0 = 不限)
    std::chrono::milliseconds timeout{0};
    /// `each`/`all` 的累计上限 (达到即投递一次)
    size_t maxItems = 16;
    size_t maxBytes = 8 * 1024;
    /// 合并窗口 (毫秒): >0 时窗口内到达的多个值合成一次投递 (仅 `each` 生效)
    std::chrono::milliseconds coalesce{0};
    /// 是否消费注册前的历史值
    bool replay = true;
    /// 超时是否投递一条提示
    bool notifyOnTimeout = true;
    /// 暂停态 (上一轮失败/中断) 是否解除暂停
    bool unpausePaused = false;
    /// 会话在跑一轮时直接丢弃这次投递 (插件触发器的 DROP_IF_BUSY 语义)
    bool dropIfBusy = false;
    /// 监听的总线主题 (空 = `evt.<event>`; 插件触发器可直接给 `plugin.<topic>`)
    std::string busTopic;
};

/// 等待句柄 (查询用快照)
struct EventWait {
    /// `w-<n>` (agent 内自增)
    std::string  waitId;
    EventWaitSpec spec;
    WaitState    state = WaitState::Armed;
    /// `all` 模式已累积的值 / `each` 模式最近一次投递的值
    std::vector<Json> values;
    size_t   bytes      = 0;
    uint64_t deliveries = 0;
    int64_t  armedAtMs = 0, lastValueAtMs = 0, closedAtMs = 0;
    /// 结束时刻 (steady 时钟; 内部用于回收终态等待)
    int64_t  closedAtSteadyMs = 0;
    std::string closeReason;
};

/// 一次投递的完整描述 (先渲染文本再交给会话, 便于测试直接断言文本)
struct EventDelivery {
    std::string sessionId, text, source, delivery;
    std::string waitId, eventName, mode, ref;
    bool        unpausePaused = false;
    /// 会话在跑一轮时直接丢弃 (插件触发器的 DROP_IF_BUSY 语义)
    bool        dropIfBusy = false;
};

/// 事件中枢的运行统计 (进诊断与加载快照)
struct EventHubStats {
    uint64_t delivered  = 0;
    uint64_t deferred   = 0;
    uint64_t dropped    = 0;
    uint64_t waitsTimedOut = 0;
    uint64_t valueCount = 0;
    /// 丢弃原因 → 次数 (原因取值如 `no-session` / `shutting-down` / `backlog`)
    std::map<std::string, uint64_t, std::less<>> droppedByReason;
};

/// 事件中枢: 有状态、有值、能结束、能回放的事件表 + 等待句柄 + 投递
///
/// 与 `EventBus` 的分工 (`bus` 负责"通知", 本类负责"值、状态与领取"):
/// - 值从 `EventBus` 的 `evt.*` 前缀订阅进来 (`attachBus`)
/// - 等待满足后按投递决策表把值渲染成一条输入, 经 `SessionInputSink` 交回会话
/// - 生命周期事件发布回总线 (`agent.hub.value` / `agent.hub.ended`), 主题不带
///   `evt.` 前缀, 因此不会被自己的前缀订阅再吃回去
///
/// 线程模型: 所有状态只在构造时给的 executor 线程 (agent io 线程) 读写, 不加锁;
/// 跨线程入口只有 [postValue] / [postEnd]。
class EventHub : public std::enable_shared_from_this<EventHub> {
public:

    /// 预算与默认值 (`AgentConfig::event*` 会覆盖其中一部分)
    struct Config {
        size_t maxEvents             = 256;
        size_t maxWaits              = 256;
        size_t maxWaitsPerSession    = 32;
        size_t valueBytes            = 32 * 1024;
        size_t valueHardBytes        = 1024 * 1024;
        size_t deliveryBacklogPerSession = 64;
        bool   allowHeadlessTurn     = false;
        /// `watch` 未指定 timeout 时使用 (0 = 不限)
        std::chrono::milliseconds waitTimeoutDefault{0};
        /// `watch` 未指定 delivery 时使用
        std::string deliveryDefault = "next-step";
    };

    /// "跑一轮"回调 (由 BaseAgent 注入; 只在 `allowHeadlessTurn` 且没有驱动端时用)
    /// - 末位参数是输入来源标记 (`event:<事件名>`), 与 `SessionPendingInput::source`
    ///   同一套取值, 写进展示消息供界面区分事件输入与用户输入
    using TurnRunner =
        std::function<void(std::string sessionId, std::string text, std::string source)>;

    EventHub(
        asio::any_io_executor                   ex,
        std::weak_ptr<agentxx::agent::AgentContext> ctx,
        Config                                  cfg = {}
    );
    ~EventHub();

    EventHub(const EventHub&)            = delete;
    EventHub& operator=(const EventHub&) = delete;

    asio::any_io_executor executor() const {
        return executor_;
    }

    /// 当前预算配置 (可在 init 阶段按 AgentConfig 覆盖)
    Config& config() noexcept {
        return cfg_;
    }

    /// 注入"跑一轮"回调 (BaseAgent::init)
    void setTurnRunner(TurnRunner runner);

    /// 挂到 agent 总线上 (订阅 `evt.` 与 `plugin.evt.` 两个前缀)
    void attachBus(const std::shared_ptr<agentxx::events::EventBus>& bus);

    /// 退订总线前缀 (关闭时调用)
    void detachBus();

    /// 值 → 给模型看的文本 (超长时转会话 share store)
    /// - 投递文本与工具返回值共用这一处, 保证两边展示一致
    std::string valueText(std::string_view sessionId, const Json& value);

    // -------------------------------------------------------------------
    // 定义 / 发布 (hub 线程)
    // -------------------------------------------------------------------

    bool define(const EventDefineSpec& spec, std::string* err = nullptr);
    bool publishValue(std::string_view name, Json value, std::string* err = nullptr);
    bool endEvent(std::string_view name, Json summary = Json::object(), std::string* err = nullptr);
    bool removeEvent(std::string_view name);

    // -------------------------------------------------------------------
    // 任意线程 (插件 / 外部)
    // -------------------------------------------------------------------

    void postValue(std::string_view name, Json value);
    void postEnd(std::string_view name, Json summary = Json::object());

    // -------------------------------------------------------------------
    // 等待
    // -------------------------------------------------------------------

    /// 登记等待; 已可满足 (回放/已结束) 时直接投递并返回 `Satisfied`
    std::expected<EventWait, std::string> watch(EventWaitSpec spec);

    std::optional<EventWait> queryWait(std::string_view waitId) const;
    std::vector<EventWait>   listWaits(std::string_view sessionId = {}, std::string_view event = {})
        const;
    bool                     cancelWait(std::string_view waitId, std::string_view reason = "Canceled");
    size_t                   cancelWaitsOfSession(std::string_view sessionId, std::string_view reason);

    // -------------------------------------------------------------------
    // 查询
    // -------------------------------------------------------------------

    std::optional<EventRecord> queryEvent(std::string_view name) const;
    Json                       listEventsJson(std::string_view prefix = {}) const;
    Json                       listWaitsJson(std::string_view sessionId = {}) const;
    Json                       statsJson() const;

    const EventHubStats& stats() const noexcept {
        return stats_;
    }

    /// 该事件是否有人关心 (已有记录, 或存在挂起的等待)
    /// - 供 `turn.*` / `tool.*` 这类内建源判断"值得不值得发": 没人等就不建记录,
    ///   否则每轮/每次工具调用都会留一条空记录
    bool wants(std::string_view name) const;

    /// 事件记录数 / 等待数 (测试与诊断)
    size_t eventCount() const noexcept {
        return records_.size();
    }
    size_t waitCount() const noexcept {
        return waits_.size();
    }

    // -------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------

    /// 关闭中枢: 取消全部等待与定时器, 之后所有投递一律丢弃
    void closeAll(std::string_view reason = "closed");

    bool closed() const noexcept {
        return closed_;
    }

    /// 阻塞等待一次 (供 `agentxx_event await`)
    ///
    /// 内部登记一个 `delivery=none` 的临时等待, 结束后一定撤销: 值到达 / 超时 /
    /// 取消令牌被置位三条路径都覆盖; 取消时抛 `CanceledException`。
    asio::awaitable<EventWait> awaitOnce(
        EventWaitSpec                         spec,
        std::shared_ptr<neograph::graph::CancelToken> ct
    );

    // -------------------------------------------------------------------
    // 定时源 (`timer.*`)
    // -------------------------------------------------------------------

    /// 建立/重设一个一次性或周期定时源; 到点向 `timer.<name>` 发布一个值
    /// - `args`:
    ///     - [name]       定时源名字 (`timer.` 后的部分)
    ///     - [afterMs]    首次触发延迟 (必须 > 0)
    ///     - [intervalMs] > 0 = 周期触发; 0 = 一次性
    /// - `return` false = 参数非法 (名字不合法/延迟非正)
    bool armTimer(std::string_view name, int64_t afterMs, int64_t intervalMs);

    /// 取消一个定时源
    bool cancelTimer(std::string_view name);

    // -------------------------------------------------------------------
    // 超时/回收 (sweep)
    // -------------------------------------------------------------------

    /// 处理到期项 (等待超时 / 事件 ttl 与 linger / 合并窗口 / 定时源)
    /// - 正常路径由内部定时器按 500ms 周期调用; 测试可直接调本函数推进
    /// - `return` 本次处理的条目数
    size_t sweepExpired(int64_t steadyNowMs = -1);

    /// 当前 steady 时间 (毫秒); 供测试与统计对齐
    static int64_t nowSteadyMs();

    // -------------------------------------------------------------------
    // 投递 (公开: 供测试直接驱动决策表)
    // -------------------------------------------------------------------

    /// 按投递决策表把文本交给会话; 失败统一计数并记 WARN
    DeliverResult deliver(const EventDelivery& d, std::string* reason = nullptr);

private:

    struct RecordEntry {
        EventRecord rec;
        int64_t     ttlDeadlineMs    = 0; ///< 0 = 无
        int64_t     lingerDeadlineMs = 0; ///< 0 = 无 (尚未结束)
    };

    struct WaitEntry {
        EventWait     wait;
        int64_t       deadlineMs = 0; ///< 0 = 不限
        /// 合并窗口里的待投递值 (仅 `each` + `coalesce > 0`)
        std::vector<Json> coalesced;
        size_t            coalescedBytes = 0;
        int64_t           coalesceUntilMs = 0;
        /// `awaitOnce` 的回调 (值到达/超时/取消时唤醒等待方)
        std::function<void(const EventWait&)> onSatisfied;
    };

    struct TimerSource {
        std::shared_ptr<asio::steady_timer> timer;
        std::string                         eventName; ///< `timer.<name>`
        int64_t                             intervalMs = 0;
        int64_t                             nextFireMs = 0;
        uint64_t                            fired      = 0;
    };

    asio::any_io_executor                       executor_;
    std::weak_ptr<agentxx::agent::AgentContext> ctx_;
    Config                                      cfg_;
    TurnRunner                                  turnRunner_;

    std::map<std::string, RecordEntry, std::less<>> records_;
    std::map<std::string, WaitEntry, std::less<>>   waits_;
    std::map<std::string, TimerSource, std::less<>> timers_;

    std::shared_ptr<agentxx::events::EventBus> bus_;
    size_t                                     valueSubId_ = 0;
    size_t                                     pluginValueSubId_ = 0;

    std::shared_ptr<asio::steady_timer> sweepTimer_;
    EventHubStats                       stats_;
    uint64_t                            waitSeq_ = 0;
    bool                                closed_  = false;
    /// sweep 定时器是否已安排 (避免重复 arm)
    bool                                sweepArmed_ = false;

    // ---- 内部实现 ----

    RecordEntry* findRecord(std::string_view name);
    const RecordEntry* findRecord(std::string_view name) const;

    /// 建记录 (预算内); 已存在时返回已有项
    RecordEntry* ensureRecord(std::string_view name, std::string_view owner, std::string_view sessionId);

    /// 值到达后匹配等待并投递
    void dispatchValue(RecordEntry& rec, const Json& value);
    /// 事件结束 (含 single 自动结束) 后的等待处理
    void dispatchEnded(RecordEntry& rec);

    /// 投递一个等待的当前值 (渲染 → deliver → 更新计数)
    /// - `args`:
    ///     - [resetValues] 投递后是否清空已投递的值 (each/all 累积语义要清; first 保留,
    ///       否则 `awaitOnce` 返回的等待拿不到值)
    void flushWait(WaitEntry& entry, std::string_view status, bool resetValues = true);

    /// 把等待收尾 (状态 + 关闭原因 + 唤醒 awaitOnce)
    void finishWait(WaitEntry& entry, WaitState state, std::string_view reason, bool notify);

    /// 定时器维护
    void armSweepTimer();
    void scheduleTimer(std::string_view name);
    void fireTimer(std::string_view name);
    void cancelAllTimers();

    /// 丢弃计数
    DeliverResult drop(std::string_view reason);

    /// 投递成功计数 (+ 关键指标 `events.delivered`)
    void noteDelivered();

    /// 中止并清空全部等待 (closeAll 用)
    void cancelAllWaits(std::string_view reason);
};

// ---------------------------------------------------------------------------
// 文本渲染 (纯函数: 便于测试逐字段断言, 也是给模型看的稳定格式)
// ---------------------------------------------------------------------------

/// 挂起成功的返回文本 (`agentxx_event opt=watch` 未立即满足时)
std::string renderWaitArmed(const EventWait& wait);

/// 一次投递的文本 (`[Event] <name> ...`), 也是模型收到的输入正文
/// - 单值给 `value:` 一行, 多值给 `values:` 列表
/// - `valueTexts` 是调用方已经定稿的值文本 (超长值已转 share store, 见 hub.cpp)
/// - `status` 取值: `value` / `values` / `stream-ended` / `stream-open` /
///   `ended-empty` / `timeout`
std::string renderEventText(
    const EventWait&                wait,
    std::string_view                status,
    const std::vector<std::string>& valueTexts,
    std::string_view                note = {}
);

/// 单个等待的状态文本 (`opt=status wait=`)
std::string renderWaitStatus(const EventWait& wait);

/// 事件的状态文本 (`opt=status event=`)
std::string renderEventStatus(const EventRecord& rec);

/// `await` 超时文本
std::string renderAwaitTimeout(const EventWaitSpec& spec, int64_t waitedMs);

} // namespace events
} // namespace agentxx
