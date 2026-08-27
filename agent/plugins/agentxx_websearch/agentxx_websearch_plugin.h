// agentxx_websearch 插件 —— 共享头
#pragma once

#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_guard.h"
#include "agentxx/plugin/plugin_kit.h"
#include <fmt/format.h>
#include <neograph/json.h>
#include <memory>
#include <string>

namespace agentxx_websearch_plugin {

struct WebSearchConfig {
    std::string baseUrl;
    std::string apiKey                  = "EMPTY";
    std::string modelName               = "Agentxx";
    int         readChunkTimeoutSeconds = 100;
};

struct PluginCtx : public agentxx::kit::PluginBase {
    agentxx::kit::ReactorLoop reactor;
    bool                      use_model_search      = false;
    bool                      convert_html2markdown = true;
    std::string               search_api_url;
    WebSearchConfig           model_cfg{};
};

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

} // namespace agentxx_websearch_plugin
