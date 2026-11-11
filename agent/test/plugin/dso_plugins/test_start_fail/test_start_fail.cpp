/// test_start_fail_plugin —— 加载期 start 失败回滚用例的测试专用插件 (真实 DSO)
///
/// 行为：start 依次完成全部注册，然后**主动失败**：
///   1. 注册工具           `dso_rollback_tool`
///   2. 注册图节点类型      `dso_rollback_type`
///   3. 订阅事件            `plugin.test_start_fail_plugin.watch`
///   4. prompt 贡献         `appendSystemPrompts.dso_rollback`
///   5. 订阅通知型扩展点频道 `agentxx.agent.point.model.start`
///   6. 注册能力            `dso.rollback.cap`
///   7. 注册资源            skill 目录 `/tmp/agentxx_dso_rollback_skills`
///   8. 发布 `{"step":"all","ok":true}` 到 `plugin.test_start_fail_plugin.probe`，然后返回 NULL + error
///
/// 若任一步骤失败，则发布 `{"step":"<name>","ok":false}` 并返回该失败。
/// 测试据此验证：
/// - start 失败必须回滚全部已生效注册（工具/图/订阅/prompt/扩展点订阅/能力/资源）；
/// - 回滚后再次加载同一 DSO 仍能重新注册同名条目（第一次的注册没有残留）。
///
/// 仅使用纯 C ABI 头；不依赖 libagentxx。
#include "agentxx/plugin/api/plugin_api.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr PluginxxStringView cstrView(const char* text) {
    uint64_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return PluginxxStringView{text, length};
}

PluginxxStringView sv(const char* text) {
    PluginxxStringView view{};
    view.data = text;
    view.size = static_cast<uint64_t>(std::strlen(text));
    return view;
}

void setError(const PluginxxHost* host, PluginxxString* out, const char* text) {
    if (!out) {
        return;
    }
    const auto     view   = sv(text);
    const uint64_t size   = view.size;
    auto*          buffer = static_cast<char*>(host->vtable->alloc(size + 1));
    if (!buffer) {
        out->data = nullptr;
        out->size = 0;
        return;
    }
    std::memcpy(buffer, text, size);
    buffer[size] = '\0';
    out->data    = buffer;
    out->size    = size;
}

const void* queryIface(const PluginxxHost* host, const char* iid) {
    const auto view = sv(iid);
    return host->vtable->query_interface(host, &view);
}

/// 发布进度事件 (供测试断言每一步都真实执行过)
/// - 插件不再有事件表: 进度走广播频道 (先声明再发; 同一频道重复声明是幂等)
void report(const PluginxxHost* host, const char* step, bool ok) {
    const auto* broadcasts
        = static_cast<const PluginxxBroadcastsIface*>(queryIface(host, PLUGINXX_IFACE_BROADCASTS));
    if (!broadcasts || !broadcasts->publish_broadcast) {
        return;
    }
    const auto decl = sv(R"({"id":"plugin.test_start_fail_plugin.probe","cache":"latest"})");
    if (broadcasts->declare_broadcast) {
        PluginxxString declErr{nullptr, 0};
        broadcasts->declare_broadcast(host, &decl, &declErr);
        if (declErr.data) {
            host->vtable->free(declErr.data);
        }
    }
    const auto topic = sv("plugin.test_start_fail_plugin.probe");
    char       buffer[64];
    const int  length = std::snprintf(
        buffer,
        sizeof(buffer),
        R"({"step":"%s","ok":%s})",
        step,
        ok ? "true" : "false"
    );
    PluginxxStringView payload{};
    payload.data = buffer;
    payload.size = static_cast<uint64_t>(length > 0 ? length : 0);
    PluginxxString pubErr{nullptr, 0};
    broadcasts->publish_broadcast(host, &topic, &payload, &pubErr);
    if (pubErr.data) {
        host->vtable->free(pubErr.data);
    }
}

const char* failStep(const PluginxxHost* host, const char* step) {
    report(host, step, false);
    return step;
}

void* PLUGINXX_CALL
    probeToolStart(void*, const PluginxxStringView*, const PluginxxStringView*, const PluginxxStringView*, const PluginxxOperatorNotify* notify, PluginxxString*) {
    if (notify && notify->done) {
        const auto payload = sv(R"({"ok":true})");
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &payload);
    }
    return nullptr;
}

void* PLUGINXX_CALL
    probeNodeRunStart(void*, const PluginxxStringView*, const PluginxxStringView*, const PluginxxStringView*, const PluginxxStringView*, const PluginxxOperatorNotify* notify, PluginxxString*) {
    if (notify && notify->done) {
        const auto payload = sv("{}");
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &payload);
    }
    return nullptr;
}

