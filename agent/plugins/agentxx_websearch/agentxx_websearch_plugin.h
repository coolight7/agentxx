/// agentxx_websearch 插件 —— 共享头
#pragma once

#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/util/json.h"
#include <fmt/format.h>
#include <memory>
#include <string>

namespace agentxx_websearch_plugin {

/// 公共助手统一由插件 SDK 提供 (实现见 plugin_kit.h)
using agentxx::plugin::ctxGuardLogger;
using agentxx::plugin::pluginLog;
using agentxx::plugin::pluginStrdup;

struct WebSearchConfig {
    std::string baseUrl;
    std::string apiKey                  = "EMPTY";
    std::string modelName               = "Agentxx";
    int         readChunkTimeoutSeconds = 100;
};

struct PluginCtx : public agentxx::plugin::PluginBase {
    bool            use_model_search      = false;
    bool            convert_html2markdown = true;
    std::string     search_api_url;
    WebSearchConfig model_cfg{};
};

} // namespace agentxx_websearch_plugin
