#include "agentxx/tools/async_tools.h"
#include "agentxx/util/neograph_json_bridge.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/async_work.h"
#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
#include "agentxx/event/hub.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/util/tool_result.h"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace tools {

namespace {

using utilxx_base::Json;

/// 后台任务的结果事件名 (单一事件名, 靠值里的 `asyncId` 区分)
constexpr std::string_view kAsyncDoneEvent = "agentxx.agent.async.done";
/// 中枢里这类等待的归属标记
constexpr std::string_view kAsyncOwner = "async";

/// `await` 默认与硬上限 (毫秒): 它会让本轮停在这里, 必须有界
constexpr int64_t kAwaitDefaultMs = 30 * 1000;
constexpr int64_t kAwaitMaxMs     = 5 * 60 * 1000;

/// 一次列出的上限 (再多也只是刷屏, 模型按 `id` 查单条更有效)
constexpr size_t kQueryLimit = 50;

/// 取会话 id (由 ToolCallNode 注入; 缺失时报错)
std::string requireSessionId(const Json& args) {
    auto sessionId = args.value("sessionId", std::string{});
    if (sessionId.empty()) {
        throw std::invalid_argument{"Toolcall inner error, need `sessionId`"};
    }
    return sessionId;
}

/// 毫秒 → `mm:ss` / `h:mm:ss` (耗时展示)
std::string formatElapsed(int64_t ms) {
    if (ms < 0) {
        ms = 0;
    }
    const auto totalSeconds = ms / 1000;
    if (totalSeconds >= 3600) {
        return fmt::format(
            "{}:{:02d}:{:02d}",
            totalSeconds / 3600,
            (totalSeconds % 3600) / 60,
            totalSeconds % 60
        );
    }
    return fmt::format("{:02d}:{:02d}", totalSeconds / 60, totalSeconds % 60);
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()
    )
        .count();
}

bool isTerminalState(std::string_view state) {
    return state == "done" || state == "failed" || state == "canceled";
}

/// 状态文本 → 给模型看的标记
std::string_view stateMark(std::string_view state) {
    if (state == "done") {
        return "ok";
    }
    if (state == "failed") {
        return "failed";
    }
    if (state == "canceled") {
        return "canceled";
    }
    return state;
}

/// 单条任务详情 (台账 JSON → 文本; 含结果正文)
std::string renderEntryDetail(const Json& entry) {
    const auto  state  = entry.value("state", std::string{});
    const auto  start  = entry.value("startedAtMs", int64_t{0});
    const auto  finish = entry.value("finishedAtMs", int64_t{0});
    std::string out    = fmt::format(
        "[Async task] id={} kind={} name={} state={}",
        entry.value("asyncId", std::string{}),
        entry.value("kind", std::string{}),
        entry.value("name", std::string{}),
        state
    );
    if (state == "running") {
        out += fmt::format(" elapsed={}", formatElapsed(nowMs() - start));
    } else if (isTerminalState(state)) {
        out += fmt::format(" elapsed={}", formatElapsed(finish - start));
    }
    if (entry.value("hasError", false)) {
        out += " hasError=true";
    }
    if (entry.value("canceled", false)) {
        out += " canceled=true";
    }
    if (!entry.value("delivered", false)) {
        out += " (result not delivered to this session)";
    }
    const auto error = entry.value("error", std::string{});
    if (!error.empty()) {
        out += fmt::format("\nerror: {}", error);
    }
    const auto result = entry.value("result", std::string{});
    if (!result.empty() && state != "queued" && state != "running") {
        out += fmt::format("\nresult:\n{}", result);
    }
    return out;
}