void PLUGINXX_CALL onProbeEvent(const PluginxxStringView*, void*) {
    // 回滚用例只需要"订阅存在/被撤销"这一事实；handler 本身不做任何事。
}

void* PLUGINXX_CALL
    probeCapabilityStart(void*, const PluginxxHost*, const PluginxxStringView*, const PluginxxStringView*, const PluginxxOperatorNotify* notify, PluginxxString*) {
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return nullptr;
}

struct ProbeCtx {
    const PluginxxHost* host = nullptr;
};

} // namespace

extern "C" PLUGINXX_EXPORT const PluginxxInfo* agentxx_plugin_agent_get_info(void) {
    static const PluginxxInfo info{
        PLUGINXX_API_VERSION,
        0,
        cstrView("test_start_fail_plugin"),
        cstrView("1.0.0"),
        cstrView("Test-only plugin: fails start after full registration (rollback fixture)"),
    };
    return &info;
}

extern "C" PLUGINXX_EXPORT int
    agentxx_plugin_agent_create(const PluginxxHost* host, void** plugin_ctx) {
    if (!host || !host->vtable || !plugin_ctx) {
        return -1;
    }
    auto* ctx = static_cast<ProbeCtx*>(host->vtable->alloc(sizeof(ProbeCtx)));
    if (!ctx) {
        return -1;
    }
    ctx->host   = host;
    *plugin_ctx = ctx;
    return 0;
}

