#include "agentxx/event/hub.h"

#include "agentxx/agent/context.h"
#include "agentxx/agent/session_input_sink.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/util/exception.h"
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

/// 值的字节数 (紧凑 JSON 文本长度; 未构造文本时按 0)
size_t valueBytesOf(const Json& value) {
    if (value.is_null()) {
        return 0;
    }
    return value.dump().size();
}

/// 值 → 给模型看的文本 (字符串直接给原文, 其余走紧凑 JSON)
std::string rawValueText(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_null()) {
        return "null";
    }
    return value.dump();
}

/// 终态等待在表里保留多久 (供 `opt=status` 事后查看)
constexpr int64_t kTerminalWaitLingerMs = 60 * 1000;

/// 超时/回收检查周期
constexpr int64_t kSweepPeriodMs = 500;

/// `await` 轮询周期: 值到达没有回调唤醒时的兜底检查 (同时检查取消令牌)
constexpr int64_t kAwaitPollMs = 25;

/// 把 `timer.` 前缀规整掉 (两种写法都接受)
std::string timerShortName(std::string_view name) {
    if (name.starts_with("timer.")) {
        return std::string{name.substr(6)};
    }
    return std::string{name};
}

} // namespace

// ---------------------------------------------------------------------------
// 构造 / 生命周期
// ---------------------------------------------------------------------------

EventHub::EventHub(asio::any_io_executor ex, std::weak_ptr<agentxx::agent::AgentContext> ctx, Config cfg) :
    executor_(std::move(ex)),
    ctx_(std::move(ctx)),
    cfg_(std::move(cfg)) {}

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
    bus_ = bus;
    auto handlePrefix = [this](std::string_view topic, const std::any& data) {
        if (closed_) {
            return;
        }
        const auto* payload = std::any_cast<std::string>(&data);
        if (nullptr == payload) {
            // 非字符串载荷不是值主题 (插件事件统一是 std::string)
            return;
        }
        const auto name = naming::eventNameOfTopic(topic);
        if (!name.has_value()) {
            return;
        }
        // 载荷不是合法 JSON 时按纯文本值处理 (插件可能发普通字符串)
        Json       value     = Json(*payload);
        const bool looksJson = !payload->empty()
                               && (payload->front() == '{' || payload->front() == '['
                                   || payload->front() == '"');
        if (looksJson) {
            auto parsed = agentxx::util::catchError<std::optional<Json>>(
                [&]() -> std::optional<Json> {
                    return Json::parse(*payload);
                },
                [&](std::string errmsg) -> std::optional<Json> {
                    XX_LOGD("[event_hub] bus payload for `{}` is not JSON: {}", topic, errmsg);
                    return std::nullopt;
                }
            );
            if (parsed.has_value()) {
                value = std::move(*parsed);
            }
        }
        std::string err;
        if (!publishValue(*name, std::move(value), &err)) {
            XX_LOGW("[event_hub] publish from bus topic `{}` rejected: {}", topic, err);
        }
    };
    // 两个前缀都要挂: agent 内部发布用 `evt.`, 插件事件经 qualifyEventTopic 补 `plugin.`
    valueSubId_       = bus->listenPrefix(naming::kValueTopicPrefix, handlePrefix);
    pluginValueSubId_ = bus->listenPrefix(
        std::string{naming::kPluginTopicPrefix} + std::string{naming::kValueTopicPrefix},
        handlePrefix
    );
}

void EventHub::detachBus() {
    if (bus_) {
        if (valueSubId_ != 0) {
            bus_->unlistenPrefix(valueSubId_);
        }
        if (pluginValueSubId_ != 0) {
            bus_->unlistenPrefix(pluginValueSubId_);
        }
    }
    valueSubId_       = 0;
    pluginValueSubId_ = 0;
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
            finishWait(entry, WaitState::Cancelled, reason, true);
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
        return true;
    }
    auto* created = ensureRecord(spec.name, spec.owner, spec.originSession);
    if (nullptr == created) {
        return fail(fmt::format(
            "too many events (limit {})",
            cfg_.maxEvents
        ));
    }
    created->rec.kind         = spec.kind;
    created->rec.description  = spec.description;
    created->rec.historyLimit = spec.historyLimit;
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
    if (!naming::validateEventName(name, &nameErr)) {
        return fail(nameErr);
    }
    const auto bytes = valueBytesOf(value);
    if (cfg_.valueHardBytes > 0 && bytes > cfg_.valueHardBytes) {
        XX_LOGW(
            "[event_hub] value for `{}` rejected: {} bytes > hard limit {}",
            name,
            bytes,
            cfg_.valueHardBytes
        );
        return fail(fmt::format("value too large ({} bytes > {} bytes)", bytes, cfg_.valueHardBytes));
    }
    auto* rec = ensureRecord(name, "tool", "");
    if (nullptr == rec) {
        return fail(fmt::format("too many events (limit {})", cfg_.maxEvents));
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
        rec->rec.state           = EventState::Open;
        rec->rec.endedAtMs       = 0;
        rec->lingerDeadlineMs    = 0;
        XX_LOGD("[event_hub] event `{}` got a value after end, reopened", name);
    }
    const auto nameCopy = rec->rec.name;
    const auto count    = rec->rec.published;
    dispatchValue(*rec, value);
    if (rec->rec.kind == EventKind::Single) {
        std::string endErr;
        endEvent(nameCopy, Json::object(), &endErr);
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
    return true;
}

