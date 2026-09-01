// agentxx_text_selection_monitor 插件 —— 共享头
#pragma once

#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/util/log.h"
#include "fmt/format.h"
#include "simdjson.h"
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace agentxx_text_selection_monitor_plugin {

inline void pluginLog(
    const AgentxxPluginHost*     host,
    const AgentxxPluginLogIface* logIf,
    int                          level,
    const std::string&           msg
) {
    if (host && logIf && logIf->log) {
        logIf->log(host, level, agentxx_plugin_sv(msg.data(), msg.size()));
    }
}

using PluginLogSink = std::function<void(int level, const std::string& msg)>;
inline std::atomic<const PluginLogSink*> g_log_sink{nullptr};

inline void pluginLog(int level, const std::string& msg) {
    if (const auto* sink = g_log_sink.load(std::memory_order_acquire)) {
        (*sink)(level, msg);
    }
}

inline char* pluginStrdup(const AgentxxPluginHost* host, const char* s) {
    if (!host || !s) {
        return nullptr;
    }
    return host->vtable->strdup(s);
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

    simdjson::ondemand::parser               parser_{};
    std::unique_ptr<simdjson::padded_string> padded_{};
    simdjson::ondemand::document             doc_{};
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

} // namespace agentxx_text_selection_monitor_plugin
