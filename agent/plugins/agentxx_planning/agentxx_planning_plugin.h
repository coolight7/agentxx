/// agentxx_planning 插件 —— 共享头
#pragma once

#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/agent_plugin_kit.h"
#include "agentxx/plugin/api/client_plugin_kit.h"
#include "utilxx_base/json.h"
#include <fmt/format.h>
#include <memory>
#include <string>

namespace agentxx_planning_plugin {

/// 公共助手统一由插件 SDK 提供 (实现见 plugin_kit.h)
using agentxx::plugin::ctxGuardLogger;
using agentxx::plugin::pluginLog;
using agentxx::plugin::pluginStrdup;

struct PluginCtx : public agentxx::plugin::PluginBase {
    /// 主程序数据目录 (挂载事务里读一次缓存; 之后工具/事件回调直接用它派生目录)
    std::string dataDir;
};

} // namespace agentxx_planning_plugin