void EventHub::postValue(std::string_view name, Json value) {
    auto self = weak_from_this();
    auto n    = std::string{name};
    asio::post(executor_, [self, n = std::move(n), value = std::move(value)]() mutable {
        if (auto locked = self.lock()) {
            std::string err;
            if (!locked->publishValue(n, std::move(value), &err)) {
                XX_LOGW("[event_hub] posted value for `{}` rejected: {}", n, err);
            }
        }
    });
}

void EventHub::postEnd(std::string_view name, Json summary) {
    auto self = weak_from_this();
    auto n    = std::string{name};
    asio::post(executor_, [self, n = std::move(n), summary = std::move(summary)]() mutable {
        if (auto locked = self.lock()) {
            std::string err;
            if (!locked->endEvent(n, std::move(summary), &err)) {
                XX_LOGW("[event_hub] posted end for `{}` rejected: {}", n, err);
            }
        }
    });
}

bool EventHub::endEvent(std::string_view name, Json summary, std::string* err) {
    auto fail = [&](std::string msg) {
        if (err) {
            *err = std::move(msg);
        }
        return false;
    };
    auto* rec = findRecord(name);
    if (nullptr == rec) {
        return fail(fmt::format("event not found: {}", name));
    }
    if (rec->rec.state != EventState::Open) {
        // 幂等: 第二次 end 是 no-op
        return true;
    }
    rec->rec.state        = EventState::Ended;
    rec->rec.endedAtMs    = systemNowMs();
    rec->rec.updatedAtMs  = rec->rec.endedAtMs;
    rec->rec.summary      = summary.is_null() ? Json::object() : std::move(summary);
    rec->lingerDeadlineMs = nowSteadyMs() + std::min<int64_t>(rec->rec.lingerMs, 24 * 60 * 60 * 1000);
    const auto nameCopy   = rec->rec.name;
    const auto count      = rec->rec.published;
    const auto summaryText = rec->rec.summary.dump();
    dispatchEnded(*rec);
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
    return true;
}

bool EventHub::removeEvent(std::string_view name) {
    auto it = records_.find(name);
    if (it == records_.end()) {
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

// ---------------------------------------------------------------------------
// 值 → 等待 → 投递
// ---------------------------------------------------------------------------

void EventHub::dispatchValue(RecordEntry& rec, const Json& value) {
    const auto bytes = valueBytesOf(value);
    for (auto& [_, entry] : waits_) {
        auto& wait = entry.wait;
        if (wait.state != WaitState::Armed || wait.spec.event != rec.rec.name) {
            continue;
        }
        auto& spec = wait.spec;
        wait.lastValueAtMs = systemNowMs();
        if (spec.mode == "first") {
            wait.values = {value};
            wait.bytes  = bytes;
            // 值保留在等待里: `opt=status` 与 `awaitOnce` 的返回值都靠它
            flushWait(entry, "value", /*resetValues=*/false);
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
                }
                continue;
            }
            wait.values = {value};
            wait.bytes  = bytes;
            flushWait(entry, "value");
            continue;
        }
        // all: 累积到上限就整体投递一次, 之后继续累积
        wait.values.push_back(value);
        wait.bytes += bytes;
        const bool reached = (spec.maxItems > 0 && wait.values.size() >= spec.maxItems)
                             || (spec.maxBytes > 0 && wait.bytes >= spec.maxBytes);
        if (reached) {
            flushWait(entry, "values");
        }
    }
}

