#include "agentxx/tools/event.h"
#include "agentxx/util/neograph_json_bridge.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/async_work.h"
#include "agentxx/agent/context.h"
#include "agentxx/event/hub.h"
#include "agentxx/event/naming.h"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace tools {

namespace {

using agentxx::events::EventDefineSpec;
using agentxx::events::EventHub;
using agentxx::events::EventKind;
using agentxx::events::EventWait;
using agentxx::events::EventWaitSpec;
using agentxx::events::WaitState;

/// `await` 默认与硬上限 (毫秒): 它会让本轮停在这里, 必须有界
constexpr int64_t kAwaitDefaultMs = 30 * 1000;
constexpr int64_t kAwaitMaxMs     = 5 * 60 * 1000;

/// 取必填字符串参数 (缺失/空时报参数错误)
std::string requireString(const utilxx_base::Json& args, std::string_view key) {
    if (!args.is_object() || !args.contains(key) || !args[key].is_string()) {
        throw std::invalid_argument{fmt::format("Arg `{}` is required and must be a string", key)};
    }
    auto value = args[key].get<std::string>();
    if (value.empty()) {
        throw std::invalid_argument{fmt::format("Arg `{}` is empty", key)};
    }
    return value;
}

/// 取会话 id (由 ToolCallNode 注入; 缺失时报错)
std::string requireSessionId(const utilxx_base::Json& args) {
    auto sessionId = args.value("sessionId", std::string{});
    if (sessionId.empty()) {
        throw std::invalid_argument{"Toolcall inner error, need `sessionId`"};
    }
    return sessionId;
}

/// 后台任务的结果事件名 (单一事件名, 靠值里的 `asyncId` 区分)
constexpr std::string_view kAsyncDoneEvent = "agentxx.agent.async.done";

/// 毫秒 → `mm:ss` (后台任务耗时展示)
std::string formatElapsed(int64_t ms) {
    if (ms < 0) {
        ms = 0;
    }
    const auto totalSeconds = ms / 1000;
    return fmt::format("{:02d}:{:02d}", totalSeconds / 60, totalSeconds % 60);
}

/// 组装等待登记 (watch / await 共用)
EventWaitSpec buildWaitSpec(
    const utilxx_base::Json& args,
    std::string_view         sessionId,
    std::string_view         defaultDelivery
) {
    EventWaitSpec spec;
    // 事件名: `event` 或（后台任务）`id` 至少给一个; 给 `id` 时事件名固定
    const auto asyncId = args.value("id", std::string{});
    const auto eventArg
        = (args.contains("event") && args["event"].is_string())
              ? args["event"].get<std::string>()
              : std::string{};
    if (!eventArg.empty()) {
        spec.event = eventArg;
    } else if (!asyncId.empty()) {
        spec.event = std::string{kAsyncDoneEvent};
    } else {
        throw std::invalid_argument{"Arg `event` (or `id` for a background task) is required"};
    }
    spec.originSession = std::string{sessionId};
    spec.owner         = "tool";
    spec.mode          = args.value("mode", std::string{"first"});
    spec.delivery      = args.value("delivery", std::string{defaultDelivery});
    spec.label         = args.value("label", std::string{});
    spec.ref           = args.value("ref", std::string{});
    spec.replay        = args.value("replay", true);
    spec.notifyOnTimeout = args.value("notify_on_timeout", true);
    spec.unpausePaused   = args.value("unpause", false);
    const auto timeoutMs = args.value<int64_t>("timeout_ms", 0);
    if (timeoutMs > 0) {
        spec.timeout = std::chrono::milliseconds{timeoutMs};
    }
    const auto maxItems = args.value<int64_t>("max_items", 0);
    if (maxItems > 0) {
        spec.maxItems = static_cast<size_t>(maxItems);
    }
    const auto coalesceMs = args.value<int64_t>("coalesce_ms", 0);
    if (coalesceMs > 0) {
        spec.coalesce = std::chrono::milliseconds{coalesceMs};
    }
    if (spec.coalesce.count() > 0 && spec.mode != "each") {
        throw std::invalid_argument{"Arg `coalesce_ms` only applies to mode=each"};
    }
    // 后台任务定向等待: 给 `id` 时按值里的 asyncId 过滤
    if (!asyncId.empty()) {
        spec.match = utilxx_base::Json{{"asyncId", asyncId}};
        if (spec.ref.empty()) {
            spec.ref = asyncId;
        }
    }
    return spec;
}

/// 模型可写的事件名
/// - 只能落在 `agentxx.agent.event.*` (内置 agent 端事件族) 且不占用内建保留子段
///   (`turn.` / `tool.` / `subagent.` / `timer.` / `host.`)
/// - 插件事件 (`plugin.<插件名>.*`) 只读: 写操作由插件自己做
std::string requireWritableEventName(const utilxx_base::Json& args) {
    auto        name = requireString(args, "event");
    std::string err;
    if (!agentxx::events::naming::validateAgentEventName(name, &err)) {
        throw std::invalid_argument{fmt::format(
            "Arg `event` = `{}` is invalid: {}",
            name,
            err
        )};
    }
    if (agentxx::events::naming::isReservedEventSegment(name)) {
        throw std::invalid_argument{fmt::format(
            "Arg `event` = `{}` uses a segment reserved by a built-in source; pick another "
            "name under `agentxx.agent.event.`",
            name
        )};
    }
    return name;
}

/// 定时源事件名前缀 (`agentxx.agent.event.timer.`)
constexpr std::string_view kTimerEventPrefix = "agentxx.agent.event.timer.";

/// 可选: `watch` 顺手建立/重设定时源 (`after_ms` / `every_ms`)
void applyTimerArgs(EventHub& hub, const utilxx_base::Json& args, const EventWaitSpec& spec) {
    const auto afterMs = args.value<int64_t>("after_ms", 0);
    const auto everyMs = args.value<int64_t>("every_ms", 0);
    if (afterMs <= 0 && everyMs <= 0) {
        return;
    }
    if (!spec.event.starts_with(kTimerEventPrefix)) {
        throw std::invalid_argument{fmt::format(
            "Arg `after_ms`/`every_ms` needs a timer event name: `{}<name>` (got `{}`)",
            kTimerEventPrefix,
            spec.event
        )};
    }
    const auto first = afterMs > 0 ? afterMs : everyMs;
    if (!hub.armTimer(spec.event, first, everyMs)) {
        throw std::runtime_error{fmt::format(
            "Cannot arm timer for event `{}` (bad name or delay)",
            spec.event
        )};
    }
}

/// 已满足的等待 → 返回文本 (status 按值条数与事件状态推导)
std::string renderSatisfiedText(EventHub& hub, const EventWait& wait) {
    std::vector<std::string> texts;
    texts.reserve(wait.values.size());
    for (const auto& value : wait.values) {
        texts.push_back(hub.valueText(wait.spec.originSession, value));
    }
    std::string status = "value";
    if (texts.empty()) {
        status = "ended-empty";
    } else if (texts.size() > 1) {
        status = "values";
    }
    std::string note;
    if (status == "ended-empty") {
        note = "the event ended without any value";
    }
    return agentxx::events::renderEventText(wait, status, texts, note);
}

/// 后台任务列表文本 (给模型读的摘要, 不含结果正文)
std::string renderAsyncWorks(
    agentxx::agent::AgentHost& host,
    const std::string&         creatorAgentId,
    std::string_view           onlyId,
    std::string_view           stateFilter,
    bool                       allScopes,
    size_t                     limit,
    size_t                     maxConcurrent
) {
    auto items = utilxx_base::Json::array();
    if (!onlyId.empty()) {
        auto one = host.asyncWorkJson(onlyId);
        if (!one.is_object() || one.empty()) {
            throw std::invalid_argument{fmt::format("Background task not found: {}", onlyId)};
        }
        items.push_back(std::move(one));
    } else {
        items = host.asyncWorksJson(allScopes ? std::string_view{} : creatorAgentId, {}, true, 200);
    }

    size_t running = 0;
    size_t queued  = 0;
    auto   lines   = std::vector<std::string>{};
    auto   done    = std::vector<std::string>{};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch()
    )
                         .count();
    for (const auto& item : items) {
        const auto state = item.value("state", std::string{});
        if (state == "running") {
            ++running;
        } else if (state == "queued") {
            ++queued;
        }
        if (stateFilter != "all" && !stateFilter.empty() && state != stateFilter) {
            continue;
        }
        const auto id   = item.value("asyncId", std::string{});
        const auto kind = item.value("kind", std::string{});
        const auto name = item.value("name", std::string{});
        if (state == "running") {
            const auto elapsed = now - item.value("startedAtMs", now);
            lines.push_back(fmt::format(
                "- {} [running] {} {} 已跑 {}",
                id,
                kind,
                name,
                formatElapsed(elapsed)
            ));
        } else if (state == "queued") {
            const auto ahead = item.value("queueAhead", int64_t{0});
            lines.push_back(fmt::format(
                "- {} [queued]  {} {} 前方 {}",
                id,
                kind,
                name,
                ahead
            ));
        } else if (state == "done" || state == "failed" || state == "canceled") {
            const auto start  = item.value("startedAtMs", int64_t{0});
            const auto finish = item.value("finishedAtMs", int64_t{0});
            const auto mark   = state == "done" ? "ok" : (state == "canceled" ? "canceled" : "failed");
            done.push_back(fmt::format(
                "- {} {} [{}] 用时 {}",
                id,
                kind,
                mark,
                formatElapsed(finish - start)
            ));
        }
        if (lines.size() >= limit) {
            break;
        }
    }