/// 任务清单 (台账 JSON 数组 → 文本)
std::string renderTaskList(std::string_view group, const Json& items) {
    size_t                   running  = 0;
    size_t                   queued   = 0;
    size_t                   done     = 0;
    size_t                   failed   = 0;
    size_t                   canceled = 0;
    std::vector<std::string> lines;
    size_t                   matched = 0;
    for (const auto& item : items) {
        const auto state = item.value("state", std::string{});
        if (state == "running") {
            ++running;
        } else if (state == "queued") {
            ++queued;
        } else if (state == "done") {
            ++done;
        } else if (state == "failed") {
            ++failed;
        } else if (state == "canceled") {
            ++canceled;
        }
        if (group != "all" && state != group) {
            continue;
        }
        if (matched >= kQueryLimit) {
            continue;
        }
        ++matched;
        const auto id   = item.value("asyncId", std::string{});
        const auto kind = item.value("kind", std::string{});
        const auto name = item.value("name", std::string{});
        if (state == "running") {
            lines.push_back(fmt::format(
                "- {} [running] {} {} elapsed={}",
                id,
                kind,
                name,
                formatElapsed(nowMs() - item.value("startedAtMs", int64_t{0}))
            ));
        } else if (state == "queued") {
            lines.push_back(fmt::format(
                "- {} [queued]  {} {} ahead={}",
                id,
                kind,
                name,
                item.value("queueAhead", int64_t{0})
            ));
        } else {
            const auto start  = item.value("startedAtMs", int64_t{0});
            const auto finish = item.value("finishedAtMs", int64_t{0});
            lines.push_back(fmt::format(
                "- {} [{}] {} {} elapsed={}",
                id,
                stateMark(state),
                kind,
                name,
                formatElapsed(finish - start)
            ));
            const auto error = item.value("error", std::string{});
            if (!error.empty()) {
                lines.push_back(fmt::format("    error: {}", error));
            }
        }
    }

    std::string out = fmt::format(
        "[Async tasks] group={} matched={} (running={} queued={} done={} failed={} canceled={})\n",
        group,
        matched,
        running,
        queued,
        done,
        failed,
        canceled
    );
    if (lines.empty()) {
        out += "(no background task matches the filter)";
        return out;
    }
    for (const auto& line : lines) {
        out += line;
        out += "\n";
    }
    out.pop_back();
    return out;
}

/// 台账里取本会话的一条任务; 不存在或属别的会话时按参数错误拒绝
Json requireOwnedEntry(
    const std::shared_ptr<agentxx::agent::AgentHost>& host,
    std::string_view                                  asyncId,
    std::string_view                                  sessionId
) {
    auto entry = host->asyncWorkJson(asyncId);
    if (!entry.is_object() || entry.empty()) {
        throw std::invalid_argument{fmt::format(
            "Background task not found: {} (finished tasks are kept for a while, see config "
            "`async_work.linger_ms`)",
            asyncId
        )};
    }
    if (entry.value("creatorSessionId", std::string{}) != sessionId) {
        throw std::invalid_argument{
            fmt::format("Background task `{}` was not started by this session", asyncId)
        };
    }
    return entry;
}

/// 容器 + 上下文 (后台任务族共用前置检查)
struct AsyncCtx {
    std::shared_ptr<agentxx::agent::AgentContext> ctx;
    std::shared_ptr<agentxx::agent::AgentHost>    host;
    std::string                                   sessionId;
};

AsyncCtx
    acquire(const std::shared_ptr<agentxx::agent::AgentContext>& ctxPtr, const Json& arguments) {
    AsyncCtx out;
    out.ctx = ctxPtr;
    if (!ctxPtr) {
        throw std::runtime_error{"AgentContext not available"};
    }
    out.sessionId = requireSessionId(arguments);
    out.host      = ctxPtr->host.lock();
    if (!out.host) {
        throw std::runtime_error{"background tasks need an agent host (this agent has no container)"
        };
    }
    return out;
}

} // namespace

// ===========================================================================
// agentxx_async_start
// ===========================================================================

AsyncStartTool::AsyncStartTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext) :
    XXToolBase(
        "agentxx_async_start",
        in_agentContext,
        /*autoSummaryOutput=*/false,
        /*canDelayLoad=*/false,
        /*maxRetry=*/0,
        /*repeatCallCheck=*/false,
        /*supportsParallel=*/false
    ) {}

