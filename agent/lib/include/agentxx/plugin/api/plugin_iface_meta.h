/// agentxx 插件接口表的元信息特化 (最低兼容版本 + 基线字节数)
///
/// 每张表在这里提供一份 `pluginxx::IfaceMeta` 特化与两条自检:
/// - `_BASELINE_SIZE >= 8` (表头本身就是 8 字节);
/// - `_BASELINE_SIZE <= sizeof(表)` (基线不能超过完整布局)。
///
/// 集中放一处是为了清点: 表数量、特化数量、`boundaries` 测试的逐表校验都对着这一份
/// 文件。**新增接口表时, 除了表头四件套, 还必须在这里补一条特化** —— 漏补不会编译
/// 失败 (未特化时按"整表必须齐全"处理), 因此由 `boundaries` 测试逐表核对数量。
///
/// 由 `plugin_api.h` / `client_plugin_api.h` 在表声明之后自动包含, 使用方无需显式包含。
#ifndef AGENTXX_PLUGIN_API_PLUGIN_IFACE_META_H
#define AGENTXX_PLUGIN_API_PLUGIN_IFACE_META_H

#include "agentxx/plugin/api/client_plugin_api.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "pluginxx/api/iface_meta.h"

namespace pluginxx {

/* ==================== agent 侧 (agentxx.agent.*) ==================== */

template<>
struct IfaceMeta<AgentxxPluginToolsIface> {
    static constexpr int32_t  min_version   = AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_BASELINE_SIZE;
};
static_assert(AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_BASELINE_SIZE >= 8u);
static_assert(AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_BASELINE_SIZE <= sizeof(AgentxxPluginToolsIface));

template<>
struct IfaceMeta<AgentxxPluginPermissionIface> {
    static constexpr int32_t  min_version   = AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION_BASELINE_SIZE;
};
static_assert(AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION_BASELINE_SIZE >= 8u);
static_assert(
    AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION_BASELINE_SIZE <= sizeof(AgentxxPluginPermissionIface)
);

template<>
struct IfaceMeta<AgentxxPluginHooksIface> {
    static constexpr int32_t  min_version   = AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_BASELINE_SIZE;
};
static_assert(AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_BASELINE_SIZE >= 8u);
static_assert(AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_BASELINE_SIZE <= sizeof(AgentxxPluginHooksIface));

template<>
struct IfaceMeta<AgentxxPluginSessionIface> {
    static constexpr int32_t  min_version   = AGENTXX_PLUGIN_IFACE_AGENT_SESSION_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_PLUGIN_IFACE_AGENT_SESSION_BASELINE_SIZE;
};
static_assert(AGENTXX_PLUGIN_IFACE_AGENT_SESSION_BASELINE_SIZE >= 8u);
static_assert(
    AGENTXX_PLUGIN_IFACE_AGENT_SESSION_BASELINE_SIZE <= sizeof(AgentxxPluginSessionIface)
);

template<>
struct IfaceMeta<AgentxxPluginContextIface> {
    static constexpr int32_t  min_version   = AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT_BASELINE_SIZE;
};
static_assert(AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT_BASELINE_SIZE >= 8u);
static_assert(
    AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT_BASELINE_SIZE <= sizeof(AgentxxPluginContextIface)
);

template<>
struct IfaceMeta<AgentxxPluginModelIface> {
    static constexpr int32_t  min_version   = AGENTXX_PLUGIN_IFACE_AGENT_MODEL_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_PLUGIN_IFACE_AGENT_MODEL_BASELINE_SIZE;
};
static_assert(AGENTXX_PLUGIN_IFACE_AGENT_MODEL_BASELINE_SIZE >= 8u);
static_assert(AGENTXX_PLUGIN_IFACE_AGENT_MODEL_BASELINE_SIZE <= sizeof(AgentxxPluginModelIface));

template<>
struct IfaceMeta<AgentxxPluginPromptIface> {
    static constexpr int32_t  min_version   = AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_BASELINE_SIZE;
};
static_assert(AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_BASELINE_SIZE >= 8u);
static_assert(AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_BASELINE_SIZE <= sizeof(AgentxxPluginPromptIface));

template<>
struct IfaceMeta<AgentxxPluginResourcesIface> {
    static constexpr int32_t  min_version   = AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_BASELINE_SIZE;
};
static_assert(AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_BASELINE_SIZE >= 8u);
static_assert(
    AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_BASELINE_SIZE <= sizeof(AgentxxPluginResourcesIface)
);

template<>
struct IfaceMeta<AgentxxPluginGraphIface> {
    static constexpr int32_t  min_version   = AGENTXX_PLUGIN_IFACE_AGENT_GRAPH_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_PLUGIN_IFACE_AGENT_GRAPH_BASELINE_SIZE;
};
static_assert(AGENTXX_PLUGIN_IFACE_AGENT_GRAPH_BASELINE_SIZE >= 8u);
static_assert(AGENTXX_PLUGIN_IFACE_AGENT_GRAPH_BASELINE_SIZE <= sizeof(AgentxxPluginGraphIface));

template<>
struct IfaceMeta<AgentxxPluginFeatureIface> {
    static constexpr int32_t  min_version   = AGENTXX_PLUGIN_IFACE_AGENT_FEATURE_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_PLUGIN_IFACE_AGENT_FEATURE_BASELINE_SIZE;
};
static_assert(AGENTXX_PLUGIN_IFACE_AGENT_FEATURE_BASELINE_SIZE >= 8u);
static_assert(
    AGENTXX_PLUGIN_IFACE_AGENT_FEATURE_BASELINE_SIZE <= sizeof(AgentxxPluginFeatureIface)
);

/* ==================== client 侧 (agentxx.client.*) ==================== */

template<>
struct IfaceMeta<AgentxxClientUiIface> {
    static constexpr int32_t  min_version   = AGENTXX_IFACE_CLIENT_UI_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_IFACE_CLIENT_UI_BASELINE_SIZE;
};
static_assert(AGENTXX_IFACE_CLIENT_UI_BASELINE_SIZE >= 8u);
static_assert(AGENTXX_IFACE_CLIENT_UI_BASELINE_SIZE <= sizeof(AgentxxClientUiIface));

template<>
struct IfaceMeta<AgentxxClientEventsIface> {
    static constexpr int32_t  min_version   = AGENTXX_IFACE_CLIENT_EVENTS_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_IFACE_CLIENT_EVENTS_BASELINE_SIZE;
};
static_assert(AGENTXX_IFACE_CLIENT_EVENTS_BASELINE_SIZE >= 8u);
static_assert(AGENTXX_IFACE_CLIENT_EVENTS_BASELINE_SIZE <= sizeof(AgentxxClientEventsIface));

template<>
struct IfaceMeta<AgentxxClientSessionIface> {
    static constexpr int32_t  min_version   = AGENTXX_IFACE_CLIENT_SESSION_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_IFACE_CLIENT_SESSION_BASELINE_SIZE;
};
static_assert(AGENTXX_IFACE_CLIENT_SESSION_BASELINE_SIZE >= 8u);
static_assert(AGENTXX_IFACE_CLIENT_SESSION_BASELINE_SIZE <= sizeof(AgentxxClientSessionIface));

template<>
struct IfaceMeta<AgentxxClientWireIface> {
    static constexpr int32_t  min_version   = AGENTXX_IFACE_CLIENT_WIRE_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_IFACE_CLIENT_WIRE_BASELINE_SIZE;
};
static_assert(AGENTXX_IFACE_CLIENT_WIRE_BASELINE_SIZE >= 8u);
static_assert(AGENTXX_IFACE_CLIENT_WIRE_BASELINE_SIZE <= sizeof(AgentxxClientWireIface));

template<>
struct IfaceMeta<AgentxxClientSelfIface> {
    static constexpr int32_t  min_version   = AGENTXX_IFACE_CLIENT_SELF_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_IFACE_CLIENT_SELF_BASELINE_SIZE;
};
static_assert(AGENTXX_IFACE_CLIENT_SELF_BASELINE_SIZE >= 8u);
static_assert(AGENTXX_IFACE_CLIENT_SELF_BASELINE_SIZE <= sizeof(AgentxxClientSelfIface));

template<>
struct IfaceMeta<AgentxxClientJsonIface> {
    static constexpr int32_t  min_version   = AGENTXX_IFACE_CLIENT_JSON_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_IFACE_CLIENT_JSON_BASELINE_SIZE;
};
static_assert(AGENTXX_IFACE_CLIENT_JSON_BASELINE_SIZE >= 8u);
static_assert(AGENTXX_IFACE_CLIENT_JSON_BASELINE_SIZE <= sizeof(AgentxxClientJsonIface));

template<>
struct IfaceMeta<AgentxxClientLogIface> {
    static constexpr int32_t  min_version   = AGENTXX_IFACE_CLIENT_LOG_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_IFACE_CLIENT_LOG_BASELINE_SIZE;
};
static_assert(AGENTXX_IFACE_CLIENT_LOG_BASELINE_SIZE >= 8u);
static_assert(AGENTXX_IFACE_CLIENT_LOG_BASELINE_SIZE <= sizeof(AgentxxClientLogIface));

template<>
struct IfaceMeta<AgentxxClientTimerIface> {
    static constexpr int32_t  min_version   = AGENTXX_IFACE_CLIENT_TIMER_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_IFACE_CLIENT_TIMER_BASELINE_SIZE;
};
static_assert(AGENTXX_IFACE_CLIENT_TIMER_BASELINE_SIZE >= 8u);
static_assert(AGENTXX_IFACE_CLIENT_TIMER_BASELINE_SIZE <= sizeof(AgentxxClientTimerIface));

template<>
struct IfaceMeta<AgentxxClientKeybindIface> {
    static constexpr int32_t  min_version   = AGENTXX_IFACE_CLIENT_KEYBIND_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_IFACE_CLIENT_KEYBIND_BASELINE_SIZE;
};
static_assert(AGENTXX_IFACE_CLIENT_KEYBIND_BASELINE_SIZE >= 8u);
static_assert(AGENTXX_IFACE_CLIENT_KEYBIND_BASELINE_SIZE <= sizeof(AgentxxClientKeybindIface));

} // namespace pluginxx

#endif /* AGENTXX_PLUGIN_API_PLUGIN_IFACE_META_H */
