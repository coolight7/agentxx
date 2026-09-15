/// agentxx_screen_capture 插件 —— 共享头
#pragma once

#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "agentxx/util/log.h"
#include "fmt/format.h"
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace agentxx_screen_capture_plugin {

/// 公共助手统一由插件 SDK 提供 (实现见 plugin_kit.h)
using agentxx::plugin::pluginLog;
using agentxx::plugin::pluginStrdup;

/// 参数读取统一经 plugin_kit.h 的 ArgReader (agentxx::util::Json 驱动),
/// 不再手写 simdjson::ondemand 局部解析桩 (历史 SimpleJson/jsonGet* 已删除)

} // namespace agentxx_screen_capture_plugin
