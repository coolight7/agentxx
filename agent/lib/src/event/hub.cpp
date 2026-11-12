#include "agentxx/event/hub.h"

#include "agentxx/agent/context.h"
#include "agentxx/agent/session_input_sink.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/event/naming.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/observability.h"
#include "agentxx/util/share_store_offload.h"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/post.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "neograph/graph/cancel.h"
#include "utilxx_base/asio_error.h"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace agentxx {
namespace events {

namespace {

/// 系统时间毫秒 (创建/更新时间的展示用)
int64_t systemNowMs() {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch()
    )
                                    .count());
}

/// 值是否满足等待的值过滤 (顶层字段相等匹配; 空过滤 = 都满足)
bool waitMatchesSpec(const EventWaitSpec& spec, const Json& value) {
    if (spec.match.empty()) {
        return true;
    }
    if (!value.is_object()) {
        return false;
    }
    for (const auto& [key, expected] : spec.match.items()) {
        if (!value.contains(key) || value[key] != expected) {
            return false;
        }
    }
    return true;
}

/// 值的字节数 (紧凑 JSON 文本长度; 未构造文本时按 0)
size_t valueBytesOf(const Json& value) {
    if (value.is_null()) {
        return 0;
    }
    return value.dump().size();
}

/// 终态等待在表里保留多久 (供事后查询)
constexpr int64_t kTerminalWaitLingerMs = 60 * 1000;
/// 超时/回收检查周期
constexpr int64_t kSweepPeriodMs = 500;

/// `await` 轮询周期: 值到达没有回调唤醒时的兜底检查 (同时检查取消令牌)
constexpr int64_t kAwaitPollMs = 25;

/// 定时源事件名前缀 (`agentxx.agent.event.timer.`)
constexpr std::string_view kTimerEventPrefix = "agentxx.agent.event.timer.";

/// 定时源事件名: `agentxx.agent.event.timer.<名字>`
/// - 两种写法都接受: 完整事件名, 或只是 `<名字>` (以及旧习惯的 `timer.<名字>`)
std::string timerEventName(std::string_view name) {
    if (name.starts_with(naming::kAgentEventPrefix)) {
        return std::string{name};
    }
    constexpr std::string_view kTimerSegment{"timer."};
    auto                       shortName = name;
    if (shortName.starts_with(kTimerSegment)) {
        shortName = shortName.substr(kTimerSegment.size());
    }
    return std::string{kTimerEventPrefix} + std::string{shortName};
}

/// 定时源短名 (`agentxx.agent.event.timer.` 之后的那个名字)
std::string timerShortName(std::string_view eventName) {
    if (eventName.starts_with(kTimerEventPrefix)) {
        return std::string{eventName.substr(kTimerEventPrefix.size())};
    }
    return std::string{eventName};
}

/// 是否是后台任务结果事件 (`agentxx.agent.async.*`)
/// - 这类值由框架自己产出 (工具输出 / 子代理回答), 投递时按"已完成的事"处理:
///   会话空闲时即使停在暂停态 (上一轮失败/取消留下的) 也要把它放进上下文
bool isAsyncResultEvent(std::string_view eventName) {
    return eventName.starts_with(naming::kAgentAsyncPrefix);
}