    std::string out = fmt::format(
        "[Async work] running={} queued={} (per-queue limit {})\n",
        running,
        queued,
        maxConcurrent
    );
    if (lines.empty() && done.empty()) {
        out += "(no background task matches the filter)";
        return out;
    }
    for (const auto& line : lines) {
        out += line;
        out += "\n";
    }
    if (!done.empty()) {
        out += "最近完成:\n";
        for (const auto& line : done) {
            out += line;
            out += "\n";
        }
    }
    while (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    return out;
}

/// 事件列表文本
std::string renderEventList(EventHub& hub, std::string_view prefix, size_t limit) {
    const auto json  = hub.listEventsJson(prefix);
    const auto count = json["count"].is_number() ? json["count"].get<int64_t>() : 0;
    std::string out  = fmt::format("[Event list] events={}\n", count);
    size_t      shown = 0;
    if (json.contains("events") && json["events"].is_array()) {
        for (const auto& item : json["events"]) {
            if (shown >= limit) {
                out += fmt::format("- ... ({} more)\n", count - static_cast<int64_t>(shown));
                break;
            }
            out += fmt::format(
                "- {} ({}/{}, values={}, waits={})",
                item.value("name", std::string{}),
                item.value("kind", std::string{}),
                item.value("state", std::string{}),
                item.value("published", int64_t{0}),
                item.value("waits", int64_t{0})
            );
            const auto description = item.value("description", std::string{});
            if (!description.empty()) {
                out += fmt::format(" {}", description);
            }
            out += "\n";
            ++shown;
        }
    }
    return out;
}

/// 等待列表文本 (只列本会话的)
std::string renderWaitList(EventHub& hub, std::string_view sessionId, size_t limit) {
    const auto  json  = hub.listWaitsJson(sessionId);
    const auto  count = json["count"].is_number() ? json["count"].get<int64_t>() : 0;
    std::string out   = fmt::format("[Event list] waits={}\n", count);
    size_t      shown = 0;
    if (json.contains("waits") && json["waits"].is_array()) {
        for (const auto& item : json["waits"]) {
            if (shown >= limit) {
                out += fmt::format("- ... ({} more)\n", count - static_cast<int64_t>(shown));
                break;
            }
            out += fmt::format(
                "- {} {} mode={} state={} deliveries={}\n",
                item.value("wait", std::string{}),
                item.value("event", std::string{}),
                item.value("mode", std::string{}),
                item.value("state", std::string{}),
                item.value("deliveries", int64_t{0})
            );
            ++shown;
        }
    }
    return out;
}

} // namespace

