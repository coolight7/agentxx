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
#include <algorithm>
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
constexpr std::string_view kAsyncDoneEvent = agentxx::events::naming::kAgentAsyncDoneEvent;
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

/// 延时毫秒 → 可读文本 (`1.5s` / `2m10s` / `1h02m`)
std::string formatDelay(int64_t ms) {
    if (ms <= 0) {
        return "0s";
    }
    if (ms < 60 * 1000) {
        return fmt::format("{:.1f}s", static_cast<double>(ms) / 1000.0);
    }
    if (ms < 3600 * 1000) {
        const auto totalSeconds = ms / 1000;
        return fmt::format("{}m{:02d}s", totalSeconds / 60, totalSeconds % 60);
    }
    const auto totalMinutes = ms / (60 * 1000);
    return fmt::format("{}h{:02d}m", totalMinutes / 60, totalMinutes % 60);
}

/// 排队中的条目还差多久起跑 (毫秒; > 0 = 还在延时)
int64_t delayRemainingMs(const Json& entry) {
    return entry.value("delayRemainingMs", int64_t{0});
}

/// 目标工具是否在创建者身上可用
///
/// 两个来源都算: 内置/中间件工具由执行图引擎持有 (见
/// `AgentContext::builtinToolIndex`), 插件与动态注册的工具在工具注册表里。
/// 引擎弱引用锁不上时说明创建者已释放, 内置索引不能再信。
bool creatorHasTool(
    const std::shared_ptr<agentxx::agent::AgentContext>& ctx,
    std::string_view                                     toolName
) {
    if (!ctx || toolName.empty()) {
        return false;
    }
    if (ctx->engine.lock()) {
        if (auto it = ctx->builtinToolIndex.find(toolName);
            it != ctx->builtinToolIndex.end()) {
            return it->second != nullptr;
        }
    }
    return ctx->toolRegistry && ctx->toolRegistry->contains(toolName);
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
    // 已跑时长只在真跑过时给 (排队中终止的任务 startedAtMs = 0)
    if (state == "running") {
        out += fmt::format(" elapsed={}", formatElapsed(nowMs() - start));
    } else if (isTerminalState(state)) {
        if (start > 0) {
            out += fmt::format(" elapsed={}", formatElapsed(finish - start));
        }
    } else if (state == "queued") {
        // 延时中: 还给模型一个"还有多久"的定量信息 (原地不动就是排队)
        const auto remaining = delayRemainingMs(entry);
        out += fmt::format(
            " ahead={}{}",
            entry.value("queueAhead", int64_t{0}),
            remaining > 0 ? fmt::format(" delay={} left", formatDelay(remaining))
                          : std::string{}
        );
    }
    if (entry.value("hasError", false)) {
        out += " hasError=true";
    }
    // 两个任务侧时限只在设了的时候给 (0 = 不限, 不占位置)
    if (entry.contains("queueTimeoutMs")) {
        out += fmt::format(
            " queueTimeout={}",
            formatDelay(entry.value("queueTimeoutMs", int64_t{0}))
        );
    }
    if (entry.contains("runTimeoutMs")) {
        out += fmt::format(
            " runTimeout={}",
            formatDelay(entry.value("runTimeoutMs", int64_t{0}))
        );
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
            // 延时中的任务用 `*` 开头标记 (与普通排队项区分)
            const auto remaining = delayRemainingMs(item);
            lines.push_back(fmt::format(
                "{} {} [queued]  {} {} ahead={}{}",
                remaining > 0 ? "*" : "-",
                id,
                kind,
                name,
                item.value("queueAhead", int64_t{0}),
                remaining > 0 ? fmt::format(" delay={} left", formatDelay(remaining))
                              : std::string{}
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
// agentxx_async_run
// ===========================================================================

AsyncRunTool::AsyncRunTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext) :
    XXToolBase(
        "agentxx_async_run",
        in_agentContext,
        /*autoSummaryOutput=*/false,
        /*canDelayLoad=*/false,
        /*maxRetry=*/0,
        /*repeatCallCheck=*/false,
        /*supportsParallel=*/false
    ) {}

neograph::ChatTool AsyncRunTool::get_definition() const {
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
                {
                    "delayMs",
                    {
                        {"type", "integer"},
                        {"description", prompt.getArg("delayMs")},
                    },
                },
                {
                    "queueTimeoutMs",
                    {
                        {"type", "integer"},
                        {"description", prompt.getArg("queueTimeoutMs")},
                    },
                },
                {
                    "runTimeoutMs",
                    {
                        {"type", "integer"},
                        {"description", prompt.getArg("runTimeoutMs")},
                    },
                },
            }, },
        {"required", Json::array({"tool"})},
    };
    return {get_name(), prompt.depict, agentxx::util::toNeographJson(params)};
}

