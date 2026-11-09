#pragma once

#include "agentxx/event/naming.h"
#include "asio/any_io_executor.hpp"
#include "asio/awaitable.hpp"
#include "asio/steady_timer.hpp"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include <chrono>
#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
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

/// 谁能往这个事件发布值 (创建时声明; 创建者自己不受限)
/// - 位掩码, 可组合
enum class PublishMask : uint32_t {
    None    = 0,        ///< 只有创建者
    Model   = 1u << 0,  ///< 模型侧工具调用 (`agentxx_async_emit`, 归属 `tool`)
    Plugin  = 1u << 1,  ///< 插件 (归属 `plugin:<插件名>`)
    Builtin = 1u << 2,  ///< 内置源 (轮次 / 工具结束 / 定时器 / 后台任务)
    Any     = Model | Plugin | Builtin,
};

/// 发布者类别 (由归属名推断; 用来对照 [PublishMask])
enum class PublishRole : uint8_t { Model, Plugin, Builtin };

/// 等待句柄状态
enum class WaitState : uint8_t { Armed, Satisfied, Canceled, TimedOut };

/// 一次投递的结果 (日志/诊断/测试用)
/// - `Queued`:   已受理 (写入待注入输入, 或作为新一轮输入入队)
/// - `Deferred`: 当前没人取, 已写入待注入输入等下一次轮次开始时被取用
/// - `Dropped`:  丢弃 (关停中 / 无会话 / 背压上限), 一定带原因并计数
enum class DeliverResult : uint8_t { Queued, Deferred, Dropped };

/// 事件定义 (登记一个事件源)
struct EventDefineSpec {
    /// 事件名 (= 总线主题)
    /// - 内置源与模型自建: `agentxx.agent.event.<名字>`
    /// - 插件: `plugin.<插件名>.<名字>` (框架校验, 不补前缀)
    std::string name;
    EventKind   kind = EventKind::Stream;
    /// 来源: `tool` / `core:turn` / `subagent` / `timer` / `plugin:<插件名>`
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
    /// 除创建者外, 还允许谁发布值 (默认 [PublishMask::None] = 只有创建者)
    PublishMask publishMask = PublishMask::None;
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
    /// 除创建者外还允许谁发布 (创建时声明; 诊断与 `list_events` 用)
    PublishMask publishMask = PublishMask::None;
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
    /// 值过滤: 顶层字段相等匹配 (`{"asyncId":"async-7"}`), 空对象 = 不过滤
    /// - 不匹配的值**不消耗**等待 (`first` 不会被别人的值吃掉)
    /// - 带过滤的等待只看值: 事件结束不会收尾它们 (多任务共用一个事件名时,
    ///   别人的任务结束不能算我的任务结束); 超时/取消照常生效
    Json match = Json::object();
    /// 超时是否投递一条提示
    bool notifyOnTimeout = true;
    /// 暂停态 (上一轮失败/中断) 是否解除暂停
    bool unpausePaused = false;
    /// 会话在跑一轮时直接丢弃这次投递 (插件触发器的 DROP_IF_BUSY 语义)
    bool dropIfBusy = false;
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

/// 发布者类别: `plugin:<名>` 开头的归属是插件, `tool` 是模型侧工具调用, 其余算内置源
PublishRole publishRoleOf(std::string_view publisherOwner);

/// 类别名 (`model` / `plugin` / `builtin`)
std::string_view publishRoleName(PublishRole role);

/// 发布权限展示 (`owner` / `owner+model` / `any`)
std::string publishMaskText(PublishMask mask);

/// 发布者能否往这条事件发布值 (创建者自己总能发, 其他人看 `publishMask`)
bool mayPublish(const EventRecord& rec, std::string_view publisherOwner);

/// 发布者能否通过"第一次发布"顺手把这条事件建出来
/// - 模型: 只能 `agentxx.agent.event.*` 且不占内建保留子段
/// - 插件: 只能 `plugin.*` (框架不补前缀)
/// - 内置源: `agentxx.agent.event.*` 与 `agentxx.agent.async.*`
bool mayCreateByPublish(std::string_view name, std::string_view publisherOwner);

/// 一次发布的统计结果 (工具用它给出有意义的结果文本)
struct PublishOutcome {
    /// 本次发布顺手建了记录
    bool created = false;
    /// 单值事件在本次发布后自动结束
    bool endedNow = false;
    /// 该事件累计发布条数 (含本次)
    uint64_t published = 0;
    /// 本次值实际投递给了几个等待
    size_t delivered = 0;
    /// 发布后还挂着的等待数
    size_t remainingWaits = 0;
    /// 逐条投递 (`{"waitId":"w-3","delivery":"next-step"}`)
    Json deliveries = Json::array();
};

/// 一次收尾的统计结果
struct EndOutcome {
    /// 事件早已结束 (重复收尾是空操作)
    bool alreadyEnded = false;
    /// 收尾前累计发布的条数
    uint64_t published = 0;
    /// 因这次收尾而结束的等待数 (它们没拿到值)
    size_t closedWaits = 0;
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
/// - 事件名就是总线主题 (命名规则见 `agentxx/event/naming.h`):
///   内置源用 `agentxx.agent.event.*` 直接调 [publishValue] (不经总线),
///   插件往总线发 `plugin.<插件名>.*` 字符串即等于往中枢发值 (`attachBus` 前缀订阅)
/// - 等待满足后按投递决策表把值渲染成一条输入, 经 `SessionInputSink` 交回会话
/// - 生命周期事件发布回总线 (`agentxx.agent.hub.value` / `agentxx.agent.hub.ended`),
///   不在中枢收值的段里, 因此不会被自己的前缀订阅再吃回去
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
        /// 投递内容 (`[Content]` 段) 的压缩阈值, 单位为 utf8 字符数 (0 = 不压缩)
        /// - 与工具输出压缩同一套判定与提示, 见 [agentxx::util::offloadShareStoreIfOversized]
        /// - 主程序按 `AgentConfig::eventContentLimitLength()` 填充
        size_t contentLimitLength    = 2 * 1024;
        size_t valueHardBytes        = 1024 * 1024;
        size_t deliveryBacklogPerSession = 64;
        bool   allowHeadlessTurn     = false;
        /// 等待未指定 timeout 时使用 (0 = 不限)
        std::chrono::milliseconds waitTimeoutDefault{0};
        /// 等待未指定 delivery 时使用
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

