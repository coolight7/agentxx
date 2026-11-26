/// test_start_fail_plugin —— 加载期 start 失败回滚用例的测试专用插件 (真实 DSO)
///
/// 行为：mount 依次完成全部注册，然后**主动失败**：
///   1. 注册工具           `dso_rollback_tool`
///   2. 注册图节点类型      `dso_rollback_type`
///   3. 订阅事件            `plugin.test_start_fail_plugin.watch`
///   4. prompt 贡献         `appendSystemPrompts.dso_rollback`
///   5. 订阅通知型功能埋点频道 `agentxx.agent.point.model.start`
///   6. 注册能力            `dso.rollback.cap`
///   7. 注册资源            skill 目录 `/tmp/agentxx_dso_rollback_skills`
///   8. 发布 `{"step":"all","ok":true}` 到 `plugin.test_start_fail_plugin.probe`，然后以失败收尾
///
/// 若任一步骤失败，则发布 `{"step":"<name>","ok":false}` 并以该失败收尾。
/// 测试据此验证：
/// - 挂载失败必须回滚全部已生效注册（工具/图/订阅/prompt/功能埋点订阅/能力/资源）；
/// - 回滚后再次加载同一 DSO 仍能重新注册同名条目（第一次的注册没有残留）。
///
/// 仅使用纯 C ABI 头；不依赖 libagentxx。
///
/// 线程与协议要点: 表成员是异步契约 —— 插件代码跑在自己的执行线程上 (主程序没起
/// 插件线程时是 io 线程), 注册结果经回调交回同一条线程, 因此这里**不能**同步等,
/// 只能用一个小状态机逐步推进, 终态经 mount 的 `notify.done` 上报。
#include "agentxx/plugin/api/plugin_api.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

