/*
 * agentxx/plugin/plugin_iface_helper.h —— 插件侧接口表查询便捷设施 (C++ header-only)
 *
 * 定位: COM 风格接口表架构下的插件侧便捷层 —— entry 时一次性查询全部已知
 * IID 并缓存指针, 后续经成员直接调用 (避免散落的手写 query_interface 转型)。
 *
 * - 【非跨边界 ABI】: 纯头文件内联设施, 仅简化插件侧样板代码; 第三方插件
 *   可不用本头而直接调 host->vtable->query_interface (纯 C 路径不受影响)
 * - 未实现的接口表对应成员为 NULL, 使用前判空 (与 query_interface 契约一致)
 * - 接口表为进程级静态只读数据: 查询结果长期有效, 无需重复查询
 */
#pragma once

#include "agentxx/plugin/client_plugin_api.h"
#include "agentxx/plugin/plugin_api.h"

namespace agentxx {
namespace plugin {

/// agent 侧接口表聚合 (一次查询; 成员为 NULL 表示宿主未实现该接口)
struct AgentIfaces {
    const AgentxxToolsIface*        tools        = nullptr; ///< "agentxx.agent.tools"
    const AgentxxHooksIface*        hooks        = nullptr; ///< "agentxx.agent.hooks"
    const AgentxxEventsIface*       events       = nullptr; ///< "agentxx.agent.events"
    const AgentxxCapabilitiesIface* capabilities = nullptr; ///< "agentxx.agent.capabilities"
    const AgentxxSchedulerIface*    scheduler    = nullptr; ///< "agentxx.agent.scheduler"
    const AgentxxSessionIface*      session      = nullptr; ///< "agentxx.agent.session"
    const AgentxxPluginsIface*      plugins      = nullptr; ///< "agentxx.agent.plugins"
    const AgentxxConfigIface*       config       = nullptr; ///< "agentxx.agent.config"
    const AgentxxPromptIface*       prompt       = nullptr; ///< "agentxx.agent.prompt"
    const AgentxxJsonIface*         json         = nullptr; ///< "agentxx.agent.json"
    const AgentxxLogIface*          log          = nullptr; ///< "agentxx.agent.log"
    const AgentxxResourcesIface*    resources    = nullptr; ///< "agentxx.agent.resources"
    const AgentxxModelIface*        model        = nullptr; ///< "agentxx.agent.model"
    const AgentxxCancelIface*       cancel       = nullptr; ///< "agentxx.agent.cancel"
    const AgentxxPlanningIface*     planning     = nullptr; ///< "agentxx.agent.planning"

    /// 从宿主查询全部已知 agent 侧接口表 (host 为空时返回全 NULL 聚合)
    static AgentIfaces query(const AgentxxHost* host) {
        AgentIfaces f;
        if (!host || !host->vtable || !host->vtable->query_interface) {
            return f;
        }
        f.tools  = AGENTXX_QUERY_IFACE(host, AgentxxToolsIface, AGENTXX_IFACE_AGENT_TOOLS);
        f.hooks  = AGENTXX_QUERY_IFACE(host, AgentxxHooksIface, AGENTXX_IFACE_AGENT_HOOKS);
        f.events = AGENTXX_QUERY_IFACE(host, AgentxxEventsIface, AGENTXX_IFACE_AGENT_EVENTS);
        f.capabilities
            = AGENTXX_QUERY_IFACE(host, AgentxxCapabilitiesIface, AGENTXX_IFACE_AGENT_CAPABILITIES);
        f.scheduler
            = AGENTXX_QUERY_IFACE(host, AgentxxSchedulerIface, AGENTXX_IFACE_AGENT_SCHEDULER);
        f.session = AGENTXX_QUERY_IFACE(host, AgentxxSessionIface, AGENTXX_IFACE_AGENT_SESSION);
        f.plugins = AGENTXX_QUERY_IFACE(host, AgentxxPluginsIface, AGENTXX_IFACE_AGENT_PLUGINS);
        f.config  = AGENTXX_QUERY_IFACE(host, AgentxxConfigIface, AGENTXX_IFACE_AGENT_CONFIG);
        f.prompt  = AGENTXX_QUERY_IFACE(host, AgentxxPromptIface, AGENTXX_IFACE_AGENT_PROMPT);
        f.json    = AGENTXX_QUERY_IFACE(host, AgentxxJsonIface, AGENTXX_IFACE_AGENT_JSON);
        f.log     = AGENTXX_QUERY_IFACE(host, AgentxxLogIface, AGENTXX_IFACE_AGENT_LOG);
        f.resources
            = AGENTXX_QUERY_IFACE(host, AgentxxResourcesIface, AGENTXX_IFACE_AGENT_RESOURCES);
        f.model    = AGENTXX_QUERY_IFACE(host, AgentxxModelIface, AGENTXX_IFACE_AGENT_MODEL);
        f.cancel   = AGENTXX_QUERY_IFACE(host, AgentxxCancelIface, AGENTXX_IFACE_AGENT_CANCEL);
        f.planning = AGENTXX_QUERY_IFACE(host, AgentxxPlanningIface, AGENTXX_IFACE_AGENT_PLANNING);
        return f;
    }
};

/// client 侧接口表聚合 (一次查询; 成员为 NULL 表示宿主未实现该接口)
struct ClientIfaces {
    const AgentxxClientUiIface*      ui      = nullptr; ///< "agentxx.client.ui"
    const AgentxxClientEventsIface*  events  = nullptr; ///< "agentxx.client.events"
    const AgentxxClientSessionIface* session = nullptr; ///< "agentxx.client.session"
    const AgentxxClientWireIface*    wire    = nullptr; ///< "agentxx.client.wire"
    const AgentxxClientSelfIface*    self    = nullptr; ///< "agentxx.client.self"
    const AgentxxClientJsonIface*    json    = nullptr; ///< "agentxx.client.json"
    const AgentxxClientLogIface*     log     = nullptr; ///< "agentxx.client.log"

    /// 从宿主查询全部已知 client 侧接口表 (host 为空时返回全 NULL 聚合)
    static ClientIfaces query(const AgentxxClientHost* host) {
        ClientIfaces f;
        if (!host || !host->vtable || !host->vtable->query_interface) {
            return f;
        }
        f.ui     = AGENTXX_QUERY_IFACE(host, AgentxxClientUiIface, AGENTXX_IFACE_CLIENT_UI);
        f.events = AGENTXX_QUERY_IFACE(host, AgentxxClientEventsIface, AGENTXX_IFACE_CLIENT_EVENTS);
        f.session
            = AGENTXX_QUERY_IFACE(host, AgentxxClientSessionIface, AGENTXX_IFACE_CLIENT_SESSION);
        f.wire = AGENTXX_QUERY_IFACE(host, AgentxxClientWireIface, AGENTXX_IFACE_CLIENT_WIRE);
        f.self = AGENTXX_QUERY_IFACE(host, AgentxxClientSelfIface, AGENTXX_IFACE_CLIENT_SELF);
        f.json = AGENTXX_QUERY_IFACE(host, AgentxxClientJsonIface, AGENTXX_IFACE_CLIENT_JSON);
        f.log  = AGENTXX_QUERY_IFACE(host, AgentxxClientLogIface, AGENTXX_IFACE_CLIENT_LOG);
        return f;
    }
};

} // namespace plugin
} // namespace agentxx
