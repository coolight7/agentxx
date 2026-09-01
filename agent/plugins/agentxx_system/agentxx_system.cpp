// agentxx_system —— 系统信息工具插件
#include "agentxx_system_plugin.h"
#include "system_impl.h"
#include <string>

using namespace agentxx_system_plugin;

namespace {

constexpr std::string_view kNameDatetime   = "agentxx_get_current_datetime";
constexpr std::string_view kDepictDatetime = "Get the current date, time, and Unix timestamp.";

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
            static const AgentxxPluginInfo info{
                AGENTXX_PLUGIN_API_VERSION,
                agentxx_plugin_sv_cstr("agentxx_system"),
                agentxx_plugin_sv_cstr("1.0.0"),
                agentxx_plugin_sv_cstr("System info tools: current date/time with Unix timestamp"),
            };
            return &info;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    PluginCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
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
            agentxx::plugin::fast_tool(
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
    agentxx::plugin::guardCallVoid(ctxGuardLogger(ctx), [&] {
        if (ctx) {
            delete ctx;
        }
    });
}
