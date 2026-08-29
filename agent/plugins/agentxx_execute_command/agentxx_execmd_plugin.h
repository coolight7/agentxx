// agentxx_execute_command 插件 —— 共享头
#pragma once

#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_guard.h"
#include "agentxx/plugin/plugin_kit.h"
#include <fmt/format.h>
#include <memory>
#include <neograph/json.h>
#include <string>

namespace agentxx_execmd_plugin {

struct PluginCtx : public agentxx::kit::PluginBase {};

inline void pluginLog(const PluginCtx* ctx, int level, const std::string& msg) {
    if (ctx) {
        ctx->log.log(level, msg);
    }
}

inline char* pluginStrdup(const AgentxxHost* host, const char* s) {
    if (!host || !s) {
        return nullptr;
    }
    return host->vtable->strdup(s);
}

inline auto ctxGuardLogger(PluginCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx) {
            ctx->log.error(msg ? msg : "");
        }
    };
}

} // namespace agentxx_execmd_plugin
