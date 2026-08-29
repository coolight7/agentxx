// agentxx_system_monitor 插件 —— 共享头
#pragma once

#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_guard.h"
#include "agentxx/plugin/plugin_kit.h"
#include "agentxx/util/log.h"
#include "fmt/format.h"
#include "simdjson.h"
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#define XX_IS_WIN_D   1
#define XX_IS_LINUX_D 0
#define XX_IS_MACOS_D 0
#elif defined(__APPLE__)
#define XX_IS_WIN_D   0
#define XX_IS_LINUX_D 0
#define XX_IS_MACOS_D 1
#else
#define XX_IS_WIN_D   0
#define XX_IS_LINUX_D 1
#define XX_IS_MACOS_D 0
#endif

namespace agentxx_system_monitor_plugin {

inline void pluginLog(
    const AgentxxHost*     host,
    const AgentxxLogIface* logIf,
    int                    level,
    const std::string&     msg
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

inline char* pluginStrdup(const AgentxxHost* host, const char* s) {
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

} // namespace agentxx_system_monitor_plugin
