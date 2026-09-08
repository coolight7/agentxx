/// agentxx_computer_use 插件 —— 共享头
#pragma once

#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/util/json.h"
#include "fmt/format.h"
#include <cstdint>
#include <string>
#include <vector>

namespace agentxx_computer_use_plugin {

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

/// 参数读取统一经 agentxx::util::Json (自主 DOM, simdjson 驱动解析),
/// 不再手写 simdjson::ondemand 局部解析桩 (历史 SimpleJson/jsonGet* 已删除)

} // namespace agentxx_computer_use_plugin