namespace {

PluginxxStringView sv(const char* text) {
    PluginxxStringView view{};
    view.data = text;
    view.size = static_cast<uint64_t>(std::strlen(text));
    return view;
}

PluginxxStringView svOf(const std::string& text) {
    PluginxxStringView view{};
    view.data = text.data();
    view.size = static_cast<uint64_t>(text.size());
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
/// - 声明与发布的载荷由主程序当场复制, 因此这里的字面量/栈缓冲只在调用期间有效
void report(const PluginxxHost* host, const char* step, bool ok) {
    const auto* broadcasts
        = static_cast<const PluginxxBroadcastsIface*>(queryIface(host, PLUGINXX_IFACE_BROADCASTS));
    if (!broadcasts || !broadcasts->publish_broadcast) {
        return;
    }
    const auto decl = sv(R"({"id":"plugin.test_start_fail_plugin.probe","cache":"latest"})");
    if (broadcasts->declare_broadcast) {
        PluginxxString declErr{nullptr, 0};
        // 表成员是异步契约: 不关心结果, 只受理
        broadcasts->declare_broadcast(host, &decl, nullptr, nullptr, &declErr);
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
    broadcasts->publish_broadcast(host, &topic, &payload, nullptr, nullptr, &pubErr);
    if (pubErr.data) {
        host->vtable->free(pubErr.data);
    }
}

void* PLUGINXX_CALL
    probeToolStart(void*, const PluginxxStringView*, const PluginxxStringView*, const PluginxxStringView*, const AgentxxPluginToolNotify* notify, PluginxxString*) {
    if (notify && notify->done) {
        const auto result  = sv(R"({"ok":true})");
        const auto emptySv = sv("");
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &result, &emptySv);
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

/// 挂载事务的每一步 (顺序即注册顺序; 名字用于失败时上报)
constexpr int         kMountStepCount = 7;
constexpr const char* kMountStepNames[kMountStepCount]
    = {"tool", "graph", "subscribe", "prompt", "point_subscribe", "capability", "resource"};

/// 挂载事务的状态机 (表成员是异步契约, 只能一步接一步推进)
///
/// - `syncDepth` 记"当前是否仍在 mount 入口的调用栈里": 没有独立插件线程时表调用
///   会内联完成, 终态回调可能在 `probeMount` 返回之前就跑到 —— 那时不能 delete,
///   交给入口收尾;
/// - `completed` 与 `syncDepth` 配合说明"已经收过尾了"。
struct MountJob {
    const PluginxxHost*    host     = nullptr;
    PluginxxOperatorNotify notify{nullptr, nullptr};
    std::string            agentId;
    uint64_t               mountSeq   = 0;
    int                    step       = 0;
    int                    syncDepth  = 0;
    bool                   completed  = false;
};

void runMountStep(MountJob* job);

/// 收尾 (失败): 上报这一步的失败并交回终态
void finishMountFail(MountJob* job, const char* step, const std::string& message) {
    if (!job || job->completed) {
        return;
    }
    job->completed = true;
    report(job->host, step, false);
    std::string text = message.empty() ? std::string{"registration rejected"} : message;
    const auto  view = svOf(text);
    if (job->notify.done) {
        job->notify.done(job->notify.host_ud, PLUGINXX_OPERATOR_FAILED, &view);
    }
    if (job->syncDepth == 0) {
        delete job;
    }
}

/// 收尾 (主动失败): 全部注册都已生效, 这里按用例要求失败以触发回滚
void finishMountOnPurpose(MountJob* job) {
    if (!job || job->completed) {
        return;
    }
    job->completed = true;
    report(job->host, "all", true);
    const auto view = sv("mount failed on purpose after full registration");
    if (job->notify.done) {
        job->notify.done(job->notify.host_ud, PLUGINXX_OPERATOR_FAILED, &view);
    }
    if (job->syncDepth == 0) {
        delete job;
    }
}

/// 一步注册的完成回调 (插件执行线程; 没起插件线程时是 io 线程)
void PLUGINXX_CALL mountStepDone(void* ud, int32_t status, const PluginxxStringView* payload) {
    auto* job = static_cast<MountJob*>(ud);
    if (!job || job->completed) {
        return;
    }
    if (status != PLUGINXX_OPERATOR_OK) {
        std::string message = "registration failed";
        if (payload && payload->data && payload->size > 0) {
            message.assign(payload->data, static_cast<size_t>(payload->size));
        }
        finishMountFail(job, kMountStepNames[job->step], message);
        return;
    }
    ++job->step;
    runMountStep(job);
}

/// 把当前 `step` 指向的注册提交出去 (受理失败就地收尾)
void runMountStep(MountJob* job) {
    if (!job || job->completed) {
        return;
    }
    if (job->step >= kMountStepCount) {
        finishMountOnPurpose(job);
        return;
    }
    const PluginxxHost* host = job->host;
    PluginxxScopeRef    ref{};
    ref.struct_size = sizeof(PluginxxScopeRef);
    ref.id          = svOf(job->agentId);
    ref.seq         = job->mountSeq;

    PluginxxString          err{nullptr, 0};
    PluginxxOperatorHandle* op = nullptr;
    switch (job->step) {
    case 0: { // 工具
        const auto* tools = static_cast<const AgentxxPluginToolsIface*>(
            queryIface(host, AGENTXX_PLUGIN_IFACE_AGENT_TOOLS)
        );
        if (!tools || !tools->register_tool) {
            finishMountFail(job, "tool_iface", "tools interface is unavailable");
            return;
        }
        AgentxxPluginToolSpec tool{};
        tool.name            = sv("dso_rollback_tool");
        tool.description     = sv("rollback probe tool");
        tool.parameters_json = sv("{}");
        tool.execute_start   = &probeToolStart;
        op = tools->register_tool(host, &ref, &tool, &mountStepDone, job, &err);
        break;
    }
    case 1: { // 图节点类型
        const auto* graph = static_cast<const AgentxxPluginGraphIface*>(
            queryIface(host, AGENTXX_PLUGIN_IFACE_AGENT_GRAPH)
        );
        if (!graph || !graph->register_node_type) {
            finishMountFail(job, "graph_iface", "graph interface is unavailable");
            return;
        }
        AgentxxPluginGraphNodeTypeSpec node{};
        node.type               = sv("dso_rollback_type");
        node.run_start          = &probeNodeRunStart;
        node.config_schema_json = sv("{}");
        op = graph->register_node_type(host, &ref, &node, &mountStepDone, job, &err);
        break;
    }
    case 2: { // 广播订阅 (频道必须先声明再订阅)
        const auto* broadcasts = static_cast<const PluginxxBroadcastsIface*>(
            queryIface(host, PLUGINXX_IFACE_BROADCASTS)
        );
        if (!broadcasts || !broadcasts->subscribe_broadcast) {
            finishMountFail(job, "broadcasts_iface", "broadcasts interface is unavailable");
            return;
        }
        if (broadcasts->declare_broadcast) {
            const auto     decl = sv(R"({"id":"plugin.test_start_fail_plugin.watch","cache":"none"})");
            PluginxxString declErr{nullptr, 0};
            broadcasts->declare_broadcast(host, &decl, nullptr, nullptr, &declErr);
            if (declErr.data) {
                host->vtable->free(declErr.data);
            }
        }
        const auto watchTopic = sv("plugin.test_start_fail_plugin.watch");
        op                    = broadcasts->subscribe_broadcast(
            host,
            &ref,
            &watchTopic,
            &onProbeEvent,
            job,
            nullptr,
            &mountStepDone,
            job,
            &err
        );
        break;
    }
    case 3: { // prompt 贡献
        const auto* prompt = static_cast<const AgentxxPluginPromptIface*>(
            queryIface(host, AGENTXX_PLUGIN_IFACE_AGENT_PROMPT)
        );
        if (!prompt || !prompt->set_prompt) {
            finishMountFail(job, "prompt_iface", "prompt interface is unavailable");
            return;
        }
        const auto promptJson = sv(R"({"appendSystemPrompts":{"dso_rollback":"probe"}})");
        op = prompt->set_prompt(host, &ref, &promptJson, &mountStepDone, job, &err);
        break;
    }
    case 4: { // 订阅通知型功能埋点 (频道由主程序声明, 这里只订阅)
        const auto* broadcasts = static_cast<const PluginxxBroadcastsIface*>(
            queryIface(host, PLUGINXX_IFACE_BROADCASTS)
        );
        if (!broadcasts || !broadcasts->subscribe_broadcast) {
            finishMountFail(job, "broadcasts_iface", "broadcasts interface is unavailable");
            return;
        }
        const auto pointTopic = sv("agentxx.agent.point.model.start");
        op                    = broadcasts->subscribe_broadcast(
            host,
            &ref,
            &pointTopic,
            &onProbeEvent,
            job,
            nullptr,
            &mountStepDone,
            job,
            &err
        );
        break;
    }
    case 5: { // 能力注册
        const auto* capabilities = static_cast<const PluginxxCapabilitiesIface*>(
            queryIface(host, PLUGINXX_IFACE_CAPABILITIES)
        );
        if (!capabilities || !capabilities->register_capability) {
            finishMountFail(job, "capabilities_iface", "capabilities interface is unavailable");
            return;
        }
        PluginxxCapabilitySpec capSpec{};
        capSpec.version     = PLUGINXX_API_VERSION;
        capSpec.struct_size = sizeof(PluginxxCapabilitySpec);
        capSpec.id          = sv("dso.rollback.cap");
        capSpec.impls       = PLUGINXX_CAPABILITY_IMPLS_ONE;
        capSpec.merge       = PLUGINXX_CAPABILITY_MERGE_NONE;
        capSpec.phases      = PLUGINXX_CAPABILITY_PHASE_MASK_IMPL;
        capSpec.phase       = PLUGINXX_CAPABILITY_PHASE_IMPL;
        capSpec.run_start   = &probeCapabilityStart;
        capSpec.user_data   = nullptr;
        op = capabilities->register_capability(
            host,
            &ref,
            &capSpec,
            &mountStepDone,
            job,
            &err
        );
        break;
    }
    default: { // 资源注册 (skill 扫描目录; 所有权归本插件, 卸载/回滚时摘除)
        const auto* resources = static_cast<const AgentxxPluginResourcesIface*>(
            queryIface(host, AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES)
        );
        if (!resources || !resources->register_skill_dir) {
            finishMountFail(job, "resources_iface", "resources interface is unavailable");
            return;
        }
        const auto skillDir = sv("/tmp/agentxx_dso_rollback_skills");
        op = resources->register_skill_dir(host, &ref, &skillDir, &mountStepDone, job, &err);
        break;
    }
    }

    if (op == nullptr) {
        std::string message = "registration rejected";
        if (err.data) {
            message.assign(err.data, static_cast<size_t>(err.size));
            host->vtable->free(err.data);
        }
        finishMountFail(job, kMountStepNames[job->step], message);
    }
}

} // namespace

extern "C" PLUGINXX_EXPORT int32_t agentxx_agent_plugin_get_info(PluginxxInfo* out) {
    if (!out || out->struct_size < PLUGINXX_INFO_BASELINE_SIZE) {
        return -1;
    }
    out->min_api_version = AGENTXX_PLUGIN_MIN_API_VERSION;
    out->flags           = 0;
    out->name            = sv("test_start_fail_plugin");
    out->version         = sv("1.0.0");
    out->description
        = sv("Test-only plugin: fails mount after full registration (rollback fixture)");
    return 0;
}

extern "C" PLUGINXX_EXPORT void agentxx_agent_plugin_create(
    const PluginxxHost*           host,
    void**                        plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
) {
    if (!host || !host->vtable || !plugin_ctx) {
        if (notify && notify->done) {
            auto msg = sv("test_start_fail: invalid create call");
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &msg);
        }
        return;
    }
    // 可重入: 禁用后再启用只重新注册, 不重建上下文
    if (*plugin_ctx == nullptr) {
        auto* ctx = static_cast<ProbeCtx*>(host->vtable->alloc(sizeof(ProbeCtx)));
        if (!ctx) {
            if (notify && notify->done) {
                auto msg = sv("test_start_fail: out of memory");
                notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &msg);
            }
            return;
        }
        ctx->host   = host;
        *plugin_ctx = ctx;
    }
    // 进程级启动: 本探针的注册全部在 mount 里做
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
}

/// 挂载事务: 逐步注册 (任一步失败即收尾, 主程序回滚已生效的注册)
///
/// 返回非空 = 已受理, 终态经 `notify.done` 交回; 表调用内联完成时可能在这里就收尾,
/// 那时返回 NULL (主程序按"已提交完成"处理, 不会重复收尾)。
static void* probeMount(
    void*                              plugin_ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error_out
) {
    (void)error_out;
    auto* ctx = static_cast<ProbeCtx*>(plugin_ctx);
    if (!ctx || !ctx->host || !spec || !notify || !notify->done) {
        if (notify && notify->done) {
            auto msg = sv("test_start_fail: invalid mount call");
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &msg);
        }
        return nullptr;
    }
    auto* job = new (std::nothrow) MountJob();
    if (!job) {
        auto msg = sv("test_start_fail: out of memory");
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &msg);
        return nullptr;
    }
    job->host   = ctx->host;
    job->notify = *notify;
    job->mountSeq = spec->mount_seq;
    if (spec->agent_id.data != nullptr) {
        job->agentId.assign(spec->agent_id.data, static_cast<size_t>(spec->agent_id.size));
    }
    // 第一步之后的推进由 [mountStepDone] 完成 (同一实例的调用按受理顺序执行)
    ++job->syncDepth;
    runMountStep(job);
    --job->syncDepth;
    if (job->completed) {
        delete job;
        return nullptr;
    }
    return job;
}

extern "C" PLUGINXX_EXPORT void* agentxx_agent_plugin_mount(
    void*                              plugin_ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error_out
) {
    return probeMount(plugin_ctx, spec, notify, error_out);
}

extern "C" PLUGINXX_EXPORT void agentxx_agent_plugin_unmount(
    void*                         plugin_ctx,
    void*                         mount_ud,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
) {
    (void)plugin_ctx;
    (void)mount_ud;
    (void)error_out;
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
}

extern "C" PLUGINXX_EXPORT void
    agentxx_agent_plugin_stop(void*, const PluginxxOperatorNotify* notify, PluginxxString*) {
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
}

extern "C" PLUGINXX_EXPORT void agentxx_agent_plugin_destroy(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
) {
    auto* ctx = static_cast<ProbeCtx*>(plugin_ctx);
    if (ctx && ctx->host) {
        ctx->host->vtable->free(ctx);
    }
    (void)error_out;
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
}
