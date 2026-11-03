#pragma once

#include "agentxx/agent/conversation_types.h"
#include "agentxx/event/naming.h"
#include <string>
#include <string_view>

namespace agentxx {
namespace test {

/// 事件名拼装 helper (测试用)
///
/// 命名契约见 `agentxx/event/naming.h`: 中枢事件名**就是**总线主题, 且必须写全名
/// (框架只校验不补前缀)。测试里统一经这里拼, 免得每个字面量都重复前缀。

/// 内置值事件全名: `agentxx.agent.event.<rest>`
inline std::string evName(std::string_view rest) {
    return std::string{events::naming::kAgentEventPrefix} + std::string{rest};
}

/// 插件值事件全名: `plugin.<插件名>.<rest>`
inline std::string pluginEvName(std::string_view pluginName, std::string_view rest) {
    return std::string{events::naming::kPluginRoot} + std::string{pluginName} + "."
           + std::string{rest};
}

/// 事件投递的来源标记: `event:<事件名>` (与投递侧同一拼法)
inline std::string eventSourceOf(std::string_view name) {
    return std::string{agentxx::agent::kInputSourceEventPrefix} + std::string{name};
}

} // namespace test
} // namespace agentxx