neograph::ChatTool AsyncStartTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    Json params = Json{
        {"type", "object"},
        {
         "properties", {
                {
                    "tool",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("tool")},
                    },
                },
                {
                    "args",
                    {
                        {"type", "object"},
                        {"description", prompt.getArg("args")},
                    },
                },
                {
                    "notify",
                    {
                        {"type", "boolean"},
                        {"description", prompt.getArg("notify")},
                    },
                },
                {
                    "label",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("label")},
                    },
                },
            }, },
        {"required", Json::array({"tool"})},
    };
    return {get_name(), prompt.depict, agentxx::util::toNeographJson(params)};
}

asio::awaitable<agentxx::util::ToolResult> AsyncStartTool::execute_async(const Json& arguments) {
    const auto args  = arguments.is_object() ? arguments : Json::object();
    auto       state = acquire(agentContext.lock(), args);

    const auto toolName = args.value("tool", std::string{});
    if (toolName.empty()) {
        throw std::invalid_argument{"Arg `tool` is required and must be a non-empty string"};
    }
    // 不给自己套自己, 也不套查询/等待/取消 (只会变成无意义的递归)
    if (toolName == get_name() || toolName == "agentxx_async_query"
        || toolName == "agentxx_async_await" || toolName == "agentxx_async_cancel") {
        throw std::invalid_argument{
            fmt::format("Arg `tool` must not be a background-task tool (`{}`)", toolName)
        };
    }
    Json toolArgs = Json::object();
    if (args.contains("args")) {
        if (!args["args"].is_object()) {
            throw std::invalid_argument{"Arg `args` must be an object"};
        }
        toolArgs = args["args"];
    }
    const bool notify = args.value("notify", true);
    const auto label  = args.value("label", std::string{});

    // 会话与调用 id 由执行层补齐 (与前台 ToolcallNode 一致): 下游工具会读它们
    const auto sessionId  = state.sessionId;
    toolArgs["sessionId"] = sessionId;
    const auto callId     = args.value("tool_call_id", std::string{});
    if (!callId.empty()) {
        toolArgs["tool_call_id"] = callId;
    }

    // 权限: 后台执行不再经过 ToolcallWrapNode, 因此在**提交前**按真正的目标工具与参数
    // 问一次 (与前台同一套权限中间件, 该询问用户时照旧询问)
    if (state.ctx->bus) {
        auto resp = co_await state.ctx->bus->request<
            agentxx::events::ReqToolPermissionCheck,
            agentxx::events::RespToolPermissionCheck>(
            agentxx::events::Topic::ToolPermissionCheck,
            agentxx::events::ReqToolPermissionCheck{
                .agentName
                = state.ctx->agentConfig ? state.ctx->agentConfig->agentName : std::string{},
                .sessionId = sessionId,
                .toolName  = toolName,
                .arguments = toolArgs,
            },
            std::chrono::milliseconds{0}
        );
        if (resp.has_value() && !resp->allow) {
            co_return agentxx::util::ToolResult::fail(
                resp->reason.empty() ? fmt::format("permission denied: `{}`", toolName)
                                     : fmt::format("permission denied: {}", resp->reason)
            );
        }
    }

    agentxx::agent::AsyncWorkSpec spec;
    spec.creatorAgentId   = state.ctx->agentId;
    spec.creatorSessionId = sessionId;
    spec.resultId         = callId;
    spec.owner            = "tool:agentxx_async_start";
    spec.baseConfig       = state.ctx->agentConfig;
    // 结果默认主动回上下文: 轮次在跑就插在下一次模型调用前, 已结束就开新一轮
    spec.notifyDelivery = notify ? std::string{"next-step"} : std::string{};
    spec.name           = label;

    std::string startedAs;
    if (toolName == "agentxx_subagent") {
        // 子代理: 参数与直接调用完全一致, 内部按 agent 任务提交
        const auto subagent = toolArgs.value("subagent", std::string{});
        if (subagent.empty()) {
            throw std::invalid_argument{"Arg `args.subagent` is required for `agentxx_subagent`"};
        }
        spec.kind    = agentxx::agent::AsyncWorkKind::Agent;
        spec.message = toolArgs.value("message", std::string{});
        if (toolArgs.contains("messages") && toolArgs["messages"].is_array()) {
            spec.messages = toolArgs["messages"];
        }
        if (spec.message.empty() && !spec.messages.is_array()) {
            throw std::invalid_argument{
                "Arg `args.message` (or `args.messages`) is required for `agentxx_subagent`"
            };
        }
        if (spec.name.empty()) {
            spec.name = subagent;
        }
        spec.systemPrompt = toolArgs.value("system_prompt", std::string{});
        if (toolArgs.contains("tools")) {
            spec.tools = toolArgs["tools"];
        }
        if (toolArgs.contains("enable_summarization")
            && toolArgs["enable_summarization"].is_boolean()) {
            spec.enableSummarization = toolArgs["enable_summarization"].get<bool>();
        }
        startedAs = fmt::format("subagent={}", subagent);
    } else {
        // 普通工具: 必须在工具注册表里 (插件 / 动态注册; lib 内置工具由引擎独立持有)
        if (!state.ctx->toolRegistry || !state.ctx->toolRegistry->find(toolName)) {
            throw std::invalid_argument{fmt::format(
                "tool `{}` is not in the tool registry (only plugin / dynamically registered "
                "tools can run in the background; call built-in tools directly)",
                toolName
            )};
        }
        spec.kind     = agentxx::agent::AsyncWorkKind::Tool;
        spec.toolName = toolName;
        spec.toolArgs = toolArgs;
        if (spec.name.empty()) {
            spec.name = toolName;
        }
        startedAs = fmt::format("tool={}", toolName);
    }

    auto submitted = state.host->submitAsyncWork(std::move(spec), state.ctx);
    if (!submitted.accepted) {
        throw std::runtime_error{fmt::format("background task rejected: {}", submitted.error)};
    }

    std::string out = fmt::format("[Async started] id={} {}\n", submitted.asyncId, startedAs);
    if (notify) {
        out += "The result will be inserted into this conversation as an input when it is done.\n";
    } else {
        out += "No automatic notification (notify=false): the result stays in the task ledger.\n";
    }
    out += fmt::format(
        "Check with agentxx_async_query {{id:\"{}\"}} or {{group:\"running\"}}, block for it with "
        "agentxx_async_await {{id:\"{}\"}}, cancel with agentxx_async_cancel {{id:\"{}\"}}.",
        submitted.asyncId,
        submitted.asyncId,
        submitted.asyncId
    );
    co_return agentxx::util::ToolResult::ok(std::move(out));
}