    /// 挂到 agent 总线上 (订阅 `plugin.` 前缀: 插件发到总线的值即中枢的事件)
    void attachBus(const std::shared_ptr<agentxx::events::EventBus>& bus);

    /// 退订总线前缀 (关闭时调用)
    void detachBus();

    /// 值列表 → `[Content]` 段文本 (超长时整段转会话 share store)
    /// - 投递文本与工具返回值共用这一处, 保证两边展示一致
    /// - 值本身是**别人的 JSON**: 对象/数组原样 `dump()` 输出, 不改字段/不改转义;
    ///   只有后台任务结果事件 (见 [eventTextPayloadField]) 的正文字段按原文单独给出
    std::string contentText(
        std::string_view         sessionId,
        std::string_view         eventName,
        const std::vector<Json>& values
    );

    // -------------------------------------------------------------------
    // 定义 / 发布 (hub 线程)
    // -------------------------------------------------------------------

    bool define(const EventDefineSpec& spec, std::string* err = nullptr);
    /// 发布一个值 (`owner` 省略时按名字推断: 插件族 → `plugin:<插件名>`, 其余 → 原样保留)
    bool publishValue(std::string_view name, Json value, std::string* err = nullptr);
    /// 发布一个值并显式给出归属 (内置事件源用: `core:turn` / `tool` / `subagent` / `timer`)
    bool publishValue(
        std::string_view name,
        Json             value,
        std::string_view owner,
        std::string*     err
    );

