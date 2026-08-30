// agentxx_computer_use 插件 —— 共享头
#pragma once

#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_guard.h"
#include "agentxx/plugin/plugin_kit.h"
#include "codegraph/core/json.hpp"
#include "fmt/format.h"
#include <cstring>
#include <string>

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

namespace agentxx_computer_use_plugin {

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

inline bool jsonGetBool(simdjson::simdjson_result<simdjson::ondemand::value> val, bool& out) {
    if (val.error()) {
        return false;
    }
    auto v = val.get_bool();
    if (v.error()) {
        return false;
    }
    out = v.value();
    return true;
}

} // namespace agentxx_computer_use_plugin