// ===========================================================================
// agentxx_async_query
// ===========================================================================

AsyncQueryTool::AsyncQueryTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext) :
    XXToolBase(
        "agentxx_async_query",
        in_agentContext,
        /*autoSummaryOutput=*/false,
        /*canDelayLoad=*/false,
        /*maxRetry=*/0,
        /*repeatCallCheck=*/false,
        /*supportsParallel=*/true
    ) {}

neograph::ChatTool AsyncQueryTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    Json params = Json{
        {"type", "object"},
        {
         "properties", {
                {
                    "group",
                    {
                        {"type", "string"},
                        {"enum",
                         Json::array({
                             "all",
                             "queued",
                             "running",
                             "done",
                             "failed",
                             "canceled",
                         })},
                        {"description", prompt.getArg("group")},
                    },
                },
                {
                    "id",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("id")},
                    },
                },
            }, },
        {"required", Json::array()},
    };
    return {get_name(), prompt.depict, agentxx::util::toNeographJson(params)};
}

asio::awaitable<agentxx::util::ToolResult> AsyncQueryTool::execute_async(const Json& arguments) {
    const auto args  = arguments.is_object() ? arguments : Json::object();
    auto       state = acquire(agentContext.lock(), args);

    const auto asyncId = args.value("id", std::string{});
    const auto group   = args.value("group", std::string{"all"});

    if (!asyncId.empty()) {
        co_return agentxx::util::ToolResult::ok(
            renderEntryDetail(requireOwnedEntry(state.host, asyncId, state.sessionId))
        );
    }

    static constexpr std::string_view kGroups[] = {
        "all",
        "queued",
        "running",
        "done",
        "failed",
        "canceled",
    };
    bool known = false;
    for (const auto& candidate : kGroups) {
        if (group == candidate) {
            known = true;
            break;
        }
    }
    if (!known) {
        throw std::invalid_argument{fmt::format(
            "Arg `group` must be one of all|queued|running|done|failed|canceled, got `{}`",
            group
        )};
    }

    // 本会话的任务 (含已结束的): 台账按会话过滤, 状态过滤在本工具里做
    auto items = state.host->asyncWorksJson({}, state.sessionId, /*includeFinished=*/true, 200);
    co_return agentxx::util::ToolResult::ok(renderTaskList(group, items));
}

