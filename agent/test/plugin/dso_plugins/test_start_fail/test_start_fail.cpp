/// test_start_fail_plugin —— 加载期 start 失败回滚用例的测试专用插件 (真实 DSO)
///
/// 行为：start 依次完成全部注册，然后**主动失败**：
///   1. 注册工具           `dso_rollback_tool`
///   2. 注册图节点类型      `dso_rollback_type`
///   3. 订阅事件            `dso_rollback.watch`
///   4. prompt 贡献         `appendSystemPrompts.dso_rollback`
///   5. 发布 `{"step":"all","ok":true}` 到 `dso_rollback.probe`，然后返回 NULL + error
///
/// 若任一步骤失败，则发布 `{"step":"<name>","ok":false}` 并返回该失败。
/// 测试据此验证：
/// - start 失败必须回滚全部已生效注册（工具/图/订阅/prompt）；
/// - 回滚后再次加载同一 DSO 仍能重新注册同名条目（第一次的注册没有残留）。
///
/// 仅使用纯 C ABI 头；不依赖 libagentxx。
#include "agentxx/plugin/api/plugin_api.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr AgentxxPluginStringView cstrView(const char* text) {
    uint64_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return AgentxxPluginStringView{text, length};
}

AgentxxPluginStringView sv(const char* text) {
    AgentxxPluginStringView view{};
    view.data = text;
    view.size = static_cast<uint64_t>(std::strlen(text));
    return view;
}

void setError(const AgentxxPluginHost* host, AgentxxPluginString* out, const char* text) {
    if (!out) {
        return;
    }
    const auto view = sv(text);
    const uint64_t size = view.size;
    auto* buffer = static_cast<char*>(host->vtable->alloc(size + 1));
    if (!buffer) {
        out->data = nullptr;
        out->size = 0;
        return;
    }
    std::memcpy(buffer, text, size);
    buffer[size] = '\0';
    out->data = buffer;
    out->size = size;
}

const void* queryIface(const AgentxxPluginHost* host, const char* iid) {
    const auto view = sv(iid);
    return host->vtable->query_interface(host, &view);
}

/// 发布进度事件 (供测试断言每一步都真实执行过)
void report(const AgentxxPluginHost* host, const char* step, bool ok) {
    const auto* events = static_cast<const AgentxxPluginEventsIface*>(
        queryIface(host, AGENTXX_PLUGIN_IFACE_AGENT_EVENTS)
    );
    if (!events || !events->publish) {
        return;
    }
    const auto topic = sv("dso_rollback.probe");
    char buffer[64];
    const int length = std::snprintf(
        buffer,
        sizeof(buffer),
        R"({"step":"%s","ok":%s})",
        step,
        ok ? "true" : "false"
    );
    AgentxxPluginStringView payload{};
    payload.data = buffer;
    payload.size = static_cast<uint64_t>(length > 0 ? length : 0);
    events->publish(host, &topic, &payload);
}

const char* failStep(const AgentxxPluginHost* host, const char* step) {
    report(host, step, false);
    return step;
}

void* AGENTXX_PLUGIN_CALL probeToolStart(
    void*,
    const AgentxxPluginStringView*,
    const AgentxxPluginStringView*,
    const AgentxxPluginStringView*,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*
) {
    if (notify && notify->done) {
        const auto payload = sv(R"({"ok":true})");
        notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, &payload);
    }
    return nullptr;
}

void* AGENTXX_PLUGIN_CALL probeNodeRunStart(
    void*,
    const AgentxxPluginStringView*,
    const AgentxxPluginStringView*,
    const AgentxxPluginStringView*,
    const AgentxxPluginStringView*,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*
) {
    if (notify && notify->done) {
        const auto payload = sv("{}");
        notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, &payload);
    }
    return nullptr;
}

void AGENTXX_PLUGIN_CALL onProbeEvent(const AgentxxPluginStringView*, void*) {
    // 回滚用例只需要"订阅存在/被撤销"这一事实；handler 本身不做任何事。
}

struct ProbeCtx {
    const AgentxxPluginHost* host = nullptr;
};

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        0,
        cstrView("test_start_fail_plugin"),
        cstrView("1.0.0"),
        cstrView("Test-only plugin: fails start after full registration (rollback fixture)"),
    };
    return &info;
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    if (!host || !host->vtable || !plugin_ctx) {
        return -1;
    }
    auto* ctx = static_cast<ProbeCtx*>(host->vtable->alloc(sizeof(ProbeCtx)));
    if (!ctx) {
        return -1;
    }
    ctx->host  = host;
    *plugin_ctx = ctx;
    return 0;
}

extern "C" AGENTXX_PLUGIN_EXPORT void* agentxx_plugin_agent_start(
    void* plugin_ctx, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString* error_out
) {
    (void)notify;
    auto* ctx = static_cast<ProbeCtx*>(plugin_ctx);
    if (!ctx || !ctx->host) {
        if (error_out) {
            error_out->data = nullptr;
            error_out->size = 0;
        }
        return nullptr;
    }
    const AgentxxPluginHost* host = ctx->host;

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
    if (tools->register_tool(host, &tool) != 0) {
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
    if (graph->register_node_type(host, &node) != 0) {
        setError(host, error_out, failStep(host, "graph"));
        return nullptr;
    }

    // 3. 事件订阅
    const auto* events = static_cast<const AgentxxPluginEventsIface*>(
        queryIface(host, AGENTXX_PLUGIN_IFACE_AGENT_EVENTS)
    );
    if (!events || !events->subscribe) {
        setError(host, error_out, failStep(host, "events_iface"));
        return nullptr;
    }
    const auto watchTopic = sv("dso_rollback.watch");
    if (!events->subscribe(host, &watchTopic, &onProbeEvent, ctx)) {
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
    if (prompt->set_prompt(host, &promptJson) != 0) {
        setError(host, error_out, failStep(host, "prompt"));
        return nullptr;
    }

    // 5. 全部注册成功后主动失败 (触发宿主回滚)
    report(host, "all", true);
    setError(host, error_out, "start failed on purpose after full registration");
    return nullptr;
}

extern "C" AGENTXX_PLUGIN_EXPORT void* agentxx_plugin_agent_stop(
    void*, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*
) {
    if (notify && notify->done) {
        notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    }
    return nullptr;
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ProbeCtx*>(plugin_ctx);
    if (ctx && ctx->host) {
        ctx->host->vtable->free(ctx);
    }
}
