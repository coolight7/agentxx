#include "agentxx/tools/tool_async.h"

#include "agentxx/agent/agent_host.h"
#include "agentxx/agent/async_work.h"
#include "agentxx/agent/context.h"
#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
#include "agentxx/plugin/tool_registry.h"
#include "agentxx/util/neograph_json_bridge.h"
#include "agentxx/util/tool_result.h"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>

namespace agentxx {
namespace tools {

namespace {

using utilxx_base::Json;

/// 取会话 id (由 ToolCallNode 注入; 缺失时报错)
std::string requireSessionId(const Json& args) {
    auto sessionId = args.value("sessionId", std::string{});
    if (sessionId.empty()) {
        throw std::invalid_argument{"Toolcall inner error, need `sessionId`"};
    }
    return sessionId;
}

} // namespace

ToolAsyncTool::ToolAsyncTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext) :
    XXToolBase(
        "agentxx_tool_async",
        in_agentContext,
        /*autoSummaryOutput=*/false,
        /*canDelayLoad=*/false,
        /*maxRetry=*/0,
        /*repeatCallCheck=*/false,
        /*supportsParallel=*/false
    ) {}

neograph::ChatTool ToolAsyncTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    Json params = Json{
        {"type", "object"},
        {
         "properties",
         {
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
                    "label",
                    {
                        {"type", "string"},
                        {"description", prompt.getArg("label")},
                    },
                },
            },
        },
        {"required", Json::array({"tool"})},
    };

    return {get_name(), prompt.depict, agentxx::util::toNeographJson(params)};
}

asio::awaitable<agentxx::util::ToolResult>
    ToolAsyncTool::execute_async(const Json& arguments) {
    const auto args      = arguments.is_object() ? arguments : Json::object();
    const auto sessionId = requireSessionId(args);

    const auto toolName = args.value("tool", std::string{});
    if (toolName.empty()) {
        throw std::invalid_argument{"Arg `tool` is required and must be a non-empty string"};
    }
    // 不给自己套自己 (后台再开后台会无限长链): 提示词里已写明不可嵌套
    if (toolName == get_name()) {
        throw std::invalid_argument{
            "Arg `tool` must not be `agentxx_tool_async` itself (no nested background submission)"
        };
    }
    Json toolArgs = Json::object();
    if (args.contains("args")) {
        if (!args["args"].is_object()) {
            throw std::invalid_argument{"Arg `args` must be an object"};
        }
        toolArgs = args["args"];
    }

    auto agentPtr = agentContext.lock();
    if (!agentPtr) {
        throw std::runtime_error{"agent context is gone"};
    }
    auto host = agentPtr->host.lock();
    if (!host) {
        throw std::runtime_error{
            "background tool run needs an agent host (this agent has no container)"
        };
    }
    // 工具必须真的在这个 agent 的工具注册表里: 否则后台任务跑到一半才发现找不到, 白占一个额度
    // - 只能跑注册表里的工具 (插件工具与动态注册的), lib 内置工具由引擎独立持有, 不在此列
    if (!agentPtr->toolRegistry || !agentPtr->toolRegistry->find(toolName)) {
        throw std::invalid_argument{fmt::format(
            "tool `{}` is not in the tool registry (only plugin / dynamically registered "
            "tools can run in the background)",
            toolName
        )};
    }

    // 参数与前台调用保持一致: 下游工具可能用到这两个字段 (ToolcallWrapNode 同样会注入)
    toolArgs["sessionId"] = sessionId;
    if (const auto callId = args.value("tool_call_id", std::string{}); !callId.empty()) {
        toolArgs["tool_call_id"] = callId;
    }

    // 权限检查: 后台执行不再经过 ToolcallWrapNode, 因此**在提交前**按真正的目标工具
    // 与参数问一次 (与前台同一套权限中间件, 该询问用户时照旧询问)。
    // 缺这一步的话, 模型只要拿到本工具的调用权就能绕过目标工具自己的限制
    if (agentPtr->bus) {
        auto resp
            = co_await agentPtr->bus
                  ->request<agentxx::events::ReqToolPermissionCheck,
                            agentxx::events::RespToolPermissionCheck>(
                      agentxx::events::Topic::ToolPermissionCheck,
                      agentxx::events::ReqToolPermissionCheck{
                          .agentName = agentPtr->agentConfig
                                           ? agentPtr->agentConfig->agentName
                                           : std::string{},
                          .sessionId = sessionId,
                          .toolName  = toolName,
                          .arguments = toolArgs,
                      },
                      std::chrono::milliseconds{0}
                  );
        if (resp.has_value() && !resp->allow) {
            co_return agentxx::util::ToolResult::fail(
                resp->reason.empty()
                    ? fmt::format("permission denied: `{}`", toolName)
                    : fmt::format("permission denied: {}", resp->reason)
            );
        }
    }

    agentxx::agent::AsyncWorkSpec spec;
    spec.kind             = agentxx::agent::AsyncWorkKind::Tool;
    spec.name             = args.value("label", std::string{});
    if (spec.name.empty()) {
        spec.name = toolName;
    }
    spec.toolName         = toolName;
    spec.toolArgs         = std::move(toolArgs);
    spec.creatorSessionId = sessionId;
    spec.creatorAgentId   = agentPtr->agentId;
    spec.resultId         = args.value("tool_call_id", std::string{});
    spec.owner            = "tool:agentxx_tool_async";
    // 中断策略跟随创建者配置 (工具内 HIL 询问在主会话上照旧冒泡)
    spec.baseConfig = agentPtr->agentConfig;

    const auto result = host->submitAsyncWork(std::move(spec), agentPtr);
    if (!result.accepted) {
        throw std::runtime_error{
            fmt::format("background tool submit rejected: {}", result.error)
        };
    }

    co_return agentxx::util::ToolResult::ok(fmt::format(
        "[Async tool started] id={} tool={}\n"
        "结果跑完会作为一条输入回到这个会话 (事件 `agentxx.agent.async.done`); "
        "想当场拿结果用 agentxx_event {{opt:\"await\", id:\"{}\"}}, "
        "要停止用 {{opt:\"cancel\", id:\"{}\"}}。",
        result.asyncId,
        toolName,
        result.asyncId,
        result.asyncId
    ));
}

} // namespace tools
} // namespace agentxx