// ===========================================================================
// agentxx_async_await
// ===========================================================================

AsyncAwaitTool::AsyncAwaitTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext) :
    XXToolBase(
        "agentxx_async_await",
        in_agentContext,
        /*autoSummaryOutput=*/false,
        /*canDelayLoad=*/false,
        /*maxRetry=*/0,
        /*repeatCallCheck=*/false,
        /*supportsParallel=*/false
    ) {}

neograph::ChatTool AsyncAwaitTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    Json params = Json{
        {"type", "object"},
        {
         "properties", {
                {
                    "id",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("id")},
                    },
                },
                {
                    "timeout_ms",
                    {
                        {"type", "integer"},
                        {"description", prompt.getArg("timeout_ms")},
                    },
                },
            }, },
        {"required", Json::array({"id"})},
    };
    return {get_name(), prompt.depict, agentxx::util::toNeographJson(params)};
}

asio::awaitable<agentxx::util::ToolResult> AsyncAwaitTool::execute_async(const Json& arguments) {
    const auto args  = arguments.is_object() ? arguments : Json::object();
    auto       state = acquire(agentContext.lock(), args);

    const auto asyncId = args.value("id", std::string{});
    if (asyncId.empty()) {
        throw std::invalid_argument{"Arg `id` is required"};
    }
    auto hub = state.ctx->eventHub;
    if (!hub) {
        throw std::runtime_error{"event hub is not initialised"};
    }

    // 已结束 (或已被 linger 清掉) 的任务直接看台账, 不再挂等待
    const auto entry = requireOwnedEntry(state.host, asyncId, state.sessionId);
    if (isTerminalState(entry.value("state", std::string{}))) {
        co_return agentxx::util::ToolResult::ok(renderEntryDetail(entry));
    }

    auto timeoutMs = args.value<int64_t>("timeout_ms", kAwaitDefaultMs);
    if (timeoutMs <= 0) {
        timeoutMs = kAwaitDefaultMs;
    }
    if (timeoutMs > kAwaitMaxMs) {
        timeoutMs = kAwaitMaxMs;
    }

    agentxx::events::EventWaitSpec spec;
    spec.event         = std::string{kAsyncDoneEvent};
    spec.originSession = state.sessionId;
    spec.owner         = std::string{kAsyncOwner};
    spec.ref           = asyncId;
    spec.mode          = "first";
    spec.delivery      = "none";
    spec.match         = Json{
                {"asyncId", asyncId}
    };
    spec.replay  = true;
    spec.timeout = std::chrono::milliseconds{timeoutMs};

    auto specCopy = spec;
    auto token    = getSessionCancelToken(state.ctx, args);
    auto wait     = co_await hub->awaitOnce(std::move(spec), std::move(token));
    if (wait.state == agentxx::events::WaitState::TimedOut) {
        co_return agentxx::util::ToolResult::ok(
            agentxx::events::renderAwaitTimeout(specCopy, timeoutMs)
        );
    }
    const auto content = hub->contentText(state.sessionId, wait.spec.event, wait.values);
    const std::string status
        = wait.values.empty() ? "ended-empty" : (wait.values.size() > 1 ? "values" : "value");
    co_return agentxx::util::ToolResult::ok(
        agentxx::events::renderEventText(wait, status, content)
    );
}

