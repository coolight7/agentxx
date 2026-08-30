// agentxx_system —— 系统信息工具插件
#include "agentxx_system_plugin.h"
#include "system_impl.h"
#include <string>

using namespace agentxx_system_plugin;

namespace {

constexpr auto kNameDatetime   = "agentxx_get_current_datetime";
constexpr auto kDepictDatetime = "Get the current date, time, and Unix timestamp.";

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin_guard::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
            static const AgentxxPluginInfo info{
                AGENTXX_PLUGIN_API_VERSION,
                AGENTXX_SV("agentxx_system"),
                AGENTXX_SV("1.0.0"),
                AGENTXX_SV("System info tools: current date/time with Unix timestamp"),
            };
            return &info;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxHost* host, void** plugin_ctx) {
    PluginCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
        [&raw](const char* msg) noexcept {
            ctxGuardLogger(raw)(msg);
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx = std::make_unique<PluginCtx>();
            ctx->init(host);
            raw = ctx.get();

            if (!ctx->iface.tools || !ctx->iface.tools->register_tool) {
                return -1;
            }

            // agentxx_get_current_datetime (fast_tool)
            agentxx::kit::fast_tool(
                *ctx,
                kNameDatetime,
                kDepictDatetime,
                R"({"type":"object","properties":{}})",
                [](std::string_view) -> std::string {
                    return currentDatetimeExecute();
                }
            );

            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(ctxGuardLogger(ctx), [&] {
        if (ctx) {
            delete ctx;
        }
    });
}