asio::awaitable<agentxx::util::ToolResult> AsyncRunTool::execute_async(const Json& arguments) {
    const auto args  = arguments.is_object() ? arguments : Json::object();
    auto       state = acquire(agentContext.lock(), args);

    const auto toolName = args.value("tool", std::string{});
    if (toolName.empty()) {
        throw std::invalid_argument{"Arg `tool` is required and must be a non-empty string"};
    }
    // 只拦"把本工具自己套进去": 那样只会变成无意义的递归, 且外层立即返回、内层再提交一条
    // 同样的任务, 模型看不到任何有用信息。其余工具 (含其他后台任务管理工具、内置工具、
    // 插件工具、子代理) 都允许异步启动
    if (toolName == get_name()) {
        throw std::invalid_argument{
            fmt::format("Arg `tool` must not be `{}` itself (that only nests the same call)",
                        toolName)
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
    // 起跑延时 (毫秒; <= 0 = 不延时): 到点之前任务一直是排队中
    auto delayMs = args.value<int64_t>("delayMs", 0);
    if (!args.contains("delayMs")) {
        delayMs = args.value<int64_t>("delay_ms", delayMs); // 兼容下划线写法
    }
    if (delayMs < 0) {
        delayMs = 0;
    }
    // 两个任务侧时限 (毫秒; <= 0 = 不限制), 与等待侧的 timeoutMs 不是一回事
    auto queueTimeoutMs = args.value<int64_t>("queueTimeoutMs", 0);
    if (!args.contains("queueTimeoutMs")) {
        queueTimeoutMs = args.value<int64_t>("queue_timeout_ms", queueTimeoutMs);
    }
    auto runTimeoutMs = args.value<int64_t>("runTimeoutMs", 0);
    if (!args.contains("runTimeoutMs")) {
        runTimeoutMs = args.value<int64_t>("run_timeout_ms", runTimeoutMs);
    }
    queueTimeoutMs = std::max<int64_t>(0, queueTimeoutMs);
    runTimeoutMs   = std::max<int64_t>(0, runTimeoutMs);

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
    spec.owner            = "tool:agentxx_async_run";
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
        // 普通工具: 内置/中间件工具 (图引擎持有) 与插件/动态注册工具都行
        if (!creatorHasTool(state.ctx, toolName)) {
            throw std::invalid_argument{fmt::format(
                "tool `{}` is not available to this agent (not a built-in tool and not in the "
                "dynamic tool registry)",
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
    spec.delay        = std::chrono::milliseconds{delayMs};
    spec.queueTimeout = std::chrono::milliseconds{queueTimeoutMs};
    spec.runTimeout   = std::chrono::milliseconds{runTimeoutMs};

    auto submitted = state.host->submitAsyncWork(std::move(spec), state.ctx);
    if (!submitted.accepted) {
        throw std::runtime_error{fmt::format("background task rejected: {}", submitted.error)};
    }

    std::string out = fmt::format("[Async started] id={} {}\n", submitted.asyncId, startedAs);
    if (submitted.delayMs > 0) {
        out += fmt::format(
            "Starts in {} (delayMs={}): the task stays queued until then. Use "
            "agentxx_async_manager {{opt:\"delay\", id:[\"{}\"], delayMs:0}} to drop the "
            "remaining delay.\n",
            formatDelay(submitted.delayMs),
            submitted.delayMs,
            submitted.asyncId
        );
    }
    if (notify) {
        out += "The result will be inserted into this conversation as an input when it is done.\n";
    } else {
        out += "No automatic notification (notify=false): the result stays in the task ledger.\n";
    }
    // 设了时限就说清各自的计时起点 (两个都是任务侧时限, 与 await 的 timeoutMs 不同)
    if (queueTimeoutMs > 0) {
        out += fmt::format(
            "Queue limit: {} counted from the moment the delay ends (or from now when there "
            "is no delay).\n",
            formatDelay(queueTimeoutMs)
        );
    }
    if (runTimeoutMs > 0) {
        out += fmt::format(
            "Run limit: {} counted from the moment it starts.\n",
            formatDelay(runTimeoutMs)
        );
    }
    out += fmt::format(
        "Check with agentxx_async_query {{id:\"{}\"}} or {{group:\"running\"}}, block for it with "
        "agentxx_async_await {{id:\"{}\"}}, cancel with agentxx_async_manager "
        "{{opt:\"cancel\", id:[\"{}\"]}}.",
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
        // 返回的是任务结果正文 (与自动投递同一份渲染): 子代理回答可能很长,
        // 工具任务的结果也可能没被工具自己裁剪过 —— 按同一个阈值转 share store
        /*autoSummaryOutput=*/true,
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
                    "timeoutMs",
                    {
                        {"type", "integer"},
                        {"description", prompt.getArg("timeoutMs")},
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
    auto entry = requireOwnedEntry(state.host, asyncId, state.sessionId);
    if (isTerminalState(entry.value("state", std::string{}))) {
        co_return agentxx::util::ToolResult::ok(renderEntryDetail(entry));
    }

    auto timeoutMs = args.value<int64_t>("timeoutMs", 0);
    if (timeoutMs <= 0) {
        // 兼容旧的下划线写法; 都没给就用默认值
        timeoutMs = args.value<int64_t>("timeout_ms", kAwaitDefaultMs);
    }
    if (timeoutMs <= 0) {
        timeoutMs = kAwaitDefaultMs;
    }
    if (timeoutMs > kAwaitMaxMs) {
        timeoutMs = kAwaitMaxMs;
    }

    // 剩余延时比这次等待还长: 挂上去只会必然超时, 直接给说明文本
    // (要提前起跑用 `agentxx_async_manager {opt:"delay", delayMs:0}`)
    if (const auto remaining = delayRemainingMs(entry); remaining > 0 && remaining > timeoutMs) {
        co_return agentxx::util::ToolResult::ok(fmt::format(
            "{}\nThis task is still waiting for its start delay ({}, submitted delayMs={}). "
            "It will not start before then; call agentxx_async_manager "
            "{{opt:\"delay\", id:[\"{}\"], delayMs:0}} to drop the remaining delay, or use a "
            "larger timeoutMs.",
            renderEntryDetail(entry),
            formatDelay(remaining),
            entry.value("delayMs", int64_t{0}),
            asyncId
        ));
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
// agentxx_async_manager
// ===========================================================================

namespace {

/// `id` 参数 → id 列表
/// - 声明是字符串数组: 模型给单个字符串时由执行层按 schema 自愈成数组
/// - 空串与重复项丢弃
std::vector<std::string> parseIdList(const Json& args) {
    std::vector<std::string> ids;
    const auto               collect = [&ids](const Json& item) {
        if (!item.is_string()) {
            return;
        }
        auto value = item.get<std::string>();
        if (value.empty() || std::find(ids.begin(), ids.end(), value) != ids.end()) {
            return;
        }
        ids.push_back(std::move(value));
    };
    if (auto it = args.find("id"); it != args.end()) {
        if (it->is_array()) {
            for (const auto& item : *it) {
                collect(item);
            }
        }
    }
    return ids;
}

/// 整数参数 (`camelCase` 优先, 兼容 `snake_case` 写法)
int64_t
    intArg(const Json& args, std::string_view camel, std::string_view snake, int64_t fallback) {
    if (args.contains(camel)) {
        return args.value<int64_t>(camel, fallback);
    }
    if (!snake.empty() && args.contains(snake)) {
        return args.value<int64_t>(snake, fallback);
    }
    return fallback;
}

} // namespace

AsyncManagerTool::AsyncManagerTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext) :
    XXToolBase(
        "agentxx_async_manager",
        in_agentContext,
        /*autoSummaryOutput=*/false,
        /*canDelayLoad=*/false,
        /*maxRetry=*/0,
        /*repeatCallCheck=*/false,
        /*supportsParallel=*/false
    ) {}

neograph::ChatTool AsyncManagerTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    Json params = Json{
        {"type", "object"},
        {
         "properties", {
                {
                    "opt",
                    {
                        {"type", "string"},
                        {"enum", Json::array({"cancel", "delay"})},
                        {"description", prompt.getArg("opt")},
                    },
                },
                {
                    "id",
                    {
                        {"type", "array"},
                        {"items", Json{{"type", "string"}}},
                        {"description", prompt.getArg("id")},
                    },
                },
                {
                    "delayMs",
                    {
                        {"type", "integer"},
                        {"description", prompt.getArg("delayMs")},
                    },
                },
            }, },
        {"required", Json::array({"opt", "id"})},
    };
    return {get_name(), prompt.depict, agentxx::util::toNeographJson(params)};
}

asio::awaitable<agentxx::util::ToolResult>
    AsyncManagerTool::execute_async(const Json& arguments) {
    const auto args  = arguments.is_object() ? arguments : Json::object();
    auto       state = acquire(agentContext.lock(), args);

    const auto opt = args.value("opt", std::string{});
    if (opt != "cancel" && opt != "delay") {
        throw std::invalid_argument{
            fmt::format("Arg `opt` must be one of cancel|delay, got `{}`", opt)
        };
    }
    const auto ids = parseIdList(args);
    if (ids.empty()) {
        throw std::invalid_argument{
            "Arg `id` is required: an array of task ids (a single id is turned into an array "
            "by the executor)"
        };
    }
    int64_t delayMs = 0;
    if (opt == "delay") {
        if (!args.contains("delayMs") && !args.contains("delay_ms")) {
            throw std::invalid_argument{
                "Arg `delayMs` is required when `opt` is delay (0 removes the delay)"
            };
        }
        delayMs = intArg(args, "delayMs", "delay_ms", 0);
        if (delayMs < 0) {
            throw std::invalid_argument{"Arg `delayMs` must be >= 0 (0 removes the delay)"};
        }
    }

    std::vector<std::string> lines;
    size_t                   okCount = 0;
    for (const auto& asyncId : ids) {
        // 归属与状态先看台账: 不存在 / 别会话的任务逐条报, 不打断整批
        Json entry;
        try {
            entry = requireOwnedEntry(state.host, asyncId, state.sessionId);
        } catch (const std::exception& e) {
            lines.push_back(fmt::format("- {}: {}", asyncId, e.what()));
            continue;
        }
        const auto stateText = entry.value("state", std::string{});
        if (isTerminalState(stateText)) {
            lines.push_back(fmt::format("- {}: already finished (state={})", asyncId, stateText));
            continue;
        }
        if (opt == "cancel") {
            if (!state.host->cancelAsyncWork(asyncId)) {
                lines.push_back(fmt::format("- {}: cancel request failed", asyncId));
                continue;
            }
            ++okCount;
            lines.push_back(fmt::format(
                "- {}: canceled ({})",
                asyncId,
                stateText == "queued" ? "was queued, removed from the queue"
                                      : "running, cancel requested"
            ));
            continue;
        }
        // opt == delay: 只有排队中的任务能改延时
        if (stateText != "queued") {
            lines.push_back(fmt::format(
                "- {}: not queued (state={}), the delay cannot be changed once it runs",
                asyncId,
                stateText
            ));
            continue;
        }
        if (!state.host->setAsyncWorkDelay(asyncId, delayMs)) {
            lines.push_back(fmt::format("- {}: delay change failed", asyncId));
            continue;
        }
        ++okCount;
        lines.push_back(
            delayMs > 0
                ? fmt::format("- {}: delay set to {}", asyncId, formatDelay(delayMs))
                : fmt::format("- {}: delay removed, it can start now", asyncId)
        );
    }

    std::string out
        = fmt::format("[Async manager] opt={} ok={}/{}", opt, okCount, ids.size());
    if (opt == "delay") {
        out += fmt::format(" delayMs={}", delayMs);
    }
    out += "\n";
    for (const auto& line : lines) {
        out += line;
        out += "\n";
    }
    out.pop_back();
    if (okCount == 0) {
        // 一条都没成功: 整体按失败报 (正文里逐条写原因)
        co_return agentxx::util::ToolResult::fail(std::move(out));
    }
    co_return agentxx::util::ToolResult::ok(std::move(out));
}

} // namespace tools
} // namespace agentxx