// ===========================================================================
// agentxx_async_cancel
// ===========================================================================

AsyncCancelTool::AsyncCancelTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext) :
    XXToolBase(
        "agentxx_async_cancel",
        in_agentContext,
        /*autoSummaryOutput=*/false,
        /*canDelayLoad=*/false,
        /*maxRetry=*/0,
        /*repeatCallCheck=*/false,
        /*supportsParallel=*/false
    ) {}

neograph::ChatTool AsyncCancelTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    Json params = Json{
        {"type", "object"},
        {
         "properties", {
                {
                    "id",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("id")},
                    },
                },
                {
                    "group",
                    {
                        {"type", "string"},
                        {"enum", Json::array({"all", "queued", "running"})},
                        {"description", prompt.getArg("group")},
                    },
                },
            }, },
        {"required", Json::array()},
    };
    return {get_name(), prompt.depict, agentxx::util::toNeographJson(params)};
}

asio::awaitable<agentxx::util::ToolResult> AsyncCancelTool::execute_async(const Json& arguments) {
    const auto args  = arguments.is_object() ? arguments : Json::object();
    auto       state = acquire(agentContext.lock(), args);

    const auto asyncId = args.value("id", std::string{});
    if (!asyncId.empty()) {
        const auto entry = requireOwnedEntry(state.host, asyncId, state.sessionId);
        if (isTerminalState(entry.value("state", std::string{}))) {
            throw std::invalid_argument{fmt::format(
                "Background task `{}` already finished (state={})",
                asyncId,
                entry.value("state", std::string{})
            )};
        }
        if (!state.host->cancelAsyncWork(asyncId)) {
            throw std::runtime_error{fmt::format("failed to cancel `{}`", asyncId)};
        }
        co_return agentxx::util::ToolResult::ok(
            fmt::format("[Async cancel] id={} canceled", asyncId)
        );
    }

    const auto group = args.value("group", std::string{"all"});
    if (group != "all" && group != "queued" && group != "running") {
        throw std::invalid_argument{fmt::format(
            "Arg `group` must be one of all|queued|running (finished tasks cannot be canceled), "
            "got `{}`",
            group
        )};
    }
    if (group == "all") {
        const auto count = state.host->cancelAsyncWorksOfSession(state.sessionId);
        co_return agentxx::util::ToolResult::ok(
            fmt::format("[Async cancel] canceled {} unfinished task(s) of this session", count)
        );
    }
    // 只取消指定状态的: 逐条按台账状态筛, 再单条取消 (排队中的会直接出队)
    auto   items  = state.host->asyncWorksJson({}, state.sessionId, /*includeFinished=*/false, 200);
    size_t tried  = 0;
    size_t failed = 0;
    for (const auto& item : items) {
        if (item.value("state", std::string{}) != group) {
            continue;
        }
        const auto id = item.value("asyncId", std::string{});
        if (id.empty()) {
            continue;
        }
        ++tried;
        if (!state.host->cancelAsyncWork(id)) {
            ++failed;
        }
    }
    co_return agentxx::util::ToolResult::ok(fmt::format(
        "[Async cancel] canceled {}/{} {} task(s) of this session{}",
        tried - failed,
        tried,
        group,
        failed > 0 ? fmt::format(" ({} failed)", failed) : std::string{}
    ));
}

} // namespace tools
} // namespace agentxx
