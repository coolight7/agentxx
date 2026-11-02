/// agentxx_execute_command 插件 —— 共享头
#pragma once

#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "execute_command_impl.h"
#include "utilxx_base/json.h"
#include <fmt/format.h>
#include <memory>
#include <string>

namespace agentxx_execmd_plugin {

/// 公共助手统一由插件 SDK 提供 (实现见 plugin_kit.h)
using agentxx::plugin::ctxGuardLogger;
using agentxx::plugin::pluginLog;
using agentxx::plugin::pluginStrdup;
/// 工具结果 (带状态): 实现体返回它表达成功/失败, 不再靠文本前缀
using agentxx::plugin::ToolResult;
using agentxx::plugin::ToolStatus;

struct PluginCtx : public agentxx::plugin::PluginBase {};

} // namespace agentxx_execmd_plugin
