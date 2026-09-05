/// agentxx_audio_stream 插件 —— 共享头
#pragma once

#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "fmt/format.h"
#include "simdjson.h"
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace agentxx_audio_stream_plugin {

inline void pluginLog(
    const AgentxxPluginHost*     host,
    const AgentxxPluginLogIface* logIf,
    int32_t                      level,
    const std::string&           msg
) {
    if (host && logIf && logIf->log) {
        auto sv = agentxx::plugin::PluginStringView::from(msg.data(), msg.size());
        logIf->log(host, level, &sv);
    }
}

inline char* pluginStrdup(const AgentxxPluginHost* host, const char* s) {
    if (!host || !s) {
        return nullptr;
    }
    auto sv = agentxx::plugin::PluginStringView::fromCstr(s);
    return agentxx::plugin::PluginString::strdup(host, &sv);
}

class SimpleJson {
public:

    explicit SimpleJson(const std::string& s) {
        padded_ = std::make_unique<simdjson::padded_string>(s);
        auto r  = parser_.iterate(*padded_);
        if (r.error()) {
            ok_ = false;
            return;
        }
        doc_ = std::move(r).value();
        ok_  = true;
    }

    bool ok() const {
        return ok_;
    }

    simdjson::ondemand::document& doc() {
        return doc_;
    }

private:

    simdjson::ondemand::parser               parser_;
    std::unique_ptr<simdjson::padded_string> padded_;
    simdjson::ondemand::document             doc_;
    bool                                     ok_ = false;
};

inline bool
    jsonGetString(simdjson::simdjson_result<simdjson::ondemand::value> val, std::string& out) {
    if (val.error()) {
        return false;
    }
    auto sv = val.get_string();
    if (sv.error()) {
        return false;
    }
    out = sv.value();
    return true;
}

inline bool jsonGetInt(simdjson::simdjson_result<simdjson::ondemand::value> val, int64_t& out) {
    if (val.error()) {
        return false;
    }
    auto v = val.get_int64();
    if (v.error()) {
        return false;
    }
    out = v.value();
    return true;
}

} // namespace agentxx_audio_stream_plugin
