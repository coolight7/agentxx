/// agentxx_string 插件 —— 共享头
#pragma once

#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include <fmt/format.h>
#include <memory>
#include <neograph/json.h>
#include <string>

namespace agentxx_string_plugin {

struct PluginCtx : public agentxx::plugin::PluginBase {};

inline auto ctxGuardLogger(PluginCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx) {
            ctx->log.error(msg ? msg : "");
        }
    };
}

} // namespace agentxx_string_plugin