/// 这批值是否全是后台**工具**任务的结果 (`kind == "tool"`)
/// - 工具结果的裁剪由工具执行层负责 (见 [agentxx::util::offloadToolResultIfOversized]),
///   事件侧不再整段卸载, 否则同一段输出会被套上第二层取回提示
/// - 空值列表 / 混合来源 / 字段缺失都按"不是"处理 (照常按长度卸载)
bool asyncValuesAreToolKind(const std::vector<Json>& values) {
    if (values.empty()) {
        return false;
    }
    for (const auto& value : values) {
        if (!value.is_object() || !value.contains("kind") || !value["kind"].is_string()
            || value["kind"].get_string_view() != "tool") {
            return false;
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 构造 / 生命周期
// ---------------------------------------------------------------------------

EventHub::EventHub(asio::any_io_executor ex, std::weak_ptr<agentxx::agent::AgentContext> ctx, Config cfg) :
    executor_(std::move(ex)),
    hubThreadId_(std::this_thread::get_id()),
    ctx_(std::move(ctx)),
    cfg_(std::move(cfg)) {}

bool EventHub::isHubThread() const noexcept {
    return std::this_thread::get_id() == hubThreadId_;
}

EventHub::~EventHub() {
    // 定时器句柄随成员释放; 已在等待的 async_wait 处理器只持有 weak_ptr, 不会回调本对象
    for (auto& [_, src] : timers_) {
        if (src.timer) {
            src.timer->cancel();
        }
    }
    if (sweepTimer_) {
        sweepTimer_->cancel();
    }
}

void EventHub::setTurnRunner(TurnRunner runner) {
    turnRunner_ = std::move(runner);
}

void EventHub::attachBus(const std::shared_ptr<agentxx::events::EventBus>& bus) {
    if (!bus) {
        return;
    }
    // 总线只用于中枢自己发强类型主题 (`Topic::HubValue` / `HubEnded`);
    // 插件发来的值现在走广播, 由 [publishPluginValue] 转入
    bus_ = bus;
}

void EventHub::attachBroadcasts(
    const std::shared_ptr<pluginxx::BroadcastRegistry>& broadcasts
) {
    broadcasts_ = broadcasts;
}

bool EventHub::publishPluginValue(
    std::string_view name,
    std::string_view valueJson,
    std::string_view owner
) {
    if (closed_ || name.empty()) {
        return false;
    }
    // 没人等、也没声明过的主题不当成值事件: 插件之间的普通消息
    // (面板刷新/数据通道) 不应在事件表里各占一条记录
    if (!wants(name)) {
        return false;
    }
    Json       value     = Json(std::string{valueJson});
    const bool looksJson = !valueJson.empty()
                           && (valueJson.front() == '{' || valueJson.front() == '['
                               || valueJson.front() == '"');
    if (looksJson) {
        auto parsed = agentxx::util::catchError<std::optional<Json>>(
            [&]() -> std::optional<Json> {
                return Json::parse(valueJson);
            },
            [&](std::string errmsg) -> std::optional<Json> {
                XX_LOGD("[event_hub] broadcast payload for `{}` is not JSON: {}", name, errmsg);
                return std::nullopt;
            }
        );
        if (parsed.has_value()) {
            value = std::move(*parsed);
        }
    }
    std::string err;
    // 广播没有发送者身份: 插件族的主题按名字推断归属 (`plugin.<名>.x` → `plugin:<名>`)
    if (!publishValue(name, std::move(value), ownerOfName(name, owner), &err)) {
        XX_LOGW("[event_hub] publish from broadcast `{}` rejected: {}", name, err);
        return false;
    }
    return true;
}

void EventHub::detachBus() {
    bus_.reset();
}

void EventHub::closeAll(std::string_view reason) {
    if (closed_) {
        return;
    }
    closed_ = true;
    cancelAllWaits(reason.empty() ? "closed" : reason);
    cancelAllTimers();
    if (sweepTimer_) {
        sweepTimer_->cancel();
    }
    detachBus();
}

void EventHub::cancelAllWaits(std::string_view reason) {
    for (auto& [_, entry] : waits_) {
        if (entry.wait.state == WaitState::Armed) {
            finishWait(entry, WaitState::Canceled, reason, true);
        }
    }
}

// ---------------------------------------------------------------------------
// 记录表
// ---------------------------------------------------------------------------

EventHub::RecordEntry* EventHub::findRecord(std::string_view name) {
    auto it = records_.find(name);
    return it == records_.end() ? nullptr : &it->second;
}

const EventHub::RecordEntry* EventHub::findRecord(std::string_view name) const {
    auto it = records_.find(name);
    return it == records_.end() ? nullptr : &it->second;
}

EventHub::RecordEntry*
    EventHub::ensureRecord(std::string_view name, std::string_view owner, std::string_view sessionId) {
    if (auto* found = findRecord(name)) {
        return found;
    }
    if (records_.size() >= cfg_.maxEvents) {
        // 先回收已结束的记录, 腾不出位置就明确拒绝 (由调用方给出错误文本)
        const auto now = nowSteadyMs();
        for (auto it = records_.begin(); it != records_.end();) {
            const bool terminal
                = it->second.rec.state != EventState::Open
                  && it->second.lingerDeadlineMs > 0 && now >= it->second.lingerDeadlineMs;
            it = terminal ? records_.erase(it) : std::next(it);
        }
        if (records_.size() >= cfg_.maxEvents) {
            return nullptr;
        }
    }
    RecordEntry entry;
    entry.rec.name          = std::string{name};
    entry.rec.kind          = EventKind::Stream;
    entry.rec.state         = EventState::Open;
    entry.rec.owner         = std::string{owner};
    entry.rec.originSession = std::string{sessionId};
    entry.rec.createdAtMs   = systemNowMs();
    entry.rec.updatedAtMs   = entry.rec.createdAtMs;
    auto& stored            = records_[std::string{name}];
    stored                  = std::move(entry);
    return &stored;
}

std::string EventHub::ownerOfName(std::string_view name, std::string_view owner) {
    if (!owner.empty()) {
        return std::string{owner};
    }
    // 插件族按主题归因: 这样插件禁用/卸载时它的记录能整批回收
    if (const auto plugin = naming::pluginNameOfTopic(name); !plugin.empty()) {
        return "plugin:" + std::string{plugin};
    }
    return std::string{};
}

PublishRole publishRoleOf(std::string_view publisherOwner) {
    // 插件的归属记作 `plugin:<插件名>` (中枢里所有插件记录都用这个前缀)
    if (publisherOwner.starts_with("plugin:") || publisherOwner.starts_with(naming::kPluginRoot)) {
        return PublishRole::Plugin;
    }
    if (publisherOwner == "tool") {
        return PublishRole::Model;
    }
    return PublishRole::Builtin;
}

std::string_view publishRoleName(PublishRole role) {
    switch (role) {
        case PublishRole::Model:
            return "model";
        case PublishRole::Plugin:
            return "plugin";
        case PublishRole::Builtin:
            return "builtin";
    }
    return "unknown";
}

namespace {

/// 框架自己的事件段 (`agentxx.agent.event.*` / `agentxx.agent.async.*`)
/// - 段内的记录由多个内置源共同使用 (轮次 / 工具结束 / 定时器 / 后台任务),
///   不能因为某一条记录先被别人建了就拦住内置源
bool isFrameworkEventName(std::string_view name) {
    return name.starts_with(naming::kAgentEventPrefix)
           || name.starts_with(naming::kAgentAsyncPrefix);
}

/// 掩码里是否开了某一类发布者
bool hasPublishBit(PublishMask mask, PublishRole role) {
    const auto bit = role == PublishRole::Model    ? PublishMask::Model
                     : role == PublishRole::Plugin ? PublishMask::Plugin
                                                   : PublishMask::Builtin;
    return (static_cast<uint32_t>(mask) & static_cast<uint32_t>(bit)) != 0;
}

} // namespace

std::string publishMaskText(PublishMask mask) {
    const auto bits = static_cast<uint32_t>(mask);
    if (bits == 0) {
        return "owner";
    }
    if (bits == static_cast<uint32_t>(PublishMask::Any)) {
        return "any";
    }
    std::string out = "owner";
    for (const auto role : {PublishRole::Model, PublishRole::Plugin, PublishRole::Builtin}) {
        if (hasPublishBit(mask, role)) {
            out += "+";
            out += publishRoleName(role);
        }
    }
    return out;
}

bool mayPublish(const EventRecord& rec, std::string_view publisherOwner) {
    if (publisherOwner == rec.owner) {
        return true;
    }
    const auto role = publishRoleOf(publisherOwner);
    // 内置源在框架自己的事件段里总是可以发布 (同一段被轮次/工具/定时器/后台任务
    // 共同使用, 也不应因为记录先被别的代码建了就拦下来)
    if (role == PublishRole::Builtin && isFrameworkEventName(rec.name)) {
        return true;
    }
    return hasPublishBit(rec.publishMask, role);
}

bool mayCreateByPublish(std::string_view name, std::string_view publisherOwner) {
    switch (publishRoleOf(publisherOwner)) {
        case PublishRole::Model:
            // 模型只能在自建事件段里建, 且不能占内建保留子段
            return name.starts_with(naming::kAgentEventPrefix)
                   && !naming::isReservedEventSegment(name);
        case PublishRole::Plugin:
            return naming::isPluginTopic(name);
        case PublishRole::Builtin:
            return name.starts_with(naming::kAgentEventPrefix)
                   || name.starts_with(naming::kAgentAsyncPrefix);
    }
    return false;
}

bool EventHub::define(const EventDefineSpec& spec, std::string* err) {
    auto fail = [&](std::string msg) {
        if (err) {
            *err = std::move(msg);
        }
        return false;
    };
    if (closed_) {
        return fail("event hub is closed");
    }
    std::string nameErr;
    if (!naming::validateEventName(spec.name, &nameErr)) {
        return fail(nameErr);
    }
    auto* existing = findRecord(spec.name);
    if (existing) {
        if (existing->rec.kind != spec.kind) {
            XX_LOGW(
                "[event_hub] event `{}` already defined as {} by `{}`, rejecting redefinition as {} "
                "by `{}`",
                spec.name,
                existing->rec.kind == EventKind::Single ? "single" : "stream",
                existing->rec.owner,
                spec.kind == EventKind::Single ? "single" : "stream",
                spec.owner
            );
            return fail("event already defined with a different kind");
        }
        // 同为流式: 只补充说明/历史条数等元信息, 不打断已有等待
        if (existing->rec.description.empty()) {
            existing->rec.description = spec.description;
        }
        if (!spec.owner.empty()) {
            existing->rec.owner = spec.owner;
        }
        // 权限只升不降: 后来者可以放宽, 但不收窄 (收窄会打断已在用的发布方)
        existing->rec.publishMask = static_cast<PublishMask>(
            static_cast<uint32_t>(existing->rec.publishMask)
            | static_cast<uint32_t>(spec.publishMask)
        );
        return true;
    }
    auto* created = ensureRecord(spec.name, ownerOfName(spec.name, spec.owner), spec.originSession);
    if (nullptr == created) {
        return fail(fmt::format(
            "too many events (limit {})",
            cfg_.maxEvents
        ));
    }
    created->rec.kind         = spec.kind;
    created->rec.description  = spec.description;
    created->rec.historyLimit = spec.historyLimit;
    created->rec.publishMask  = spec.publishMask;
    if (spec.kind == EventKind::Single || spec.historyLimit == 0) {
        created->rec.historyLimit = 1;
    }
    if (spec.ttl.count() > 0) {
        created->ttlDeadlineMs = nowSteadyMs() + spec.ttl.count();
    }
    created->rec.lingerMs = spec.linger.count();
    return true;
}

bool EventHub::publishValue(std::string_view name, Json value, std::string* err) {
    return publishValue(name, std::move(value), std::string_view{}, err);
}

bool EventHub::publishValue(
    std::string_view name,
    Json             value,
    std::string_view owner,
    std::string*     err
) {
    auto outcome = publishValueEx(name, std::move(value), owner);
    if (outcome.has_value()) {
        return true;
    }
    if (err) {
        *err = outcome.error();
    }
    return false;
}

std::expected<PublishOutcome, std::string>
    EventHub::publishValueEx(std::string_view name, Json value, std::string_view owner) {
    if (closed_) {
        return std::unexpected{std::string{"event hub is closed"}};
    }
    std::string nameErr;
    if (!naming::validateEventName(name, &nameErr)) {
        return std::unexpected{nameErr};
    }
    const auto bytes = valueBytesOf(value);
    if (cfg_.valueHardBytes > 0 && bytes > cfg_.valueHardBytes) {
        XX_LOGW(
            "[event_hub] value for `{}` rejected: {} bytes > hard limit {}",
            name,
            bytes,
            cfg_.valueHardBytes
        );
        return std::unexpected{
            fmt::format("value too large ({} bytes > {} bytes)", bytes, cfg_.valueHardBytes)
        };
    }
    // 发布者身份: 显式给了就按它判定 (模型工具 / 插件 / 内置源);
    // 没给说明是中枢内部代码直接调用, 视为记录自己的归属 (与旧行为一致, 不做拦截)
    auto*      existing  = findRecord(name);
    const auto publisher = (owner.empty() && nullptr != existing) ? existing->rec.owner
                                                                  : ownerOfName(name, owner);
    const auto role      = publishRoleOf(publisher);
    if (existing) {
        if (!mayPublish(existing->rec, publisher)) {
            return std::unexpected{fmt::format(
                "`{}` belongs to `{}` and only allows publishing by {} (this call is {})",
                name,
                existing->rec.owner.empty() ? std::string_view{"-"} : existing->rec.owner,
                publishMaskText(existing->rec.publishMask),
                publishRoleName(role)
            )};
        }
    } else if (!mayCreateByPublish(name, publisher)) {
        // 名字没建过, 且当前发布者没资格建它: 把"找创建者"这条出路写进错误文本
        return std::unexpected{fmt::format(
            "event `{}` is not defined, and {} may not create it (ask whoever owns it to define "
            "the event, or use `agentxx.agent.event.<name>` for your own)",
            name,
            publishRoleName(role)
        )};
    }

    PublishOutcome outcome;
    outcome.created = (nullptr == existing);
    auto* rec        = ensureRecord(name, publisher, "");
    if (nullptr == rec) {
        return std::unexpected{fmt::format("too many events (limit {})", cfg_.maxEvents)};
    }
    if (rec->rec.kind == EventKind::Single && rec->rec.published > 0) {
        ++rec->rec.overwritten;
        XX_LOGW("[event_hub] single-value event `{}` got another value, overwriting", name);
        rec->rec.history.clear();
    }
    const size_t historyLimit = rec->rec.historyLimit == 0 ? 1 : rec->rec.historyLimit;
    if (rec->rec.history.size() >= historyLimit) {
        rec->rec.history.pop_front();
        ++rec->rec.droppedForHistory;
    }
    rec->rec.history.push_back(value);
    ++rec->rec.published;
    ++stats_.valueCount;
    rec->rec.updatedAtMs = systemNowMs();
    // 已被 end 的事件再收到值: 记录保留, 但重新打开 (流式事件允许"续上")
    if (rec->rec.state != EventState::Open) {
        rec->rec.state        = EventState::Open;
        rec->rec.endedAtMs    = 0;
        rec->lingerDeadlineMs = 0;
        XX_LOGD("[event_hub] event `{}` got a value after end, reopened", name);
    }
    const auto nameCopy = rec->rec.name;
    const auto count    = rec->rec.published;
    outcome.deliveries  = dispatchValue(*rec, value);
    outcome.delivered   = outcome.deliveries.size();
    outcome.published   = count;
    if (rec->rec.kind == EventKind::Single) {
        // 单值事件发完就收尾: 以记录自己的归属收尾, 不受本次发布者影响
        std::string endErr;
        if (endEvent(nameCopy, Json::object(), rec->rec.owner, &endErr)) {
            outcome.endedNow = true;
        }
    }
    const auto remaining = std::count_if(waits_.begin(), waits_.end(), [&](const auto& item) {
        return item.second.wait.state == WaitState::Armed && item.second.wait.spec.event == nameCopy;
    });
    outcome.remainingWaits = static_cast<size_t>(remaining);
    // 通知面: 同名广播频道 (值原样给出; 已结束的频道由 endEvent 处理)
    if (broadcasts_) {
        // 频道按需声明: 事件名是运行期才知道的, 未声明就补一条 (幂等)
        if (!broadcasts_->declares(nameCopy)) {
            pluginxx::BroadcastDecl decl;
            decl.id      = nameCopy;
            decl.cache   = pluginxx::BroadcastCache_e::Latest;
            decl.publish = pluginxx::BroadcastPublisher_e::Owner;
            decl.scope   = pluginxx::BroadcastScope_e::App;
            broadcasts_->declare(decl, nullptr);
        }
        std::string reason;
        if (!broadcasts_->publish(nameCopy, value.dump(), std::string{}, &reason)
            && !reason.empty()) {
            XX_LOGD("[event_hub] broadcast `{}` not published: {}", nameCopy, reason);
        }
    }
    if (bus_ && bus_->hasListeners<EventHubValue>(Topic::HubValue)) {
        EventHubValue ev{.name = nameCopy, .valueJson = value.dump(), .valueCount = count};
        auto          bus = bus_;
        asio::co_spawn(
            executor_,
            [bus, ev]() -> asio::awaitable<void> {
                co_await bus->publish(Topic::HubValue, ev);
            },
            asio::detached
        );
    }
    return outcome;
}

void EventHub::postValue(std::string_view name, Json value) {
    postValue(name, std::move(value), std::string_view{});
}

void EventHub::postValue(std::string_view name, Json value, std::string_view owner) {
    auto self     = weak_from_this();
    auto n        = std::string{name};
    auto ownerStr = std::string{owner};
    asio::post(
        executor_,
        [self, n = std::move(n), ownerStr = std::move(ownerStr), value = std::move(value)]() mutable {
            if (auto locked = self.lock()) {
                std::string err;
                if (!locked->publishValue(n, std::move(value), ownerStr, &err)) {
                    XX_LOGW("[event_hub] posted value for `{}` rejected: {}", n, err);
                }
            }
        }
    );
}

void EventHub::postEnd(std::string_view name, Json summary) {
    postEnd(name, std::move(summary), std::string_view{});
}

void EventHub::postEnd(std::string_view name, Json summary, std::string_view owner) {
    auto self     = weak_from_this();
    auto n        = std::string{name};
    auto ownerStr = std::string{owner};
    asio::post(
        executor_,
        [self, n = std::move(n), summary = std::move(summary), ownerStr = std::move(ownerStr)](
        ) mutable {
            if (auto locked = self.lock()) {
                std::string err;
                if (!locked->endEvent(n, std::move(summary), ownerStr, &err)) {
                    XX_LOGW("[event_hub] posted end for `{}` rejected: {}", n, err);
                }
            }
        }
    );
}

bool EventHub::endEvent(std::string_view name, Json summary, std::string* err) {
    return endEvent(name, std::move(summary), std::string_view{}, err);
}

bool EventHub::endEvent(
    std::string_view name,
    Json             summary,
    std::string_view owner,
    std::string*     err
) {
    auto outcome = endEventEx(name, std::move(summary), owner);
    if (outcome.has_value()) {
        return true;
    }
    if (err) {
        *err = outcome.error();
    }
    return false;
}

std::expected<EndOutcome, std::string>
    EventHub::endEventEx(std::string_view name, Json summary, std::string_view owner) {
    auto* rec = findRecord(name);
    if (nullptr == rec) {
        return std::unexpected{fmt::format("event not found: {}", name)};
    }
    const auto publisher = owner.empty()
                               ? rec->rec.owner
                               : ownerOfName(name, owner);
    if (!mayPublish(rec->rec, publisher)) {
        return std::unexpected{fmt::format(
            "`{}` belongs to `{}` and only allows publishing by {} (this call is {})",
            name,
            rec->rec.owner.empty() ? std::string_view{"-"} : rec->rec.owner,
            publishMaskText(rec->rec.publishMask),
            publishRoleName(publishRoleOf(publisher))
        )};
    }
    EndOutcome outcome;
    outcome.published = rec->rec.published;
    if (rec->rec.state != EventState::Open) {
        // 幂等: 第二次 end 是 no-op
        outcome.alreadyEnded = true;
        return outcome;
    }
    // 这次收尾会让多少等待结束: 与 dispatchEnded 同一过滤条件 (带值过滤的等待不收)
    outcome.closedWaits = static_cast<size_t>(std::count_if(
        waits_.begin(),
        waits_.end(),
        [&](const auto& item) {
            return item.second.wait.state == WaitState::Armed
                   && item.second.wait.spec.event == name && item.second.wait.spec.match.empty();
        }
    ));
    rec->rec.state        = EventState::Ended;
    rec->rec.endedAtMs    = systemNowMs();
    rec->rec.updatedAtMs  = rec->rec.endedAtMs;
    rec->rec.summary      = summary.is_null() ? Json::object() : std::move(summary);
    rec->lingerDeadlineMs = nowSteadyMs() + std::min<int64_t>(rec->rec.lingerMs, 24 * 60 * 60 * 1000);
    const auto nameCopy    = rec->rec.name;
    const auto count       = rec->rec.published;
    const auto summaryText = rec->rec.summary.dump();
    dispatchEnded(*rec);
    // 广播面: 同一个名字的频道也结束 (订阅方收到 on_end; 再次发布会被拒)
    if (broadcasts_ && broadcasts_->declares(nameCopy)) {
        std::string reason;
        if (!broadcasts_->end(nameCopy, summaryText, std::string{}, &reason) && !reason.empty()) {
            XX_LOGD("[event_hub] broadcast `{}` not ended: {}", nameCopy, reason);
        }
    }
    if (bus_ && bus_->hasListeners<EventHubEnded>(Topic::HubEnded)) {
        EventHubEnded ev{.name = nameCopy, .summaryJson = summaryText, .valueCount = count};
        auto          bus = bus_;
        asio::co_spawn(
            executor_,
            [bus, ev]() -> asio::awaitable<void> {
                co_await bus->publish(Topic::HubEnded, ev);
            },
            asio::detached
        );
    }
    return outcome;
}

bool EventHub::removeEvent(std::string_view name) {
    return removeEvent(name, std::string_view{}, nullptr);
}

bool EventHub::removeEvent(std::string_view name, std::string_view owner, std::string* err) {
    auto it = records_.find(name);
    if (it == records_.end()) {
        if (err) {
            *err = fmt::format("event not found: {}", name);
        }
        return false;
    }
    const auto publisher = owner.empty() ? it->second.rec.owner : ownerOfName(name, owner);
    if (!mayPublish(it->second.rec, publisher)) {
        if (err) {
            *err = fmt::format(
                "`{}` belongs to `{}` and only allows publishing by {} (this call is {})",
                name,
                it->second.rec.owner.empty() ? std::string_view{"-"} : it->second.rec.owner,
                publishMaskText(it->second.rec.publishMask),
                publishRoleName(publishRoleOf(publisher))
            );
        }
        return false;
    }
    // 事件被移除: 还挂在上面的等待按"事件已结束且无值"收尾, 否则会永远等下去
    for (auto& [_, entry] : waits_) {
        if (entry.wait.state == WaitState::Armed && entry.wait.spec.event == name) {
            flushWait(entry, "ended-empty");
            finishWait(entry, WaitState::Satisfied, "event-removed", true);
        }
    }
    records_.erase(it);
    return true;
}

bool EventHub::wants(std::string_view name) const {
    if (findRecord(name)) {
        return true;
    }
    return std::any_of(waits_.begin(), waits_.end(), [&](const auto& item) {
        return item.second.wait.state == WaitState::Armed && item.second.wait.spec.event == name;
    });
}

size_t EventHub::removeEventsOfOwner(std::string_view owner) {
    if (owner.empty()) {
        return 0;
    }
    // 先收集名字再逐个删: removeEvent 会顺手把挂着它的等待收尾, 不能边遍历边 erase
    std::vector<std::string> names;
    for (const auto& [name, entry] : records_) {
        if (entry.rec.owner == owner) {
            names.push_back(name);
        }
    }
    for (const auto& name : names) {
        removeEvent(name);
    }
    if (!names.empty()) {
        XX_LOGI("[event_hub] removed {} event(s) of owner `{}`", names.size(), owner);
    }
    return names.size();
}

size_t EventHub::cancelWaitsOfOwner(std::string_view owner, std::string_view reason) {
    if (owner.empty()) {
        return 0;
    }
    size_t canceled = 0;
    for (auto& [_, entry] : waits_) {
        if (entry.wait.state != WaitState::Armed || entry.wait.spec.owner != owner) {
            continue;
        }
        finishWait(entry, WaitState::Canceled, reason.empty() ? "owner-removed" : reason, true);
        ++canceled;
    }
    if (canceled != 0) {
        XX_LOGI("[event_hub] canceled {} wait(s) of owner `{}`", canceled, owner);
    }
    return canceled;
}

size_t EventHub::eventCountOfOwner(std::string_view owner) const {
    if (owner.empty()) {
        return 0;
    }
    return static_cast<size_t>(std::count_if(records_.begin(), records_.end(), [&](const auto& item) {
        return item.second.rec.owner == owner;
    }));
}

size_t EventHub::waitCountOfOwner(std::string_view owner) const {
    if (owner.empty()) {
        return 0;
    }
    return static_cast<size_t>(std::count_if(waits_.begin(), waits_.end(), [&](const auto& item) {
        return item.second.wait.state == WaitState::Armed && item.second.wait.spec.owner == owner;
    }));
}

size_t EventHub::removeOwned(std::string_view owner, std::string_view reason) {
    if (owner.empty()) {
        return 0;
    }
    auto       ownerCopy  = std::string{owner};
    auto       reasonCopy = std::string{reason};
    auto       count      = std::make_shared<size_t>(0);
    // 结果放在堆上: 即使等超时了, 后到的执行也不会写坏调用方的栈
    callSync([ownerCopy, reasonCopy, count](EventHub& hub) {
        *count = hub.cancelWaitsOfOwner(ownerCopy, reasonCopy) + hub.removeEventsOfOwner(ownerCopy);
    });
    return *count;
}

std::pair<size_t, size_t> EventHub::ownedCounts(std::string_view owner) {
    if (owner.empty()) {
        return {0, 0};
    }
    auto ownerCopy = std::string{owner};
    auto counts    = std::make_shared<std::pair<size_t, size_t>>(0, 0);
    callSync([ownerCopy, counts](EventHub& hub) {
        *counts = {hub.eventCountOfOwner(ownerCopy), hub.waitCountOfOwner(ownerCopy)};
    });
    return *counts;
}

// ---------------------------------------------------------------------------
// 值 → 等待 → 投递
// ---------------------------------------------------------------------------

Json EventHub::dispatchValue(RecordEntry& rec, const Json& value) {
    const auto bytes      = valueBytesOf(value);
    Json       deliveries = Json::array();
    for (auto& [_, entry] : waits_) {
        auto& wait = entry.wait;
        if (wait.state != WaitState::Armed || wait.spec.event != rec.rec.name) {
            continue;
        }
        // 值过滤: 不匹配的值不消耗等待 (别人的任务完成不能把"我等的那一条"吃掉)
        if (!waitMatchesSpec(wait.spec, value)) {
            continue;
        }
        auto& spec = wait.spec;
        wait.lastValueAtMs = systemNowMs();
        if (spec.mode == "first") {
            wait.values = {value};
            wait.bytes  = bytes;
            // 值保留在等待里: 查询与 `awaitOnce` 的返回值都靠它
            flushWait(entry, "value", /*resetValues=*/false);
            deliveries.push_back({{ "waitId", wait.waitId }, { "delivery", wait.spec.delivery }});
            finishWait(entry, WaitState::Satisfied, "delivered", true);
            continue;
        }
        if (spec.mode == "each") {
            if (spec.coalesce.count() > 0) {
                entry.coalesced.push_back(value);
                entry.coalescedBytes += bytes;
                if (entry.coalesceUntilMs == 0) {
                    entry.coalesceUntilMs = nowSteadyMs() + spec.coalesce.count();
                }
                const bool reached = (spec.maxItems > 0 && entry.coalesced.size() >= spec.maxItems)
                                     || (spec.maxBytes > 0 && entry.coalescedBytes >= spec.maxBytes);
                if (reached) {
                    wait.values = std::move(entry.coalesced);
                    entry.coalesced.clear();
                    entry.coalescedBytes  = 0;
                    entry.coalesceUntilMs = 0;
                    flushWait(entry, "values");
                    deliveries.push_back(
                        { { "waitId", wait.waitId }, { "delivery", wait.spec.delivery } }
                    );
                }
                continue;
            }
            wait.values = {value};
            wait.bytes  = bytes;
            flushWait(entry, "value");
            deliveries.push_back({ { "waitId", wait.waitId }, { "delivery", wait.spec.delivery } });
            continue;
        }
        // all: 累积到上限就整体投递一次, 之后继续累积
        wait.values.push_back(value);
        wait.bytes += bytes;
        const bool reached = (spec.maxItems > 0 && wait.values.size() >= spec.maxItems)
                             || (spec.maxBytes > 0 && wait.bytes >= spec.maxBytes);
        if (reached) {
            flushWait(entry, "values");
            deliveries.push_back({ { "waitId", wait.waitId }, { "delivery", wait.spec.delivery } });
        }
    }
    return deliveries;
}

void EventHub::dispatchEnded(RecordEntry& rec) {
    for (auto& [_, entry] : waits_) {
        auto& wait = entry.wait;
        if (wait.state != WaitState::Armed || wait.spec.event != rec.rec.name) {
            continue;
        }
        // 带值过滤的等待不随事件结束收尾: 共用一个事件名的多条流里,
        // 别人的流结束不代表我要的那条有结果
        if (!wait.spec.match.empty()) {
            continue;
        }
        if (!entry.coalesced.empty()) {
            wait.values = std::move(entry.coalesced);
            entry.coalesced.clear();
            entry.coalescedBytes  = 0;
            entry.coalesceUntilMs = 0;
            flushWait(entry, "values");
        }
        if (wait.spec.mode == "all") {
            // 流结束: 把攒下来的值一次给出 (可能为空)
            flushWait(entry, wait.values.empty() ? "ended-empty" : "stream-ended");
            finishWait(entry, WaitState::Satisfied, "event-ended", true);
        } else if (wait.spec.mode == "first") {
            // 一个值都没等到: 明确告诉模型"结束了也没有值", 否则它会永远挂着
            flushWait(entry, "ended-empty");
            finishWait(entry, WaitState::Satisfied, "event-ended", true);
        } else {
            // each: 值都投过了, 静默收尾
            finishWait(entry, WaitState::Satisfied, "event-ended", true);
        }
    }
}

void EventHub::flushWait(WaitEntry& entry, std::string_view status, bool resetValues) {
    auto&      wait    = entry.wait;
    const auto content = contentText(wait.spec.originSession, wait.spec.event, wait.values);
    std::string note;
    if (status == "stream-ended" || status == "ended-empty") {
        if (const auto* rec = findRecord(wait.spec.event)) {
            note = fmt::format("stream ended with {} value(s)", rec->rec.published);
        }
        if (wait.values.empty()) {
            note = "the event ended without any value";
        }
    }
    EventDelivery delivery;
    delivery.sessionId     = wait.spec.originSession;
    delivery.text          = renderEventText(wait, status, content, note);
    delivery.source        = std::string{agentxx::agent::kInputSourceEventPrefix}
                             + wait.spec.event;
    delivery.delivery      = wait.spec.delivery;
    delivery.waitId        = wait.waitId;
    delivery.eventName     = wait.spec.event;
    delivery.mode          = wait.spec.mode;
    delivery.ref           = wait.spec.ref;
    delivery.unpausePaused = wait.spec.unpausePaused;
    delivery.dropIfBusy    = wait.spec.dropIfBusy;

    std::string  reason;
    const auto   result = deliver(delivery, &reason);
    ++wait.deliveries;
    if (result == DeliverResult::Dropped) {
        wait.closeReason = "dropped:" + reason;
    }
    XX_LOGD(
        "[event_hub] wait {} ({} {}) delivered status={} result={}{}",
        wait.waitId,
        wait.spec.event,
        wait.spec.mode,
        status,
        static_cast<int>(result),
        reason.empty() ? std::string{} : fmt::format(" reason={}", reason)
    );
    if (resetValues) {
        wait.values.clear();
        wait.bytes = 0;
    }
}

void EventHub::finishWait(
    WaitEntry&       entry,
    WaitState        state,
    std::string_view reason,
    bool             notify
) {
    auto& wait = entry.wait;
    if (wait.state != WaitState::Armed) {
        return;
    }
    wait.state              = state;
    wait.closedAtSteadyMs   = nowSteadyMs();
    if (wait.closeReason.empty() || reason != "delivered") {
        wait.closeReason = std::string{reason};
    }
    if (notify && entry.onSatisfied) {
        auto callback       = std::move(entry.onSatisfied);
        entry.onSatisfied   = nullptr;
        const auto snapshot = wait;
        callback(snapshot);
    }
}

std::string EventHub::contentText(
    std::string_view         sessionId,
    std::string_view         eventName,
    const std::vector<Json>& values
) {
    // 值按原样输出 (别人的 JSON 不改); 只有框架自己的后台任务结果事件把正文字段原文给出
    auto text = renderEventValuesText(values, eventTextPayloadField(eventName));
    if (text.empty() || cfg_.contentLimitLength == 0) {
        return text;
    }
    // 后台工具任务 (`kind=tool`) 的结果正文在工具执行层就按工具自己的声明裁剪过
    // (见 [agentxx::util::offloadToolResultIfOversized]): 这里不再整段卸载, 否则
    // 同一段输出会被套上第二层取回提示 (工具层的提示 + 事件层的提示各一个 id)
    if (isAsyncResultEvent(eventName) && asyncValuesAreToolKind(values)) {
        return text;
    }
    // 字节数不足阈值时不可能达到阈值字符数 (utf8 每字符至少 1 字节), 提前返回
    if (text.size() < cfg_.contentLimitLength) {
        return text;
    }
    auto ctx = ctx_.lock();
    if (!ctx || sessionId.empty()) {
        return text;
    }
    // 整段内容转会话 share store, 文本里给 id 与取回提示
    // (与工具输出压缩同一套判定与提示, 见 offloadShareStoreIfOversized)
    auto offload
        = agentxx::util::offloadShareStoreIfOversized(ctx, sessionId, text, cfg_.contentLimitLength);
    return std::move(offload.text);
}

DeliverResult EventHub::drop(std::string_view reason) {
    ++stats_.dropped;
    ++stats_.droppedByReason[std::string{reason}];
    if (auto ctx = ctx_.lock(); ctx && ctx->metrics) {
        ctx->metrics->noteEventDropped();
    }
    XX_LOGW("[event_hub] event input dropped: {}", reason);
    return DeliverResult::Dropped;
}

/// 投递成功结算 (计数 + 关键指标)
void EventHub::noteDelivered() {
    ++stats_.delivered;
    if (auto ctx = ctx_.lock(); ctx && ctx->metrics) {
        ctx->metrics->noteEventDelivered();
    }
}

DeliverResult EventHub::deliver(const EventDelivery& d, std::string* reasonOut) {
    auto setReason = [&](std::string_view r) {
        if (reasonOut) {
            *reasonOut = std::string{r};
        }
    };
    if (closed_) {
        setReason("closed");
        return drop("closed");
    }
    auto ctx = ctx_.lock();
    if (!ctx) {
        setReason("no-context");
        return drop("no-context");
    }
    if (ctx->isShuttingDown()) {
        setReason("shutting-down");
        return drop("shutting-down");
    }
    auto session = ctx->sessions ? ctx->sessions->get(d.sessionId) : nullptr;
    if (!session) {
        // 不自动新建会话: 乱发事件不应凭空造出会话
        XX_LOGW(
            "[event_hub] event `{}` input dropped: session `{}` not found",
            d.eventName,
            d.sessionId
        );
        setReason("no-session");
        return drop("no-session");
    }
    if (!session->isIoThread()) {
        // 防御: 正常路径都在 io 线程投递; 不在时 post 回 hub 线程再投
        auto self = shared_from_this();
        auto copy = d;
        asio::post(executor_, [self, copy = std::move(copy)]() {
            std::string ignored;
            self->deliver(copy, &ignored);
        });
        setReason({});
        noteDelivered();
        return DeliverResult::Queued;
    }
    if (d.delivery == "none") {
        // 只留在 hub, 供 status/list 查
        setReason("delivery-none");
        ++stats_.deferred;
        return DeliverResult::Deferred;
    }

    auto sink      = session->inputSink.lock();
    const bool busy = sink && sink->turnActive();

    auto enqueuePending = [&](std::string_view mode) {
        if (session->pendingInputCount("event:") >= cfg_.deliveryBacklogPerSession) {
            return false;
        }
        agentxx::agent::SessionPendingInput input;
        input.text        = d.text;
        input.source      = d.source.empty()
                                ? (std::string{agentxx::agent::kInputSourceEventPrefix} + d.eventName)
                                : d.source;
        input.delivery    = std::string{mode};
        input.createdAtMs = systemNowMs();
        session->enqueuePendingInput(std::move(input));
        return true;
    };

    const bool nextTurn = (d.delivery == agentxx::agent::InputDelivery::NextTurn);

    if (d.dropIfBusy && busy) {
        setReason("busy");
        return drop("busy");
    }

    if (!busy) {
        // 空闲: 有驱动端就开新一轮 (没有"下一次 modelcall"可言), 否则写待注入输入等
        if (sink) {
            // 后台任务结果 (`agentxx.agent.async.*`) 是"已经跑完的事"的产物, 必须进
            // 上下文: 会话若因上一轮失败/取消停在暂停态, 也要解除暂停把它跑起来,
            // 否则它只能躺在队列里等到用户下次发消息 (那时才被顺带取走)
            const bool unpause = d.unpausePaused || isAsyncResultEvent(d.eventName);
            if (sink->submitTurnInput(d.text, d.source, unpause)) {
                setReason({});
                noteDelivered();
                return DeliverResult::Queued;
            }
            setReason("sink-rejected");
            return drop("sink-rejected");
        }
        if (cfg_.allowHeadlessTurn && turnRunner_) {
            // 无驱动端自起一轮: 来源标记一并下去, 展示历史与有驱动端时一致
            turnRunner_(
                d.sessionId,
                d.text,
                d.source.empty()
                    ? (std::string{agentxx::agent::kInputSourceEventPrefix} + d.eventName)
                    : d.source
            );
            setReason({});
            noteDelivered();
            return DeliverResult::Queued;
        }
        if (enqueuePending(agentxx::agent::InputDelivery::NextStep)) {
            setReason("no-driver");
            ++stats_.deferred;
            return DeliverResult::Deferred;
        }
        setReason("backlog");
        return drop("backlog");
    }

    // 轮次在跑
    if (nextTurn) {
        // 排队等这一轮跑完
        if (sink && sink->submitTurnInput(d.text, d.source, d.unpausePaused)) {
            setReason({});
            noteDelivered();
            return DeliverResult::Queued;
        }
        if (enqueuePending(agentxx::agent::InputDelivery::NextStep)) {
            setReason({});
            noteDelivered();
            return DeliverResult::Queued;
        }
        setReason("backlog");
        return drop("backlog");
    }
    // next-step 在下一次 modelcall 前插入上下文; inject 只进下一次请求
    const auto mode = d.delivery == agentxx::agent::InputDelivery::Inject
                          ? agentxx::agent::InputDelivery::Inject
                          : agentxx::agent::InputDelivery::NextStep;
    if (enqueuePending(mode)) {
        setReason({});
        noteDelivered();
        return DeliverResult::Queued;
    }
    setReason("backlog");
    return drop("backlog");
}

// ---------------------------------------------------------------------------
// 等待
// ---------------------------------------------------------------------------

std::expected<EventWait, std::string> EventHub::watch(EventWaitSpec spec) {
    if (closed_) {
        return std::unexpected{std::string{"event hub is closed"}};
    }
    std::string err;
    if (!naming::validateEventName(spec.event, &err)) {
        return std::unexpected{err};
    }
    if (spec.mode != "first" && spec.mode != "each" && spec.mode != "all") {
        return std::unexpected{
            fmt::format("mode must be first|each|all, got `{}`", spec.mode)
        };
    }
    if (spec.delivery.empty()) {
        spec.delivery = cfg_.deliveryDefault;
    }
    if (spec.delivery != "none" && !agentxx::agent::InputDelivery::known(spec.delivery)) {
        return std::unexpected{
            fmt::format("delivery must be next-step|inject|next-turn|none, got `{}`", spec.delivery)
        };
    }
    if (spec.originSession.empty() && spec.delivery != "none") {
        return std::unexpected{std::string{"originSession is empty"}};
    }
    if (waits_.size() >= cfg_.maxWaits) {
        return std::unexpected{fmt::format("too many waits (limit {})", cfg_.maxWaits)};
    }
    if (!spec.originSession.empty() && cfg_.maxWaitsPerSession > 0) {
        const auto perSession = static_cast<size_t>(std::count_if(
            waits_.begin(),
            waits_.end(),
            [&](const auto& item) {
                return item.second.wait.state == WaitState::Armed
                       && item.second.wait.spec.originSession == spec.originSession;
            }
        ));
        if (perSession >= cfg_.maxWaitsPerSession) {
            return std::unexpected{fmt::format(
                "too many waits for this session (limit {})",
                cfg_.maxWaitsPerSession
            )};
        }
    }
    if (spec.timeout.count() <= 0 && cfg_.waitTimeoutDefault.count() > 0) {
        spec.timeout = cfg_.waitTimeoutDefault;
    }
    if (spec.mode == "first") {
        spec.maxItems = 1;
    }

    // 有人在等这个事件: 建一条记录 (即使发布方还没 define), 否则查询与
    // 回放都看不到它
    if (nullptr
        == ensureRecord(
            spec.event,
            ownerOfName(spec.event, spec.owner.empty() ? "watch" : spec.owner),
            spec.originSession
        )) {
        return std::unexpected{fmt::format("too many events (limit {})", cfg_.maxEvents)};
    }
    XX_LOGI(
        "[event_hub] watch {}: events={} waits={}",
        spec.event,
        records_.size(),
        waits_.size()
    );

    WaitEntry entry;
    entry.wait.waitId     = fmt::format("w-{}", ++waitSeq_);
    entry.wait.armedAtMs  = systemNowMs();
    entry.wait.state      = WaitState::Armed;
    if (spec.timeout.count() > 0) {
        entry.deadlineMs = nowSteadyMs() + spec.timeout.count();
    }
    const auto waitId = entry.wait.waitId;
    entry.wait.spec   = std::move(spec);
    auto& stored      = waits_[waitId];
    stored            = std::move(entry);

    // 回放: 注册之前到达的值也要给 (减少"我明明发了它却没收到"的往返)
    if (stored.wait.spec.replay) {
        if (auto* rec = findRecord(stored.wait.spec.event); rec && !rec->rec.history.empty()) {
            std::vector<Json> replay;
            replay.reserve(rec->rec.history.size());
            for (const auto& value : rec->rec.history) {
                if (waitMatchesSpec(stored.wait.spec, value)) {
                    replay.push_back(value);
                }
            }
            if (replay.empty()) {
                // 历史里没有匹配的值: 不回放, 继续等新值
            } else if (stored.wait.spec.mode == "all") {
                for (auto& value : replay) {
                    stored.wait.values.push_back(value);
                    stored.wait.bytes += valueBytesOf(value);
                    if ((stored.wait.spec.maxItems > 0
                         && stored.wait.values.size() >= stored.wait.spec.maxItems)
                        || (stored.wait.spec.maxBytes > 0
                            && stored.wait.bytes >= stored.wait.spec.maxBytes)) {
                        break;
                    }
                }
            } else {
                // first / each: 历史值合并成一次投递 (逐条投会把注册瞬间变成刷屏)
                stored.wait.values.clear();
                stored.wait.values.push_back(replay.back());
                stored.wait.bytes = valueBytesOf(replay.back());
                if (stored.wait.spec.mode == "first") {
                    flushWait(stored, "value", /*resetValues=*/false);
                    finishWait(stored, WaitState::Satisfied, "delivered", true);
                } else {
                    flushWait(stored, stored.wait.values.size() > 1 ? "values" : "value");
                }
            }
        }
    }

    // 事件已经结束: 不再等
    // - 带值过滤的等待除外: 它是多任务共用一个事件名时的定向等待, 只看值
    if (stored.wait.state == WaitState::Armed && stored.wait.spec.match.empty()) {
        if (auto* rec = findRecord(stored.wait.spec.event);
            rec && rec->rec.state != EventState::Open) {
            if (stored.wait.spec.mode == "first") {
                flushWait(stored, "ended-empty");
                finishWait(stored, WaitState::Satisfied, "event-ended", true);
            } else if (stored.wait.spec.mode == "all") {
                flushWait(stored, stored.wait.values.empty() ? "ended-empty" : "stream-ended");
                finishWait(stored, WaitState::Satisfied, "event-ended", true);
            } else {
                finishWait(stored, WaitState::Satisfied, "event-ended", true);
            }
        }
    }
    armSweepTimer();
    return stored.wait;
}

std::optional<EventWait> EventHub::queryWait(std::string_view waitId) const {
    auto it = waits_.find(waitId);
    if (it == waits_.end()) {
        return std::nullopt;
    }
    return it->second.wait;
}

std::vector<EventWait>
    EventHub::listWaits(std::string_view sessionId, std::string_view event) const {
    std::vector<EventWait> out;
    for (const auto& [_, entry] : waits_) {
        if (!sessionId.empty() && entry.wait.spec.originSession != sessionId) {
            continue;
        }
        if (!event.empty() && entry.wait.spec.event != event) {
            continue;
        }
        out.push_back(entry.wait);
    }
    return out;
}

bool EventHub::cancelWait(std::string_view waitId, std::string_view reason) {
    auto it = waits_.find(waitId);
    if (it == waits_.end()) {
        return false;
    }
    if (it->second.wait.state != WaitState::Armed) {
        return false;
    }
    finishWait(it->second, WaitState::Canceled, reason, true);
    return true;
}

size_t EventHub::cancelWaitsOfSession(std::string_view sessionId, std::string_view reason) {
    size_t Canceled = 0;
    for (auto& [_, entry] : waits_) {
        if (entry.wait.state != WaitState::Armed) {
            continue;
        }
        if (!sessionId.empty() && entry.wait.spec.originSession != sessionId) {
            continue;
        }
        finishWait(entry, WaitState::Canceled, reason, true);
        ++Canceled;
    }
    return Canceled;
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

std::optional<EventRecord> EventHub::queryEvent(std::string_view name) const {
    const auto* rec = findRecord(name);
    if (nullptr == rec) {
        return std::nullopt;
    }
    EventRecord copy = rec->rec;
    copy.waitCount   = static_cast<size_t>(std::count_if(
        waits_.begin(),
        waits_.end(),
        [&](const auto& item) {
            return item.second.wait.state == WaitState::Armed && item.second.wait.spec.event == name;
        }
    ));
    return copy;
}

Json EventHub::listEventsJson(std::string_view prefix) const {
    Json list = Json::array();
    for (const auto& [name, entry] : records_) {
        if (!prefix.empty() && !std::string_view{name}.starts_with(prefix)) {
            continue;
        }
        Json item = Json::object();
        item["name"]        = name;
        item["kind"]        = entry.rec.kind == EventKind::Single ? "single" : "stream";
        item["state"]       = entry.rec.state == EventState::Open      ? "open"
                              : entry.rec.state == EventState::Ended   ? "ended"
                                                                      : "expired";
        item["owner"]       = entry.rec.owner;
        item["description"] = entry.rec.description;
        item["publishTo"]   = publishMaskText(entry.rec.publishMask);
        item["published"]   = entry.rec.published;
        item["history"]     = entry.rec.history.size();
        item["waits"]       = std::count_if(
            waits_.begin(),
            waits_.end(),
            [&](const auto& w) {
                return w.second.wait.state == WaitState::Armed
                       && w.second.wait.spec.event == name;
            }
        );
        if (!entry.rec.history.empty()) {
            item["last"] = entry.rec.history.back();
        }
        list.push_back(std::move(item));
    }
    Json out    = Json::object();
    out["count"] = list.size();
    out["events"] = std::move(list);
    return out;
}

Json EventHub::listWaitsJson(std::string_view sessionId) const {
    Json list = Json::array();
    for (const auto& [waitId, entry] : waits_) {
        if (!sessionId.empty() && entry.wait.spec.originSession != sessionId) {
            continue;
        }
        Json item = Json::object();
        item["wait"]     = waitId;
        item["event"]    = entry.wait.spec.event;
        item["mode"]     = entry.wait.spec.mode;
        item["delivery"] = entry.wait.spec.delivery;
        item["state"]    = entry.wait.state == WaitState::Armed     ? "armed"
                           : entry.wait.state == WaitState::Satisfied ? "satisfied"
                           : entry.wait.state == WaitState::Canceled ? "Canceled"
                                                                      : "timed-out";
        item["values"]     = entry.wait.values.size();
        item["deliveries"] = entry.wait.deliveries;
        if (!entry.wait.closeReason.empty()) {
            item["reason"] = entry.wait.closeReason;
        }
        list.push_back(std::move(item));
    }
    Json out      = Json::object();
    out["count"]  = list.size();
    out["waits"]  = std::move(list);
    return out;
}

Json EventHub::statsJson() const {
    Json out                    = Json::object();
    out["events"]                = records_.size();
    out["waits"]                 = waits_.size();
    out["delivered"]             = stats_.delivered;
    out["deferred"]              = stats_.deferred;
    out["dropped"]               = stats_.dropped;
    out["waitsTimedOut"]         = stats_.waitsTimedOut;
    out["values"]                = stats_.valueCount;
    out["closed"]                = closed_;
    out["allowHeadlessTurn"]     = cfg_.allowHeadlessTurn;
    Json reasons                 = Json::object();
    for (const auto& [reason, count] : stats_.droppedByReason) {
        reasons[reason] = count;
    }
    out["droppedByReason"] = std::move(reasons);
    return out;
}

// ---------------------------------------------------------------------------
// 定时源
// ---------------------------------------------------------------------------

bool EventHub::armTimer(std::string_view name, int64_t afterMs, int64_t intervalMs) {
    if (closed_ || afterMs <= 0) {
        return false;
    }
    const auto  eventName = timerEventName(name);
    const auto  shortName = timerShortName(eventName);
    std::string err;
    if (shortName.empty()) {
        XX_LOGW("[event_hub] armTimer: timer name is empty");
        return false;
    }
    if (!naming::validateAgentEventName(eventName, &err)) {
        XX_LOGW("[event_hub] invalid timer name `{}`: {}", eventName, err);
        return false;
    }
    std::string defineErr;
    EventDefineSpec spec;
    spec.name        = eventName;
    spec.kind        = EventKind::Stream;
    spec.owner       = "timer";
    spec.description = fmt::format("timer source `{}`", shortName);
    spec.historyLimit = 1;
    define(spec, &defineErr);

    TimerSource src;
    src.eventName  = eventName;
    src.intervalMs = intervalMs;
    src.nextFireMs = nowSteadyMs() + afterMs;
    src.timer      = std::make_shared<asio::steady_timer>(executor_);
    auto& stored   = timers_[eventName];
    stored         = std::move(src);
    scheduleTimer(eventName);
    return true;
}

bool EventHub::cancelTimer(std::string_view name) {
    auto key = timerEventName(name);
    auto it  = timers_.find(key);
    if (it == timers_.end()) {
        return false;
    }
    if (it->second.timer) {
        it->second.timer->cancel();
    }
    timers_.erase(it);
    return true;
}

void EventHub::cancelAllTimers() {
    for (auto& [_, src] : timers_) {
        if (src.timer) {
            src.timer->cancel();
        }
    }
    timers_.clear();
}

void EventHub::scheduleTimer(std::string_view key) {
    auto it = timers_.find(key);
    if (it == timers_.end()) {
        return;
    }
    auto& src = it->second;
    if (nullptr == src.timer) {
        return;
    }
    const auto delayMs = std::max<int64_t>(0, src.nextFireMs - nowSteadyMs());
    src.timer->expires_after(std::chrono::milliseconds{delayMs});
    auto weak = weak_from_this();
    src.timer->async_wait([weak, key = std::string{key}](const utilxx_base::AsioErrorCode& ec) {
        if (ec) {
            return;
        }
        if (auto locked = weak.lock()) {
            locked->fireTimer(key);
        }
    });
}

void EventHub::fireTimer(std::string_view key) {
    auto it = timers_.find(key);
    if (it == timers_.end()) {
        return;
    }
    auto& src = it->second;
    ++src.fired;
    Json value = Json::object();
    value["name"]      = timerShortName(src.eventName);
    value["n"]         = src.fired;
    value["firedAtMs"] = systemNowMs();
    const bool repeating = src.intervalMs > 0;
    const auto eventName = src.eventName;
    std::string err;
    if (!publishValue(eventName, std::move(value), "timer", &err)) {
        XX_LOGW("[event_hub] timer `{}` publish failed: {}", eventName, err);
        timers_.erase(it);
        return;
    }
    if (!repeating) {
        timers_.erase(it);
        return;
    }
    auto again = timers_.find(key);
    if (again == timers_.end()) {
        return;
    }
    again->second.nextFireMs = nowSteadyMs() + again->second.intervalMs;
    scheduleTimer(key);
}

// ---------------------------------------------------------------------------
// 超时 / 回收
// ---------------------------------------------------------------------------

int64_t EventHub::nowSteadyMs() {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()
    )
                                    .count());
}

void EventHub::armSweepTimer() {
    if (closed_ || sweepArmed_) {
        return;
    }
    if (!sweepTimer_) {
        sweepTimer_ = std::make_shared<asio::steady_timer>(executor_);
    }
    sweepArmed_ = true;
    sweepTimer_->expires_after(std::chrono::milliseconds{kSweepPeriodMs});
    auto weak = weak_from_this();
    sweepTimer_->async_wait([weak](const utilxx_base::AsioErrorCode& ec) {
        if (ec) {
            return;
        }
        auto locked = weak.lock();
        if (!locked) {
            return;
        }
        // 到期后清标记, 由 sweepExpired 末尾按需重新安排
        locked->sweepArmed_ = false;
        locked->sweepExpired();
    });
}

size_t EventHub::sweepExpired(int64_t steadyNowMs) {
    const auto now = steadyNowMs < 0 ? nowSteadyMs() : steadyNowMs;
    size_t     handled = 0;

    // 合并窗口到点: 把攒着的值投出去
    for (auto& [_, entry] : waits_) {
        if (entry.wait.state != WaitState::Armed || entry.coalesceUntilMs == 0) {
            continue;
        }
        if (now < entry.coalesceUntilMs) {
            continue;
        }
        entry.wait.values = std::move(entry.coalesced);
        entry.coalesced.clear();
        entry.coalescedBytes  = 0;
        entry.coalesceUntilMs = 0;
        if (!entry.wait.values.empty()) {
            flushWait(entry, entry.wait.values.size() > 1 ? "values" : "value");
            ++handled;
        }
    }

    // 等待超时
    for (auto& [_, entry] : waits_) {
        if (entry.wait.state != WaitState::Armed || entry.deadlineMs <= 0 || now < entry.deadlineMs) {
            continue;
        }
        ++stats_.waitsTimedOut;
        if (auto ctx = ctx_.lock(); ctx && ctx->metrics) {
            ctx->metrics->noteEventWaitTimeout();
        }
        if (entry.wait.spec.notifyOnTimeout) {
            entry.wait.values.clear();
            entry.wait.bytes = 0;
            flushWait(entry, "timeout");
        }
        finishWait(entry, WaitState::TimedOut, "timeout", true);
        ++handled;
    }

    // 回收终态等待
    for (auto it = waits_.begin(); it != waits_.end();) {
        const bool terminal = it->second.wait.state != WaitState::Armed
                              && it->second.wait.closedAtSteadyMs > 0
                              && now - it->second.wait.closedAtSteadyMs > kTerminalWaitLingerMs;
        it = terminal ? waits_.erase(it) : std::next(it);
    }

    // 事件 ttl / linger
    for (auto it = records_.begin(); it != records_.end();) {
        auto& entry = it->second;
        bool  erase = false;
        if (entry.rec.state == EventState::Open && entry.ttlDeadlineMs > 0
            && now >= entry.ttlDeadlineMs) {
            entry.rec.state         = EventState::Expired;
            entry.rec.endedAtMs     = systemNowMs();
            entry.lingerDeadlineMs  = now + std::min<int64_t>(entry.rec.lingerMs, 24 * 60 * 60 * 1000);
            const auto name         = entry.rec.name;
            if (auto* record = findRecord(name)) {
                dispatchEnded(*record);
            }
            ++handled;
        } else if (entry.rec.state != EventState::Open && entry.lingerDeadlineMs > 0
                   && now >= entry.lingerDeadlineMs) {
            erase = true;
        }
        it = erase ? records_.erase(it) : std::next(it);
    }

    if (!closed_) {
        armSweepTimer();
    }
    return handled;
}

// ---------------------------------------------------------------------------
// 阻塞等待 (agentxx_async_await / 插件登记等待后的取值路径)
// ---------------------------------------------------------------------------

asio::awaitable<EventWait>
    EventHub::awaitOnce(EventWaitSpec spec, std::shared_ptr<neograph::graph::CancelToken> ct) {
    spec.delivery = "none";
    spec.dropIfBusy = false;
    // 工具自己会把"超时"作为结果返回, 临时等待不需要再往会话投提示
    spec.notifyOnTimeout = false;
    struct SharedResult {
        std::optional<EventWait> wait;
    };
    auto shared = std::make_shared<SharedResult>();

    auto watched = watch(std::move(spec));
    if (!watched.has_value()) {
        throw std::invalid_argument{watched.error()};
    }
    if (watched->state != WaitState::Armed) {
        // 已回放/已结束: 直接给出结果 (不留等待记录)
        auto it = waits_.find(watched->waitId);
        if (it != waits_.end() && it->second.wait.state != WaitState::Armed) {
            waits_.erase(it);
        }
        co_return *watched;
    }

    const auto waitId     = watched->waitId;
    const auto deadlineMs = watched->spec.timeout.count() > 0
                                ? nowSteadyMs() + watched->spec.timeout.count()
                                : 0;
    if (auto it = waits_.find(waitId); it != waits_.end()) {
        it->second.onSatisfied = [shared](const EventWait& finalWait) {
            shared->wait = finalWait;
        };
    }

    auto cleanup = [this, waitId]() {
        auto it = waits_.find(waitId);
        if (it != waits_.end() && it->second.wait.state != WaitState::Armed) {
            waits_.erase(it);
        }
    };

    while (true) {
        if (shared->wait.has_value()) {
            auto result = std::move(*shared->wait);
            cleanup();
            co_return result;
        }
        if (closed_) {
            cancelWait(waitId, "closed");
            throw neograph::graph::CancelledException{"event hub closed"};
        }
        if (ct && ct->is_cancelled()) {
            cancelWait(waitId, "Canceled");
            cleanup();
            throw neograph::graph::CancelledException{"event await Canceled"};
        }
        if (deadlineMs > 0 && nowSteadyMs() >= deadlineMs) {
            auto it = waits_.find(waitId);
            if (it != waits_.end()) {
                if (it->second.wait.state == WaitState::Armed) {
                    // 工具自己会把"超时"文本作为结果返回, 因此不投递超时提示
                    ++stats_.waitsTimedOut;
    if (auto ctx = ctx_.lock(); ctx && ctx->metrics) {
        ctx->metrics->noteEventWaitTimeout();
    }
                    finishWait(it->second, WaitState::TimedOut, "timeout", false);
                }
                auto result = it->second.wait;
                cleanup();
                co_return result;
            }
            auto fallback = *watched;
            fallback.state      = WaitState::TimedOut;
            fallback.closeReason = "timeout";
            co_return fallback;
        }
        asio::steady_timer timer(executor_, std::chrono::milliseconds{kAwaitPollMs});
        utilxx_base::AsioErrorCode ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
    }
}

} // namespace events
} // namespace agentxx
