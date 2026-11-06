#include "agentxx/tools/async_emit.h"
#include "agentxx/util/neograph_json_bridge.h"

#include "agentxx/agent/context.h"
#include "agentxx/event/hub.h"
#include "agentxx/event/naming.h"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace agentxx {
namespace tools {

namespace {

using agentxx::events::EndOutcome;
using agentxx::events::EventDefineSpec;
using agentxx::events::EventHub;
using agentxx::events::EventKind;
using agentxx::events::PublishMask;
using agentxx::events::PublishOutcome;

/// 模型侧工具调用的发布者归属 (事件记录创建时也用它)
constexpr std::string_view kModelOwner = "tool";

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

/// 解析 `public_publish`: `true` = 除创建者外任何调用方也能发 (对应 `PublishMask::Any`)
/// - 缺省 `false` = 只有创建者能发
PublishMask parsePublicPublish(const utilxx_base::Json& args) {
    if (!args.contains("public_publish") || args["public_publish"].is_null()) {
        return PublishMask::None;
    }
    const auto& raw = args["public_publish"];
    if (!raw.is_boolean()) {
        throw std::invalid_argument{"Arg `public_publish` must be a boolean"};
    }
    return raw.get<bool>() ? PublishMask::Any : PublishMask::None;
}

/// 本次投递到的等待: `w-3 next-step, w-7 none`
std::string joinDeliveries(const utilxx_base::Json& deliveries) {
    std::string out;
    for (const auto& item : deliveries) {
        if (!out.empty()) {
            out += ", ";
        }
        out += item.value("waitId", std::string{"?"});
        const auto delivery = item.value("delivery", std::string{});
        if (!delivery.empty()) {
            out += " " + delivery;
        }
    }
    return out;
}

} // namespace

AsyncEmitTool::AsyncEmitTool(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext) :
    XXToolBase(
        "agentxx_async_emit",
        in_agentContext,
        /*autoSummaryOutput=*/true,
        /*canDelayLoad=*/false,
        /*maxRetry=*/0,
        /*repeatCallCheck=*/false,
        /*supportsParallel=*/false
    ) {}

neograph::ChatTool AsyncEmitTool::get_definition() const {
    auto        agentPtr = agentContext.lock();
    const auto& prompt   = agentPtr->agentConfig->prompt.toolPrompt[get_name()];

    utilxx_base::Json params = utilxx_base::Json{
        {"type", "object"},
        {
         "properties", {
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
                    "end",
                    {
                        {"type", "boolean"},
                        {"description", prompt.getArg("end")},
                    },
                },
                {
                    "public_publish",
                    {
                        {"type", "boolean"},
                        {"description", prompt.getArg("public_publish")},
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
        {"required", utilxx_base::Json::array({"event"})},
    };
    return {get_name(), prompt.depict, agentxx::util::toNeographJson(params)};
}

asio::awaitable<agentxx::util::ToolResult>
    AsyncEmitTool::execute_async(const utilxx_base::Json& arguments) {
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
    const auto eventName = requireString(arguments, "event");
    std::string nameErr;
    if (!agentxx::events::naming::validateEventName(eventName, &nameErr)) {
        throw std::invalid_argument{fmt::format("Arg `event` = `{}` is invalid: {}", eventName, nameErr)};
    }
    const bool wantEnd  = arguments.value("end", false);
    const bool hasValue = arguments.contains("value");
    if (!hasValue && !wantEnd) {
        throw std::invalid_argument{
            "Give `value` to publish something, `end: true` to close the stream, or both"
        };
    }

    // 事件不存在: 只有拿到创建资格时才建 (模型只能在 `agentxx.agent.event.*` 自建)
    bool    created = false;
    PublishMask mask = PublishMask::None;
    if (!hub->queryEvent(eventName).has_value()) {
        if (!hasValue) {
            throw std::invalid_argument{fmt::format(
                "Event `{}` does not exist, so there is nothing to end; publish a value to create it",
                eventName
            )};
        }
        if (!agentxx::events::mayCreateByPublish(eventName, kModelOwner)) {
            throw std::invalid_argument{fmt::format(
                "Event `{}` is not defined and you may not create it; only its owner can "
                "(use `agentxx.agent.event.<name>` for events of your own)",
                eventName
            )};
        }
        mask                   = parsePublicPublish(arguments);
        EventDefineSpec spec   = {};
        spec.name              = eventName;
        spec.kind              = EventKind::Stream;
        spec.owner             = std::string{kModelOwner};
        spec.originSession     = sessionId;
        spec.description       = arguments.value("label", std::string{});
        spec.publishMask       = mask;
        std::string defineErr;
        if (!hub->define(spec, &defineErr)) {
            throw std::runtime_error{defineErr};
        }
        created = true;
    }

    std::optional<PublishOutcome> published;
    if (hasValue) {
        auto outcome = hub->publishValueEx(eventName, arguments["value"], kModelOwner);
        if (!outcome.has_value()) {
            throw std::runtime_error{outcome.error()};
        }
        published = std::move(*outcome);
    }
    std::optional<EndOutcome> ended;
    if (wantEnd) {
        auto outcome = hub->endEventEx(eventName, utilxx_base::Json::object(), kModelOwner);
        if (!outcome.has_value()) {
            throw std::runtime_error{outcome.error()};
        }
        ended = std::move(*outcome);
    }

    // 结果只说这次调用带来的新信息: 投递到了谁、累计多少、还剩几个等待
    std::string text;
    if (created) {
        text += fmt::format(
            "Created a new stream (publishing allowed to: {})",
            agentxx::events::publishMaskText(mask)
        );
    }
    if (published.has_value()) {
        if (!text.empty()) {
            text += "; ";
        }
        text += fmt::format("{} value(s) published so far", published->published);
        if (published->delivered > 0) {
            text += fmt::format(
                "; delivered to {} waiter(s): {}",
                published->delivered,
                joinDeliveries(published->deliveries)
            );
        } else {
            text += "; nobody was waiting (the value stays in history for replay)";
        }
        if (published->remainingWaits > 0) {
            text += fmt::format("; {} waiter(s) still attached", published->remainingWaits);
        }
        if (published->endedNow) {
            text += "; single-value event closed automatically";
        }
    }
    if (ended.has_value()) {
        if (!text.empty()) {
            text += "; ";
        }
        if (ended->alreadyEnded) {
            text += "The stream was already closed, nothing changed";
        } else {
            text += fmt::format("stream closed after {} value(s)", ended->published);
            if (ended->closedWaits > 0) {
                text += fmt::format("; {} waiter(s) finished without a value", ended->closedWaits);
            }
        }
    }
    if (text.empty()) {
        text = "Nothing to do";
    }
    co_return agentxx::util::ToolResult::ok(text);
}

} // namespace tools
} // namespace agentxx