    /// 带发布者归属与统计结果的版本 (发布权限在这里判定)
    /// - `owner` 为空时按事件名推断 (插件族 → `plugin:<名>`)
    /// - 拒绝条件: 名字非法 / 值超硬上限 / 不是创建者且定义未放开对应发布者 /
    ///   事件不存在且当前发布者不允许建它
    std::expected<PublishOutcome, std::string>
                 publishValueEx(std::string_view name, Json value, std::string_view owner);

    bool endEvent(std::string_view name, Json summary = Json::object(), std::string* err = nullptr);

    /// 带发布者归属与统计的收尾 (收尾别人的流同样要权限)
    std::expected<EndOutcome, std::string>
         endEventEx(std::string_view name, Json summary, std::string_view owner);
    bool endEvent(
        std::string_view name,
        Json             summary,
        std::string_view owner,
        std::string*     err
    );
    bool removeEvent(std::string_view name);
    /// 带发布者归属的移除
    bool removeEvent(std::string_view name, std::string_view owner, std::string* err);

    // -------------------------------------------------------------------
    // 任意线程 (插件 / 外部)
    // -------------------------------------------------------------------

    void postValue(std::string_view name, Json value);
    /// 带归属的版本 (异步子代理等后台发布方用; 归属影响诊断与插件卸载回收)
    void postValue(std::string_view name, Json value, std::string_view owner);
    void postEnd(std::string_view name, Json summary = Json::object());
    /// 带归属的收尾 (后台发布方用; 收尾别人的流会被权限拒绝)
    void postEnd(std::string_view name, Json summary, std::string_view owner);

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
    /// - 供 `agentxx.agent.event.turn.*` / `agentxx.agent.event.tool.end.*` 这类内建源
    ///   判断"值得不值得发": 没人等就不建记录, 否则每轮/每次工具调用都会留一条空记录
    /// - 总线桥也只收"有人关心"的主题: 插件发到 `plugin.*` 的普通消息不当成值事件
    bool wants(std::string_view name) const;

    /// 当前是否在 hub 线程 (构造时记录的那个线程)
    /// - 插件面的表调用据此决定"直接执行"还是"投递到 hub 线程再等"
    bool isHubThread() const noexcept;

    /// 在中枢线程执行一次调用并取回结果 (**任意线程**)
    /// - 已在中枢线程: 直接执行 (中枢不加锁, 只有它的线程能读写状态);
    /// - 其它线程: 投递并等待结果, 最多等 10 秒 —— 中枢线程正忙在别的长任务上时
    ///   宁可返回默认值并记一条警告, 也不让调用方永久阻塞
    /// - 中枢对象在等待期间被释放时不执行, 同样返回默认值
    /// - `completed` 非空时写入"这次调用真的执行了": false = 超时或中枢已释放,
    ///   调用方据此把它当成失败, 不要把默认值当成功果
    /// - 定义放在类外 (那里中枢类型已完整, 闭包的调用结果才能求值)
    template<typename Fn>
    auto callSync(Fn&& fn, bool* completed = nullptr);

    /// 事件记录数 / 等待数 (测试与诊断)
    size_t eventCount() const noexcept {
        return records_.size();
    }
    size_t waitCount() const noexcept {
        return waits_.size();
    }

    // -------------------------------------------------------------------
    // 归属回收 (插件禁用/卸载时按 owner 整批清)
    // -------------------------------------------------------------------

    /// 删除该 owner 名下的全部事件记录 (还挂着等待的按"已结束且无值"收尾)
    /// `return` 删除条数
    size_t removeEventsOfOwner(std::string_view owner);

    /// 取消该 owner 名下还在等的等待
    /// `return` 取消条数
    size_t cancelWaitsOfOwner(std::string_view owner, std::string_view reason);

    /// 该 owner 名下的事件记录数 / 等待数 (须在中枢线程调用)
    size_t eventCountOfOwner(std::string_view owner) const;
    size_t waitCountOfOwner(std::string_view owner) const;

    /// 按归属整批回收 (插件禁用/卸载): 先取消等待, 再删事件记录
    /// - **任意线程** (内部经 [callSync] 投到中枢线程)
    /// `return` 处理的条目数 (等待 + 记录)
    size_t removeOwned(std::string_view owner, std::string_view reason = "owner-removed");