/// 挂载事务: 逐步注册 (任一步失败即返回, 主程序回滚已生效的注册)
/// - v2 起 agent 级注册必须带引用, 因此整段从 start 搬到 mount
static void* probeMount(
    void*                              plugin_ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error_out
) {
    (void)notify;
    PluginxxScopeRef ref{};
    ref.struct_size = sizeof(PluginxxScopeRef);
    if (spec) {
        ref.id  = spec->agent_id;
        ref.seq = spec->mount_seq;
    }
    auto* ctx = static_cast<ProbeCtx*>(plugin_ctx);
    if (!ctx || !ctx->host) {
        if (error_out) {
            error_out->data = nullptr;
            error_out->size = 0;
        }
        return nullptr;
    }
    const PluginxxHost* host = ctx->host;

    // 1. 工具
    const auto* tools = static_cast<const AgentxxPluginToolsIface*>(
        queryIface(host, AGENTXX_PLUGIN_IFACE_AGENT_TOOLS)
    );
    if (!tools || !tools->register_tool) {
        setError(host, error_out, failStep(host, "tool_iface"));
        return nullptr;
    }
    AgentxxPluginToolSpec tool{};
    tool.name            = sv("dso_rollback_tool");
    tool.description     = sv("rollback probe tool");
    tool.parameters_json = sv("{}");
    tool.execute_start   = &probeToolStart;
    if (tools->register_tool(host, &ref, &tool) != 0) {
        setError(host, error_out, failStep(host, "tool"));
        return nullptr;
    }

    // 2. 图节点类型
    const auto* graph = static_cast<const AgentxxPluginGraphIface*>(
        queryIface(host, AGENTXX_PLUGIN_IFACE_AGENT_GRAPH)
    );
    if (!graph || !graph->register_node_type) {
        setError(host, error_out, failStep(host, "graph_iface"));
        return nullptr;
    }
    AgentxxPluginGraphNodeTypeSpec node{};
    node.type               = sv("dso_rollback_type");
    node.run_start          = &probeNodeRunStart;
    node.config_schema_json = sv("{}");
    if (graph->register_node_type(host, &ref, &node) != 0) {
        setError(host, error_out, failStep(host, "graph"));
        return nullptr;
    }

    // 3. 广播订阅
    const auto* broadcasts
        = static_cast<const PluginxxBroadcastsIface*>(queryIface(host, PLUGINXX_IFACE_BROADCASTS));
    if (!broadcasts || !broadcasts->subscribe_broadcast) {
        setError(host, error_out, failStep(host, "broadcasts_iface"));
        return nullptr;
    }
    const auto watchTopic = sv("plugin.test_start_fail_plugin.watch");
    // 广播频道必须先声明再订阅 (声明为属主发布 + 不缓存)
    if (broadcasts->declare_broadcast) {
        const auto     watchDecl = sv(R"({"id":"plugin.test_start_fail_plugin.watch","cache":"none"})");
        PluginxxString declErr{nullptr, 0};
        broadcasts->declare_broadcast(host, &watchDecl, &declErr);
        if (declErr.data) {
            host->vtable->free(declErr.data);
        }
    }
    if (!broadcasts->subscribe_broadcast(
            host,
            &ref,
            &watchTopic,
            &onProbeEvent,
            ctx,
            nullptr,
            nullptr
        )) {
        setError(host, error_out, failStep(host, "subscribe"));
        return nullptr;
    }

    // 4. prompt 贡献
    const auto* prompt = static_cast<const AgentxxPluginPromptIface*>(
        queryIface(host, AGENTXX_PLUGIN_IFACE_AGENT_PROMPT)
    );
    if (!prompt || !prompt->set_prompt) {
        setError(host, error_out, failStep(host, "prompt_iface"));
        return nullptr;
    }
    const auto promptJson = sv(R"({"appendSystemPrompts":{"dso_rollback":"probe"}})");
    if (prompt->set_prompt(host, &ref, &promptJson) != 0) {
        setError(host, error_out, failStep(host, "prompt"));
        return nullptr;
    }

    // 5. 订阅通知型扩展点 (频道由主程序声明, 这里只订阅; 回滚时随实例整批撤销)
    const auto pointTopic = sv("agentxx.agent.point.model.start");
    if (!broadcasts->subscribe_broadcast(
            host,
            &ref,
            &pointTopic,
            &onProbeEvent,
            ctx,
            nullptr,
            nullptr
        )) {
        setError(host, error_out, failStep(host, "point_subscribe"));
        return nullptr;
    }

    // 6. 能力注册
    const auto* capabilities = static_cast<const PluginxxCapabilitiesIface*>(
        queryIface(host, PLUGINXX_IFACE_CAPABILITIES)
    );
    if (!capabilities || !capabilities->register_capability) {
        setError(host, error_out, failStep(host, "capabilities_iface"));
        return nullptr;
    }
    const auto             capability = sv("dso.rollback.cap");
    PluginxxCapabilitySpec capSpec{};
    capSpec.version     = PLUGINXX_API_VERSION;
    capSpec.struct_size = sizeof(PluginxxCapabilitySpec);
    capSpec.id          = capability;
    capSpec.impls       = PLUGINXX_CAPABILITY_IMPLS_ONE;
    capSpec.merge       = PLUGINXX_CAPABILITY_MERGE_NONE;
    capSpec.phases      = PLUGINXX_CAPABILITY_PHASE_MASK_IMPL;
    capSpec.phase       = PLUGINXX_CAPABILITY_PHASE_IMPL;
    capSpec.sync        = 0;
    capSpec.run_start   = &probeCapabilityStart;
    capSpec.user_data   = ctx;
    PluginxxString capErr{nullptr, 0};
    if (capabilities->register_capability(host, &ref, &capSpec, &capErr) != 0) {
        if (capErr.data) {
            host->vtable->free(capErr.data);
        }
        setError(host, error_out, failStep(host, "capability"));
        return nullptr;
    }
    if (capErr.data) {
        host->vtable->free(capErr.data);
    }

    // 7. 资源注册 (skill 扫描目录; 所有权归本插件, 卸载/回滚时摘除)
    const auto* resources = static_cast<const AgentxxPluginResourcesIface*>(
        queryIface(host, AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES)
    );
    if (!resources || !resources->register_skill_dir) {
        setError(host, error_out, failStep(host, "resources_iface"));
        return nullptr;
    }
    const auto skillDir = sv("/tmp/agentxx_dso_rollback_skills");
    if (resources->register_skill_dir(host, &ref, &skillDir) != 0) {
        setError(host, error_out, failStep(host, "resource"));
        return nullptr;
    }

    // 8. 全部注册成功后主动失败 (触发主程序回滚)
    report(host, "all", true);
    setError(host, error_out, "mount failed on purpose after full registration");
    return nullptr;
}

extern "C" PLUGINXX_EXPORT void* agentxx_plugin_agent_start(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
) {
    (void)plugin_ctx;
    (void)error_out;
    // 进程级启动: 本探针的注册全部在 mount 里做
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return nullptr;
}

extern "C" PLUGINXX_EXPORT void* agentxx_plugin_agent_mount(
    void*                              plugin_ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error_out
) {
    return probeMount(plugin_ctx, spec, notify, error_out);
}

extern "C" PLUGINXX_EXPORT void* agentxx_plugin_agent_unmount(
    void*                         plugin_ctx,
    void*                         mount_ud,
    const PluginxxOperatorNotify* notify,
    PluginxxString*
) {
    (void)plugin_ctx;
    (void)mount_ud;
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return nullptr;
}

extern "C" PLUGINXX_EXPORT void*
    agentxx_plugin_agent_stop(void*, const PluginxxOperatorNotify* notify, PluginxxString*) {
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return nullptr;
}

extern "C" PLUGINXX_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ProbeCtx*>(plugin_ctx);
    if (ctx && ctx->host) {
        ctx->host->vtable->free(ctx);
    }
}
