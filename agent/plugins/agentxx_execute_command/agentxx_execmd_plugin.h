// agentxx_execute_command 插件 —— 共享头
#pragma once

#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include <fmt/format.h>
#include <memory>
#include <neograph/json.h>
#include <string>

namespace agentxx_execmd_plugin {

struct PluginCtx : public agentxx::plugin::PluginBase {};

inline void pluginLog(const PluginCtx* ctx, int level, const std::string& msg) {
    if (ctx) {
        ctx->log.log(level, msg);
    }
}

inline char* pluginStrdup(const AgentxxPluginHost* host, const char* s) {
    if (!host || !s) {
        return nullptr;
    }
    auto sv = agentxx::plugin::PluginStringView::fromCstr(s);
    return agentxx::plugin::PluginString::strdup(host, &sv);
}

inline auto ctxGuardLogger(PluginCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx) {
            ctx->log.error(msg ? msg : "");
        }
    };
}

} // namespace agentxx_execmd_plugin