    /// 该归属名下的事件记录数 / 等待数 (任意线程)
    /// `return` {事件记录数, 等待数}
    std::pair<size_t, size_t> ownedCounts(std::string_view owner);

    // -------------------------------------------------------------------
    // 生命周期
    // -------------------------------------------------------------------

    /// 关闭中枢: 取消全部等待与定时器, 之后所有投递一律丢弃
    void closeAll(std::string_view reason = "closed");

    bool closed() const noexcept {
        return closed_;
    }

    /// 阻塞等待一次 (供 `agentxx_async_await`)
    ///
    /// 内部登记一个 `delivery=none` 的临时等待, 结束后一定撤销: 值到达 / 超时 /
    /// 取消令牌被置位三条路径都覆盖; 取消时抛 `CanceledException`。
    asio::awaitable<EventWait> awaitOnce(
        EventWaitSpec                         spec,
        std::shared_ptr<neograph::graph::CancelToken> ct
    );

    // -------------------------------------------------------------------
    // 定时源 (`agentxx.agent.event.timer.*`)
    // -------------------------------------------------------------------

    /// 建立/重设一个一次性或周期定时源; 到点向 `agentxx.agent.event.timer.<名字>` 发布一个值
    /// - `args`:
    ///     - [name]       完整事件名 (`agentxx.agent.event.timer.<名字>`) 或只是 `<名字>`
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
        std::string                         eventName; ///< `agentxx.agent.event.timer.<name>`
        int64_t                             intervalMs = 0;
        int64_t                             nextFireMs = 0;
        uint64_t                            fired      = 0;
    };

    asio::any_io_executor                       executor_;
    /// hub 线程 (构造时所在线程); 跨线程调用方据此判断能否直接执行
    std::thread::id                             hubThreadId_;
    std::weak_ptr<agentxx::agent::AgentContext> ctx_;
    Config                                      cfg_;
    TurnRunner                                  turnRunner_;

    std::map<std::string, RecordEntry, std::less<>> records_;
    std::map<std::string, WaitEntry, std::less<>>   waits_;
    std::map<std::string, TimerSource, std::less<>> timers_;

    std::shared_ptr<agentxx::events::EventBus> bus_;
    /// 总线 `plugin.` 前缀订阅 id (插件发到总线的值事件)
    size_t valueSubId_ = 0;

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
    /// `return` 本次值实际投递到的等待 (`[{"waitId":..,"delivery":..}]`)
    Json dispatchValue(RecordEntry& rec, const Json& value);
    /// 事件结束 (含 single 自动结束) 后的等待处理
    void dispatchEnded(RecordEntry& rec);