EventTool::EventTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext) :
    XXToolBase(
        "agentxx_event",
        in_agentContext,
        /*autoSummaryOutput=*/true,
        /*canDelayLoad=*/false,
        /*maxRetry=*/0,
        /*repeatCallCheck=*/false,
        /*supportsParallel=*/false
    ) {}

neograph::ChatTool EventTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    utilxx_base::Json params = utilxx_base::Json{
        {"type", "object"},
        {
         "properties", {
                {
                    "opt",
                    {
                        {"type", "string"},
                        {"enum",
                         utilxx_base::Json::array({
                             "watch",
                             "await",
                             "status",
                             "list",
                             "works",
                             "cancel",
                             "emit",
                             "end",
                         })},
                        {"description", prompt.getArg("opt")},
                    },
                },
                {
                    "event",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("event")},
                    },
                },
                {
                    "wait",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("wait")},
                    },
                },
                {
                    "id",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("id")},
                    },
                },
                {
                    "mode",
                    {
                        {"type", "string"},
                        {"enum", utilxx_base::Json::array({"first", "each", "all"})},
                        {"description", prompt.getArg("mode")},
                    },
                },
                {
                    "delivery",
                    {
                        {"type", "string"},
                        {"enum",
                         utilxx_base::Json::array({"next-step", "inject", "next-turn", "none"})},
                        {"description", prompt.getArg("delivery")},
                    },
                },
                {
                    "timeout_ms",
                    {
                        {"type", "integer"},
                        {"description", prompt.getArg("timeout_ms")},
                    },
                },
                {
                    "max_items",
                    {
                        {"type", "integer"},
                        {"description", prompt.getArg("max_items")},
                    },
                },
                {
                    "coalesce_ms",
                    {
                        {"type", "integer"},
                        {"description", prompt.getArg("coalesce_ms")},
                    },
                },
                {
                    "after_ms",
                    {
                        {"type", "integer"},
                        {"description", prompt.getArg("after_ms")},
                    },
                },
                {
                    "every_ms",
                    {
                        {"type", "integer"},
                        {"description", prompt.getArg("every_ms")},
                    },
                },
                {
                    "value",
                    {
                        {"description", prompt.getArg("value")},
                    },
                },
                {
                    "summary",
                    {
                        {"description", prompt.getArg("summary")},
                    },
                },
                {
                    "single",
                    {
                        {"type", "boolean"},
                        {"description", prompt.getArg("single")},
                    },
                },
                {
                    "remove",
                    {
                        {"type", "boolean"},
                        {"description", prompt.getArg("remove")},
                    },
                },
                {
                    "all",
                    {
                        {"type", "boolean"},
                        {"description", prompt.getArg("all")},
                    },
                },
                {
                    "replay",
                    {
                        {"type", "boolean"},
                        {"description", prompt.getArg("replay")},
                    },
                },
                {
                    "notify_on_timeout",
                    {
                        {"type", "boolean"},
                        {"description", prompt.getArg("notify_on_timeout")},
                    },
                },
                {
                    "unpause",
                    {
                        {"type", "boolean"},
                        {"description", prompt.getArg("unpause")},
                    },
                },
                {
                    "label",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("label")},
                    },
                },
                {
                    "ref",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("ref")},
                    },
                },
                {
                    "scope",
                    {
                        {"type", "string"},
                        {"enum", utilxx_base::Json::array({"events", "waits", "all"})},
                        {"description", prompt.getArg("scope")},
                    },
                },
                {
                    "prefix",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("prefix")},
                    },
                },
                {
                    "limit",
                    {
                        {"type", "integer"},
                        {"description", prompt.getArg("limit")},
                    },
                },
            }, },
        {"required", utilxx_base::Json::array({"opt"})},
    };
    return {get_name(), prompt.depict, agentxx::util::toNeographJson(params)};
}

