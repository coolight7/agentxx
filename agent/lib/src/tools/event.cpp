#include "agentxx/tools/event.h"
#include "agentxx/util/neograph_json_bridge.h"

#include "agentxx/agent/context.h"
#include "agentxx/event/hub.h"
#include "agentxx/event/naming.h"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include <stdexcept>
#include <string>
#include <string_view>

namespace agentxx {
namespace tools {

namespace {

using agentxx::events::EventDefineSpec;
using agentxx::events::EventHub;
using agentxx::events::EventKind;

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

/// 模型可写的事件名
/// - 只能落在 `agentxx.agent.event.*` (内置 agent 端事件族) 且不占用内建保留子段
///   (`turn.` / `tool.` / `subagent.` / `timer.` / `host.`)
/// - 插件事件 (`plugin.<插件名>.*`): 模型不写, 发布由插件自己做
std::string requireWritableEventName(const utilxx_base::Json& args) {
    auto        name = requireString(args, "event");
    std::string err;
    if (!agentxx::events::naming::validateAgentEventName(name, &err)) {
        throw std::invalid_argument{fmt::format("Arg `event` = `{}` is invalid: {}", name, err)};
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
                        {"enum", utilxx_base::Json::array({"emit", "end"})},
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
                    "create",
                    {
                        {"type", "boolean"},
                        {"description", prompt.getArg("create")},
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
                    "remove",
                    {
                        {"type", "boolean"},
                        {"description", prompt.getArg("remove")},
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
        // 中枢常驻, 走到这里说明 init 没跑完 (属于程序错误, 不该被模型看到)
        throw std::runtime_error{"event hub is not initialised"};
    }
    const auto sessionId = requireSessionId(arguments);
    const auto opt       = requireString(arguments, "opt");

    if (opt == "emit") {
        const auto eventName = requireWritableEventName(arguments);
        const bool single    = arguments.value("single", false);
        const bool create    = arguments.value("create", true);
        if (create && !hub->queryEvent(eventName).has_value()) {
            EventDefineSpec spec;
            spec.name          = eventName;
            spec.kind          = single ? EventKind::Single : EventKind::Stream;
            spec.owner         = "tool";
            spec.originSession = sessionId;
            spec.description   = arguments.value("label", std::string{});
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
        const auto record    = hub->queryEvent(eventName);
        const auto state     = record.has_value()
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
        const auto  eventName = requireWritableEventName(arguments);
        auto        summary   = arguments.contains("summary") ? arguments["summary"]
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

    throw std::invalid_argument{fmt::format("Arg `opt` must be `emit` or `end`, got `{}`", opt)};
}

} // namespace tools
} // namespace agentxx
