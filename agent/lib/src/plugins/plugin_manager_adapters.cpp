#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/event/event_stream.h"
#include "agentxx/plugin/op_driver.h"
#include "agentxx/util/async_offload.h"
#include "agentxx/util/log.h"
#include "asio/this_coro.hpp"
#include "fmt/format.h"

#include <algorithm>

namespace agentxx {
namespace plugin {

// =====================================================================
// PluginTool
// =====================================================================

PluginTool::PluginTool(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext,
    std::shared_ptr<PluginInstance>             instance,
    AgentxxToolSpec                             spec
) :
    XXToolBase(
        std::string{spec.name.data ? spec.name.data : "", spec.name.size},
        std::move(agentContext),
        (spec.flags & AGENTXX_TOOL_FLAG_AUTO_SUMMARY) != 0,
        false,
        0,
        false
    ),
    name_{spec.name.data ? spec.name.data : "", spec.name.size},
    description_{spec.description.data ? spec.description.data : "", spec.description.size},
    parametersJson_{
        spec.parameters_json.data ? spec.parameters_json.data : "",
        spec.parameters_json.size
    },
    parameters_(neograph::json::object()),
    instance_(instance) {
    spec_                 = spec;
    spec_.name            = agentxx_plugin_sv(name_.data(), name_.size());
    spec_.description     = agentxx_plugin_sv(description_.data(), description_.size());
    spec_.parameters_json = agentxx_plugin_sv(parametersJson_.data(), parametersJson_.size());

    if (!parametersJson_.empty()) {
        try {
            auto params = neograph::json::parse(parametersJson_);
            if (params.is_object()) {
                parameters_ = std::move(params);
            }
        } catch (const std::exception& e) {
            XX_LOGW("PluginTool `{}`: invalid parameters_json: {}", name_, e.what());
        }
    }
}

neograph::ChatTool PluginTool::get_definition() const {
    neograph::ChatTool def;
    def.name        = name_;
    def.description = description_;
    def.parameters  = parameters_;
    return def;
}

asio::awaitable<std::string> PluginTool::execute_async(const neograph::json& arguments) {
    auto inst = instance_.lock();
    if (!inst) {
        throw std::runtime_error("plugin instance released");
    }
    if (!inst->enabled) {
        throw std::runtime_error("plugin disabled");
    }
    if (!spec_.execute_start) {
        throw std::runtime_error("plugin tool has null execute_start callback");
    }

    std::string argsJson   = arguments.dump();
    std::string sessionId  = arguments.value("sessionId", std::string{});
    std::string toolCallId = arguments.value("tool_call_id", std::string{});

    auto                                          agentCtx = agentContext.lock();
    std::shared_ptr<neograph::graph::CancelToken> cancelToken;
    if (agentCtx) {
        cancelToken = agentxx::tools::getSessionCancelToken(agentCtx, arguments);
    }

    auto       ex       = co_await asio::this_coro::executor;
    const auto spec     = spec_;
    auto       instKeep = inst;

    plugin::OpDrive drive;
    drive.start = [spec, instKeep, argsJson, sessionId, toolCallId](
                      const AgentxxOpNotify* notify,
                      char**                 err
                  ) -> void* {
        return spec.execute_start(
            spec.user_data,
            agentxx_plugin_sv(argsJson.data(), argsJson.size()),
            agentxx_plugin_sv(sessionId.data(), sessionId.size()),
            agentxx_plugin_sv(toolCallId.data(), toolCallId.size()),
            notify,
            err
        );
    };
    drive.cancel = [spec, instKeep](void* op) {
        if (spec.execute_cancel) {
            spec.execute_cancel(spec.user_data, op);
        }
    };

    auto awaitArgs = plugin::PluginOpAwaitArgs{
        .inst         = std::move(inst),
        .label        = name_,
        .ex           = ex,
        .cancelToken  = std::move(cancelToken),
        .drive        = std::move(drive),
    };

    if (spec_.default_timeout_ms > 0) {
        auto timeout = std::chrono::milliseconds{spec_.default_timeout_ms};
        co_return co_await agentxx::util::asyncWithTimeout<std::string>(
            [a = std::move(awaitArgs)]() mutable -> asio::awaitable<std::string> {
                co_return co_await plugin::awaitPluginOp(std::move(a));
            },
            timeout,
            []() -> std::string {
                return "[Plugin tool timeout]";
            }
        );
    }
    co_return co_await plugin::awaitPluginOp(std::move(awaitArgs));
}

// =====================================================================
// PluginMiddlewareHandle
// =====================================================================

PluginMiddlewareHandle::PluginMiddlewareHandle(
    std::string_view                            name,
    std::weak_ptr<agentxx::agent::AgentContext> agentContext,
    std::shared_ptr<PluginInstance>             instance
) :
    BaseMiddlewareHandle(name, std::move(agentContext)),
    instance_(instance) {}

void PluginMiddlewareHandle::setHook(const AgentxxHookSpec& spec) {
    if (spec.point < 0 || spec.point >= AGENTXX_HOOK_COUNT) {
        return;
    }
    auto& h   = hooks_[static_cast<size_t>(spec.point)];
    h.start   = spec.hook_start;
    h.cancel  = spec.hook_cancel;
    h.ud      = spec.user_data;
    h.set     = spec.hook_start != nullptr;
}

void PluginMiddlewareHandle::clearHook(AgentxxHookPoint point) {
    if (point < 0 || point >= AGENTXX_HOOK_COUNT) {
        return;
    }
    hooks_[static_cast<size_t>(point)] = HookEntry{};
}

static neograph::json summarizeNodeInput(
    AgentxxHookPoint                  point,
    const neograph::graph::NodeInput& in
) {
    neograph::json j;
    j["sessionId"] = in.ctx.thread_id;
    j["point"]     = static_cast<int>(point);
    return j;
}

asio::awaitable<void> PluginMiddlewareHandle::dispatch(
    AgentxxHookPoint                  point,
    const neograph::graph::NodeInput& in
) {
    if (point < 0 || point >= AGENTXX_HOOK_COUNT) {
        co_return;
    }
    const auto& hook = hooks_[static_cast<size_t>(point)];
    if (!hook.set || !hook.start) {
        co_return;
    }
    auto inst = instance_.lock();
    if (!inst || !inst->enabled) {
        co_return;
    }

    auto        inputJson = summarizeNodeInput(point, in).dump();
    auto        ex        = co_await asio::this_coro::executor;
    auto        instKeep  = inst;

    plugin::OpDrive drive;
    drive.start = [hook, instKeep, inputJson, point](
                      const AgentxxOpNotify* notify,
                      char**                 err
                  ) -> void* {
        return hook.start(
            hook.ud,
            point,
            agentxx_plugin_sv(inputJson.data(), inputJson.size()),
            notify,
            err
        );
    };
    drive.cancel = [hook](void* op) {
        if (hook.cancel) {
            hook.cancel(hook.ud, op);
        }
    };

    try {
        co_await plugin::awaitPluginOp(plugin::PluginOpAwaitArgs{
            .inst        = inst,
            .label       = fmt::format("hook#{}", static_cast<int>(point)),
            .ex          = ex,
            .cancelToken = nullptr,
            .drive       = std::move(drive),
        });
    } catch (const std::exception& e) {
        XX_LOGW("Plugin `{}` hook point={} failed: {}", inst->name, static_cast<int>(point), e.what());
    } catch (...) {
        XX_LOGW("Plugin `{}` hook point={} unknown failure", inst->name, static_cast<int>(point));
    }
}

asio::awaitable<void> PluginMiddlewareHandle::onAgentcallStartFunc(neograph::graph::NodeInput& in) {
    co_await dispatch(AGENTXX_HOOK_AGENT_START, in);
}

asio::awaitable<void> PluginMiddlewareHandle::
    onAgentcallEndFunc(const neograph::graph::NodeInput& in, neograph::graph::NodeOutput&) {
    co_await dispatch(AGENTXX_HOOK_AGENT_END, in);
}

asio::awaitable<void> PluginMiddlewareHandle::onModelcallStartFunc(neograph::graph::NodeInput& in) {
    co_await dispatch(AGENTXX_HOOK_MODEL_START, in);
}

asio::awaitable<void> PluginMiddlewareHandle::onModelcallRunFunc(neograph::graph::NodeInput& in) {
    co_await dispatch(AGENTXX_HOOK_MODEL_RUN, in);
}

asio::awaitable<void> PluginMiddlewareHandle::
    onModelcallEndFunc(const neograph::graph::NodeInput& in, neograph::graph::NodeOutput&) {
    co_await dispatch(AGENTXX_HOOK_MODEL_END, in);
}

asio::awaitable<void> PluginMiddlewareHandle::onToolcallStartFunc(neograph::graph::NodeInput& in) {
    co_await dispatch(AGENTXX_HOOK_TOOL_START, in);
}

asio::awaitable<void> PluginMiddlewareHandle::
    onToolcallEndFunc(const neograph::graph::NodeInput& in, neograph::graph::NodeOutput&) {
    co_await dispatch(AGENTXX_HOOK_TOOL_END, in);
}

// =====================================================================
// 注册与事件方法
// =====================================================================

int PluginManager::registerTool(PluginInstance* inst, const AgentxxToolSpec* spec) {
    if (!inst || !spec || agentxx_plugin_sv_empty(spec->name)) {
        return -1;
    }
    std::string toolName{spec->name.data, spec->name.size};

    if (registry_->contains(toolName)) {
        XX_LOGW("ToolRegistry: tool `{}` conflicts with built-in tool", toolName);
        return -1;
    }

    auto shared = inst->self.lock();
    if (!shared) {
        return -1;
    }
    auto tool = std::make_shared<PluginTool>(agentContext_, shared, *spec);
    registry_->registerTool(toolName, tool);
    inst->toolNames.push_back(toolName);
    inst->tools.push_back(tool);
    XX_LOGI("Plugin `{}` registered tool `{}`", inst->name, toolName);
    return 0;
}

int PluginManager::unregisterTool(PluginInstance* inst, const char* name) {
    if (!inst || !name) {
        return -1;
    }
    std::string toolName = name;
    auto        it       = std::find(inst->toolNames.begin(), inst->toolNames.end(), toolName);
    if (it == inst->toolNames.end()) {
        XX_LOGW("Plugin `{}` unregister tool `{}` not owned by this plugin", inst->name, toolName);
        return -1;
    }
    inst->toolNames.erase(it);
    registry_->unregisterTool(toolName);
    inst->tools.erase(
        std::remove_if(
            inst->tools.begin(),
            inst->tools.end(),
            [&toolName](const std::shared_ptr<PluginTool>& t) {
                return t->get_definition().name == toolName;
            }
        ),
        inst->tools.end()
    );
    XX_LOGI("Plugin `{}` unregistered tool `{}`", inst->name, toolName);
    return 0;
}

int PluginManager::registerHook(PluginInstance* inst, const AgentxxHookSpec* spec) {
    if (!inst || !spec || spec->point < 0 || spec->point >= AGENTXX_HOOK_COUNT
        || !spec->hook_start) {
        return -1;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return -1;
    }
    if (!inst->middleware) {
        auto shared = inst->self.lock();
        if (!shared) {
            return -1;
        }
        inst->middleware = std::make_shared<PluginMiddlewareHandle>(
            fmt::format("{}_middleware", inst->name),
            agentContext_,
            shared
        );
        ctx->middlewareHandleContext->handles.push_back(inst->middleware);
    }
    inst->middleware->setHook(*spec);

    inst->hookRegistrations.erase(
        std::remove_if(
            inst->hookRegistrations.begin(),
            inst->hookRegistrations.end(),
            [spec](const PluginInstance::HookRegistration& h) {
                return h.point == spec->point;
            }
        ),
        inst->hookRegistrations.end()
    );
    inst->hookRegistrations.push_back(
        PluginInstance::HookRegistration{spec->point, spec->hook_start, spec->hook_cancel, spec->user_data}
    );
    return 0;
}

int PluginManager::unregisterHook(PluginInstance* inst, AgentxxHookPoint point) {
    if (!inst || point < 0 || point >= AGENTXX_HOOK_COUNT) {
        return -1;
    }
    auto it = std::find_if(
        inst->hookRegistrations.begin(),
        inst->hookRegistrations.end(),
        [point](const PluginInstance::HookRegistration& h) {
            return h.point == point;
        }
    );
    if (it == inst->hookRegistrations.end()) {
        return -1;
    }
    inst->hookRegistrations.erase(it);
    if (inst->middleware) {
        inst->middleware->clearHook(point);
    }
    return 0;
}

AgentxxSubscription* PluginManager::subscribe(
    PluginInstance* inst,
    const char*     topic,
    void (*handler)(AgentxxPluginStringView event_json, void* ud),
    void* ud
) {
    if (!inst || !topic || !*topic || !handler) {
        return nullptr;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->bus) {
        return nullptr;
    }
    std::string fullTopic = std::string(topic);
    if (!fullTopic.starts_with("plugin.") && !fullTopic.starts_with("client.")) {
        fullTopic = "plugin." + fullTopic;
    }
    auto sub = std::make_shared<AgentxxSubscription>();
    sub->bus     = ctx->bus;
    sub->topic   = fullTopic;
    sub->inst    = inst;
    sub->handler = handler;
    sub->ud      = ud;

    auto subId = ctx->bus->get<std::string>(fullTopic).subscribe(
        [sub](const std::string& data) -> asio::awaitable<void> {
            if (!sub->inst || !sub->inst->enabled || !sub->handler) {
                co_return;
            }
            PluginInstance::InflightGuard guard(sub->inst);
            try {
                sub->handler(agentxx_plugin_sv(data.data(), data.size()), sub->ud);
            } catch (const std::exception& e) {
                XX_LOGW(
                    "Plugin `{}` event handler threw: {}",
                    sub->inst ? sub->inst->name : "?",
                    e.what()
                );
            } catch (...) {
                XX_LOGW(
                    "Plugin `{}` event handler threw unknown exception",
                    sub->inst ? sub->inst->name : "?"
                );
            }
            co_return;
        }
    );
    sub->subscriptionId = subId;
    inst->subscriptions.push_back(sub);
    return sub.get();
}

void PluginManager::unsubscribe(AgentxxSubscription* sub) {
    if (!sub || !sub->bus || sub->subscriptionId == 0) {
        return;
    }
    sub->bus->get<std::string>(sub->topic).unsubscribe(sub->subscriptionId);
    sub->subscriptionId = 0;
    if (sub->inst) {
        auto it = std::find_if(
            sub->inst->subscriptions.begin(),
            sub->inst->subscriptions.end(),
            [sub](const std::shared_ptr<AgentxxSubscription>& s) {
                return s.get() == sub;
            }
        );
        if (it != sub->inst->subscriptions.end()) {
            sub->inst->subscriptions.erase(it);
        }
    }
}

int PluginManager::publish(const char* topic, const char* event_json) {
    if (!topic || !*topic) {
        return -1;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->bus) {
        return -1;
    }
    std::string fullTopic = std::string(topic);
    if (!fullTopic.starts_with("plugin.") && !fullTopic.starts_with("client.")) {
        fullTopic = "plugin." + fullTopic;
    }
    std::string payload = event_json ? std::string(event_json) : std::string("{}");
    if (ioExecutor_) {
        asio::co_spawn(
            ioExecutor_,
            [bus = ctx->bus, fullTopic = std::move(fullTopic), payload = std::move(payload)]() -> asio::awaitable<void> {
                co_await bus->publish(fullTopic, payload);
            },
            asio::detached
        );
    }
    return 0;
}

} // namespace plugin
} // namespace agentxx