void EventHub::dispatchEnded(RecordEntry& rec) {
    for (auto& [_, entry] : waits_) {
        auto& wait = entry.wait;
        if (wait.state != WaitState::Armed || wait.spec.event != rec.rec.name) {
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
    auto& wait = entry.wait;
    std::vector<std::string> texts;
    texts.reserve(wait.values.size());
    for (const auto& value : wait.values) {
        texts.push_back(valueText(wait.spec.originSession, value));
    }
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
    delivery.text          = renderEventText(wait, status, texts, note);
    delivery.source        = "event:" + wait.spec.event;
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

std::string EventHub::valueText(std::string_view sessionId, const Json& value) {
    auto text = rawValueText(value);
    if (cfg_.valueBytes == 0 || text.size() < cfg_.valueBytes) {
        return text;
    }
    auto ctx = ctx_.lock();
    if (!ctx || sessionId.empty()) {
        return text;
    }
    // 超长值转会话 share store, 文本里给 id 与取回提示 (与工具输出压缩同一套实现)
    auto offload = agentxx::util::offloadShareStoreIfOversized(ctx, sessionId, text, cfg_.valueBytes);
    return std::move(offload.text);
}

DeliverResult EventHub::drop(std::string_view reason) {
    ++stats_.dropped;
    ++stats_.droppedByReason[std::string{reason}];
    XX_LOGW("[event_hub] event input dropped: {}", reason);
    return DeliverResult::Dropped;
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
        ++stats_.delivered;
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
        input.source      = d.source.empty() ? std::string{"event"} : d.source;
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
            if (sink->submitTurnInput(d.text, d.source, d.unpausePaused)) {
                setReason({});
                ++stats_.delivered;
                return DeliverResult::Queued;
            }
            setReason("sink-rejected");
            return drop("sink-rejected");
        }
        if (cfg_.allowHeadlessTurn && turnRunner_) {
            turnRunner_(d.sessionId, d.text);
            setReason({});
            ++stats_.delivered;
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
            ++stats_.delivered;
            return DeliverResult::Queued;
        }
        if (enqueuePending(agentxx::agent::InputDelivery::NextStep)) {
            setReason({});
            ++stats_.delivered;
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
        ++stats_.delivered;
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

    // 有人在等这个事件: 建一条记录 (即使发布方还没 define), 否则 `opt=status` /
    // `opt=list` 看不到它, 也回放不了先到的值
    if (nullptr == ensureRecord(spec.event, spec.owner.empty() ? "watch" : spec.owner, spec.originSession)) {
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
            std::vector<Json> replay{rec->rec.history.begin(), rec->rec.history.end()};
            if (stored.wait.spec.mode == "all") {
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
    if (stored.wait.state == WaitState::Armed) {
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
    finishWait(it->second, WaitState::Cancelled, reason, true);
    return true;
}

size_t EventHub::cancelWaitsOfSession(std::string_view sessionId, std::string_view reason) {
    size_t cancelled = 0;
    for (auto& [_, entry] : waits_) {
        if (entry.wait.state != WaitState::Armed) {
            continue;
        }
        if (!sessionId.empty() && entry.wait.spec.originSession != sessionId) {
            continue;
        }
        finishWait(entry, WaitState::Cancelled, reason, true);
        ++cancelled;
    }
    return cancelled;
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
                           : entry.wait.state == WaitState::Cancelled ? "cancelled"
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
    auto shortName = timerShortName(name);
    std::string err;
    if (shortName.empty()) {
        XX_LOGW("[event_hub] armTimer: timer name is empty");
        return false;
    }
    if (!naming::validateEventName(std::string{"timer."} + shortName, &err)) {
        XX_LOGW("[event_hub] invalid timer name `{}`: {}", shortName, err);
        return false;
    }
    auto eventName = std::string{"timer."} + shortName;
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
    auto key = std::string{"timer."} + timerShortName(name);
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
    if (!publishValue(eventName, std::move(value), &err)) {
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
// 阻塞等待 (agentxx_event opt=await)
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
            cancelWait(waitId, "cancelled");
            cleanup();
            throw neograph::graph::CancelledException{"event await cancelled"};
        }
        if (deadlineMs > 0 && nowSteadyMs() >= deadlineMs) {
            auto it = waits_.find(waitId);
            if (it != waits_.end()) {
                if (it->second.wait.state == WaitState::Armed) {
                    // 工具自己会把"超时"文本作为结果返回, 因此不投递超时提示
                    ++stats_.waitsTimedOut;
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