asio::awaitable<agentxx::util::ToolResult>
    EventTool::execute_async(const utilxx_base::Json& arguments) {
    auto agentContextPtr = agentContext.lock();
    if (!agentContextPtr) {
        throw std::runtime_error{"AgentContext not available"};
    }
    auto hub = agentContextPtr->eventHub;
    if (!hub) {
        throw std::runtime_error{
            "event hub is not available (disabled by config `event.enable=false`)"
        };
    }
    const auto sessionId = requireSessionId(arguments);
    const auto opt       = requireString(arguments, "opt");
    const auto asyncId   = arguments.value("id", std::string{});

    // 后台任务工具面: `works` / `status id=` / `cancel id=|scope=mine` 都直接问 agent 容器
    const auto hostOf = [&]() -> std::shared_ptr<agentxx::agent::AgentHost> {
        auto host = agentContextPtr->host.lock();
        if (!host) {
            throw std::runtime_error{"background tasks need the agent host (not attached)"};
        }
        return host;
    };
    // 本 agent 的并发额度 (唯一一类并发约束)
    const auto perQueueLimit = [&]() -> size_t {
        return agentContextPtr->agentConfig ? agentContextPtr->agentConfig->asyncWorkMaxConcurrent
                                            : size_t{3};
    };

    if (opt == "works") {
        const auto scope  = arguments.value("scope", std::string{"mine"});
        const auto state  = arguments.value("state", std::string{"all"});
        const auto limit  = static_cast<size_t>(
            std::clamp<int64_t>(arguments.value<int64_t>("limit", 20), 1, 200)
        );
        const bool allScopes = scope == "all";
        if (!allScopes && scope != "mine") {
            throw std::invalid_argument{fmt::format("Arg `scope` is invalid for opt=works: {}", scope)};
        }
        co_return agentxx::util::ToolResult::ok(renderAsyncWorks(
            *hostOf(),
            agentContextPtr->agentId,
            asyncId,
            state,
            allScopes,
            limit,
            perQueueLimit()
        ));
    }

    if (opt == "watch") {
        auto spec = buildWaitSpec(arguments, sessionId, hub->config().deliveryDefault);
        applyTimerArgs(*hub, arguments, spec);
        auto result = hub->watch(std::move(spec));
        if (!result.has_value()) {
            throw std::invalid_argument{result.error()};
        }
        if (result->state == WaitState::Satisfied) {
            co_return agentxx::util::ToolResult::ok(renderSatisfiedText(*hub, *result));
        }
        co_return agentxx::util::ToolResult::ok(agentxx::events::renderWaitArmed(*result));
    }

    if (opt == "await") {
        auto spec = buildWaitSpec(arguments, sessionId, "none");
        spec.delivery = "none";
        auto timeoutMs = arguments.value<int64_t>("timeout_ms", kAwaitDefaultMs);
        if (timeoutMs <= 0) {
            timeoutMs = kAwaitDefaultMs;
        }
        if (timeoutMs > kAwaitMaxMs) {
            XX_LOGW(
                "[event_tool] await timeout_ms {} trimmed to the hard limit {}",
                timeoutMs,
                kAwaitMaxMs
            );
            timeoutMs     = kAwaitMaxMs;
        }
        spec.timeout = std::chrono::milliseconds{timeoutMs};
        auto cancelToken = getSessionCancelToken(agentContextPtr, arguments);
        auto result      = co_await hub->awaitOnce(std::move(spec), std::move(cancelToken));
        if (result.state == WaitState::TimedOut) {
            co_return agentxx::util::ToolResult::ok(agentxx::events::renderAwaitTimeout(result.spec, timeoutMs));
        }
        if (result.state == WaitState::Canceled) {
            throw neograph::graph::CancelledException{"event await Canceled"};
        }
        co_return agentxx::util::ToolResult::ok(renderSatisfiedText(*hub, result));
    }

    if (opt == "status") {
        // `id`: 查某条后台任务
        if (!asyncId.empty()) {
            const auto entry = hostOf()->asyncWorkJson(asyncId);
            if (!entry.is_object() || entry.empty()) {
                throw std::invalid_argument{fmt::format("Background task not found: {}", asyncId)};
            }
            const auto state = entry.value("state", std::string{});
            auto       out   = fmt::format(
                "[Async work status] {} kind={} name={} state={}",
                asyncId,
                entry.value("kind", std::string{}),
                entry.value("name", std::string{}),
                state
            );
            if (state == "running" || state == "done" || state == "failed"
                || state == "canceled") {
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch()
                )
                                     .count();
                const auto start  = entry.value("startedAtMs", int64_t{0});
                const auto finish = entry.value("finishedAtMs", int64_t{0});
                out += fmt::format(" elapsed={}", formatElapsed((finish > 0 ? finish : now) - start));
            }
            if (entry.value("hasError", false)) {
                out += " hasError=true";
            }
            if (entry.value("canceled", false)) {
                out += " canceled=true";
            }
            const auto error = entry.value("error", std::string{});
            if (!error.empty()) {
                out += fmt::format("\nerror: {}", error);
            }
            const auto result = entry.value("result", std::string{});
            if (!result.empty() && state != "running" && state != "queued") {
                out += fmt::format("\nresult:\n{}", result);
            }
            co_return agentxx::util::ToolResult::ok(out);
        }
        const auto waitId = arguments.value("wait", std::string{});
        if (!waitId.empty()) {
            auto wait = hub->queryWait(waitId);
            if (!wait.has_value()) {
                throw std::invalid_argument{fmt::format("Wait not found: {}", waitId)};
            }
            co_return agentxx::util::ToolResult::ok(agentxx::events::renderWaitStatus(*wait));
        }
        const auto eventName = arguments.value("event", std::string{});
        if (eventName.empty()) {
            throw std::invalid_argument{"Arg `wait` or `event` is required for opt=status"};
        }
        auto record = hub->queryEvent(eventName);
        if (!record.has_value()) {
            throw std::invalid_argument{fmt::format("Event not found: {}", eventName)};
        }
        co_return agentxx::util::ToolResult::ok(agentxx::events::renderEventStatus(*record));
    }

    if (opt == "list") {
        const auto scope  = arguments.value("scope", std::string{"all"});
        const auto prefix = arguments.value("prefix", std::string{});
        const auto rawLimit = arguments.value<int64_t>("limit", 20);
        const auto limit  = static_cast<size_t>(std::clamp<int64_t>(rawLimit, 1, 200));
        std::string out;
        if (scope == "events" || scope == "all") {
            out += renderEventList(*hub, prefix, limit);
        }
        if (scope == "waits" || scope == "all") {
            out += renderWaitList(*hub, sessionId, limit);
        }
        if (out.empty()) {
            throw std::invalid_argument{fmt::format("Arg `scope` is invalid: {}", scope)};
        }
        if (!out.empty() && out.back() == '\n') {
            out.pop_back();
        }
        co_return agentxx::util::ToolResult::ok(out);
    }

    if (opt == "cancel") {
        // `id`: 取消某条后台任务; `scope=mine`: 取消自己名下全部
        if (!asyncId.empty()) {
            if (!hostOf()->cancelAsyncWork(asyncId)) {
                throw std::invalid_argument{fmt::format(
                    "Background task not found or already finished: {}",
                    asyncId
                )};
            }
            co_return agentxx::util::ToolResult::ok(
                fmt::format("[Async work cancel] id={} canceled", asyncId)
            );
        }
        if (arguments.value("scope", std::string{}) == "mine") {
            const auto count = hostOf()->cancelAsyncWorks(agentContextPtr);
            co_return agentxx::util::ToolResult::ok(
                fmt::format("[Async work cancel] canceled {} task(s) of this agent", count)
            );
        }
        if (arguments.value("all", false)) {
            const auto count = hub->cancelWaitsOfSession(sessionId, "tool-cancel");
            co_return agentxx::util::ToolResult::ok(fmt::format("[Event cancel] Canceled {} wait(s) of this session", count));
        }
        const auto waitId = requireString(arguments, "wait");
        if (!hub->cancelWait(waitId, "tool-cancel")) {
            throw std::invalid_argument{fmt::format("Wait not found or already closed: {}", waitId)};
        }
        co_return agentxx::util::ToolResult::ok(fmt::format("[Event cancel] wait={} Canceled", waitId));
    }

    if (opt == "emit") {
        const auto eventName = requireWritableEventName(arguments);
        const bool single    = arguments.value("single", false);
        const bool create    = arguments.value("create", true);
        if (create && !hub->queryEvent(eventName).has_value()) {
            EventDefineSpec spec;
            spec.name        = eventName;
            spec.kind        = single ? EventKind::Single : EventKind::Stream;
            spec.owner       = "tool";
            spec.originSession = sessionId;
            spec.description = arguments.value("label", std::string{});
            std::string defineErr;
            if (!hub->define(spec, &defineErr)) {
                throw std::runtime_error{defineErr};
            }
        }
        auto value = arguments.contains("value") ? arguments["value"] : utilxx_base::Json::object();
        std::string err;
        if (!hub->publishValue(eventName, std::move(value), &err)) {
            throw std::runtime_error{err};
        }
        const auto record   = hub->queryEvent(eventName);
        const auto state    = record.has_value()
                                  ? (record->state == agentxx::events::EventState::Open ? "open"
                                        : record->state == agentxx::events::EventState::Ended
                                            ? "ended"
                                            : "expired")
                                  : "unknown";
        const auto published = record.has_value() ? record->published : uint64_t{0};
        const auto waits     = record.has_value() ? record->waitCount : size_t{0};
        co_return agentxx::util::ToolResult::ok(fmt::format(
            "[Event emit] event={} state={} published={} waits={}",
            eventName,
            state,
            published,
            waits
        ));
    }

    if (opt == "end") {
        const auto eventName = requireWritableEventName(arguments);
        auto summary = arguments.contains("summary") ? arguments["summary"]
                                                     : utilxx_base::Json::object();
        std::string err;
        if (!hub->endEvent(eventName, std::move(summary), &err)) {
            throw std::invalid_argument{err};
        }
        const auto published = [&]() -> uint64_t {
            auto record = hub->queryEvent(eventName);
            return record.has_value() ? record->published : 0;
        }();
        if (arguments.value("remove", false)) {
            hub->removeEvent(eventName);
            co_return agentxx::util::ToolResult::ok(fmt::format(
                "[Event end] event={} published={} removed=true",
                eventName,
                published
            ));
        }
        co_return agentxx::util::ToolResult::ok(
            fmt::format("[Event end] event={} state=ended published={}", eventName, published)
        );
    }

    throw std::invalid_argument{fmt::format("Arg `opt` is invalid: {}", opt)};
}

} // namespace tools
} // namespace agentxx