    /// 记录归属: 显式给出优先, 否则按主题推断 (`plugin.<名>.x` → `plugin:<名>`)
    static std::string ownerOfName(std::string_view name, std::string_view owner);

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
// 跨线程调用 (模板定义放类外: 中枢类型在这里已完整)
// ---------------------------------------------------------------------------

template<typename Fn>
auto EventHub::callSync(Fn&& fn, bool* completed) {
    using Result = decltype(fn(std::declval<EventHub&>()));
    // 等超时/中枢已释放时返回默认值, 因此结果类型必须能默认构造
    static_assert(
        std::is_void_v<Result> || std::is_default_constructible_v<Result>,
        "callSync 的结果类型必须可默认构造 (超时时返回 Result{})"
    );
    if (completed != nullptr) {
        *completed = true;
    }
    if (isHubThread()) {
        return fn(*this);
    }
    auto weak = weak_from_this();
    if constexpr (std::is_void_v<Result>) {
        auto promise = std::make_shared<std::promise<void>>();
        auto future  = promise->get_future();
        asio::post(
            executor_,
            [weak, promise, fn = std::forward<Fn>(fn)]() mutable {
                auto self = weak.lock();
                if (!self) {
                    promise->set_value();
                    return;
                }
                try {
                    fn(*self);
                    promise->set_value();
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            }
        );
        if (future.wait_for(std::chrono::seconds{10}) != std::future_status::ready) {
            XX_LOGW("[event_hub] hub call timed out (waiting for the event hub thread)");
            if (completed != nullptr) {
                *completed = false;
            }
            return;
        }
        future.get();
        return;
    } else {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future  = promise->get_future();
        asio::post(
            executor_,
            [weak, promise, fn = std::forward<Fn>(fn)]() mutable {
                auto self = weak.lock();
                if (!self) {
                    promise->set_value(Result{});
                    return;
                }
                try {
                    promise->set_value(fn(*self));
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            }
        );
        if (future.wait_for(std::chrono::seconds{10}) != std::future_status::ready) {
            XX_LOGW("[event_hub] hub call timed out (waiting for the event hub thread)");
            if (completed != nullptr) {
                *completed = false;
            }
            return Result{};
        }
        return future.get();
    }
}

// ---------------------------------------------------------------------------
// 文本渲染 (纯函数: 便于测试逐字段断言, 也是给模型看的稳定格式)
// ---------------------------------------------------------------------------

/// 值列表 → 纯文本 (给模型看的 `[Content]` 段)
///
/// 值可能是别人自定义的 JSON (插件/模型发布的东西), 所以**不做任何字段解释**:
/// - 单个值: 字符串给原文; 对象/数组给紧凑 JSON (`dump()`, 键序/转义/数字格式与发布时一致)
/// - 多个值 (`mode=all` 等): 整体给 JSON 数组
/// - 只有 [textPayloadField] 非空时, 才把值里那个顶层字符串字段按原文单独给出
///   (那是"产物正文" —— 工具输出 / 子代理回答, 本身可能是 JSON 文本, 原文即原文)
///
/// - `args`:
///     - [values]            事件值列表 (为空时返回空串, 由 [renderEventText] 补占位)
///     - [textPayloadField]  值里"产物正文"的字段名 (空 = 不解释字段, 整个值按 JSON/原文给出);
///                           仅 [eventTextPayloadField] 认出的框架事件会传
///
/// - `return` 内容段文本 (末尾不带换行)
std::string renderEventValuesText(
    const std::vector<Json>& values,
    std::string_view         textPayloadField = {}
);

/// 值里"产物正文"的字段名 (空 = 该事件的值就是别人的 JSON, 不做解释)
///
/// 只有框架自己的后台任务结果事件 (`agentxx.agent.async.*`) 把多行正文塞在 JSON 字符串里
/// (`content` = 工具输出 / 子代理回答): 这些字段按原文给出, 不经 JSON 转义。
/// 其余事件 (包括插件事件与模型 `agentxx_async_emit` 发的自定义 JSON) 的值一律原样输出,
/// 以免把别人自定义的 JSON 改成另一种文本格式。
std::string_view eventTextPayloadField(std::string_view eventName);

/// 一次投递的文本, 也是模型收到的输入正文 —— 三段固定结构:
///
/// ```text
/// [Event] <事件名>
/// [Info]
/// mode: first
/// wait: w-3
/// status: value
/// [Content]
/// <内容>
/// ```
///
/// - 首行只放事件名 (`mode` 等元信息全部进 `[Info]`), 模型据此识别"这是事件而不是
///   用户说话", 折叠预览与日志也能一眼认出是哪个事件
/// - `[Info]` 中 `mode` / `wait` / `status` 固定输出, `ref` / `label` / `note` 有值才写
/// - `[Content]` 是 [renderEventValuesText] 的结果; 超长时已转会话 share store,
///   文本里是取回提示与开头片段; 内容为空时写 `(no value)`
/// - `status` 取值: `value` / `values` / `stream-ended` / `stream-open` /
///   `ended-empty` / `timeout`
std::string renderEventText(
    const EventWait& wait,
    std::string_view status,
    std::string_view contentText,
    std::string_view note = {}
);

/// `await` 超时文本
std::string renderAwaitTimeout(const EventWaitSpec& spec, int64_t waitedMs);

} // namespace events
} // namespace agentxx
