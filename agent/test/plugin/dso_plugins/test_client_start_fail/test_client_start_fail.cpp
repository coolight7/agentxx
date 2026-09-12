/// test_client_start_fail_plugin —— 客户端加载期 start 失败回滚用例的测试专用插件
///
/// 行为 (受环境变量控制, 供同一 DSO 先失败后成功的两段式用例):
/// - 未设置 `AGENTXX_TEST_CLIENT_START_OK`:
///   start 依次完成全部 UI 注册并逐一经句柄自检 (register 后立即 update,
///   返回非 0 即提前失败), 然后**主动失败** (返回 NULL + error):
///     1. 状态栏项 `test_client_start_fail.status` (+ update 自检)
///     2. 侧边栏面板 `test_client_start_fail.panel` (+ update 自检)
///     3. Info 栏段落 `test_client_start_fail.info` (+ update 自检)
///     4. 斜杠命令 `test_client_start_fail_cmd`
///     5. 事件订阅 `AGENTXX_CLIENT_EVT_READY`
///   句柄自检保证"走到最后一步"意味着前面的注册真实生效, 宿主回滚必须
///   全部撤销; 测试随后断言无任何残留。
/// - 已设置该环境变量: start 直接成功返回 (供第二次加载验证回滚后同名
///   UI 项/命令/订阅可完整重新注册)。
///
/// 仅使用纯 C ABI 头 (client_plugin_api.h); 不依赖 libagentxx。
#include "agentxx/plugin/api/client_plugin_api.h"

#include <cstdlib>
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

const void* queryIface(const AgentxxPluginHost* host, const char* iid) {
    const auto view = sv(iid);
    return host->vtable->query_interface(host, &view);
}

void AGENTXX_PLUGIN_CALL onReadyEvent(const AgentxxPluginStringView*, void*) {
    // 回滚用例只关心订阅是否被撤销; handler 本身不做任何事。
}

int32_t AGENTXX_PLUGIN_CALL
    probeCommandExecute(void*, const AgentxxPluginStringView*, AgentxxPluginString* actionOut, AgentxxPluginString*) {
    if (actionOut) {
        actionOut->data = nullptr;
        actionOut->size = 0;
    }
    return 0;
}

struct ProbeCtx {
    const AgentxxPluginHost* host    = nullptr;
    bool                     started = false;
};

/// 注册事务: 每步注册后立即用句柄操作自检, 失败返回 false。
bool registerAll(const AgentxxPluginHost* host, ProbeCtx* ctx) {
    const auto* ui
        = static_cast<const AgentxxClientUiIface*>(queryIface(host, AGENTXX_IFACE_CLIENT_UI));
    if (!ui || !ui->register_status_item) {
        return false;
    }

    const auto statusInit = sv(R"({"text":"probe: 0"})");
    const auto statusId   = sv("test_client_start_fail.status");
    auto*      status     = ui->register_status_item(host, &statusId, &statusInit, 0, 10);
    if (!status) {
        return false;
    }
    const auto statusUpdate = sv(R"({"text":"probe: 1"})");
    if (ui->update_status_item && ui->update_status_item(host, status, &statusUpdate) != 0) {
        return false;
    }

    if (!ui->register_panel) {
        return false;
    }
    const auto panelId    = sv("test_client_start_fail.panel");
    const auto panelProps = sv(R"({"title":"Probe"})");
    auto*      panel      = ui->register_panel(host, &panelId, &panelProps);
    if (!panel) {
        return false;
    }
    const auto panelItems = sv(R"({"items":[{"kind":"text","text":"probe"}]})");
    if (ui->update_panel && ui->update_panel(host, panel, &panelItems) != 0) {
        return false;
    }

    if (!ui->register_info_section) {
        return false;
    }
    const auto infoId    = sv("test_client_start_fail.info");
    const auto infoProps = sv(R"({"title":"Probe Info"})");
    auto*      info      = ui->register_info_section(host, &infoId, &infoProps);
    if (!info) {
        return false;
    }
    const auto infoItems = sv(R"({"items":[{"kind":"text","text":"probe"}]})");
    if (ui->update_info_section && ui->update_info_section(host, info, &infoItems) != 0) {
        return false;
    }

    if (!ui->register_command) {
        return false;
    }
    const auto cmdName = sv("test_client_start_fail_cmd");
    const auto cmdDesc = sv("client rollback probe command");
    if (ui->register_command(host, &cmdName, &cmdDesc, &probeCommandExecute, ctx) != 0) {
        return false;
    }

    const auto* events
        = static_cast<const AgentxxClientEventsIface*>(queryIface(host, AGENTXX_IFACE_CLIENT_EVENTS)
        );
    if (!events || !events->subscribe) {
        return false;
    }
    if (!events->subscribe(host, AGENTXX_CLIENT_EVT_READY, &onReadyEvent, ctx)) {
        return false;
    }
    return true;
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxClientPluginInfo* agentxx_plugin_client_get_info(void
) {
    static const AgentxxClientPluginInfo info{
        AGENTXX_CLIENT_PLUGIN_API_VERSION,
        0,
        cstrView("test_client_start_fail_plugin"),
        cstrView("1.0.0"),
        cstrView(
            "Test-only client plugin: fails start after full UI registration (rollback fixture)"
        ),
    };
    return &info;
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_client_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    if (!host || !host->vtable || !plugin_ctx) {
        return -1;
    }
    auto* ctx = static_cast<ProbeCtx*>(host->vtable->alloc(sizeof(ProbeCtx)));
    if (!ctx) {
        return -1;
    }
    ctx->host    = host;
    ctx->started = false;
    *plugin_ctx  = ctx;
    return 0;
}

extern "C" AGENTXX_PLUGIN_EXPORT void* agentxx_plugin_client_start(
    void*                              plugin_ctx,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error_out
) {
    auto* ctx = static_cast<ProbeCtx*>(plugin_ctx);
    if (!ctx || !ctx->host) {
        setError(ctx ? ctx->host : nullptr, error_out, "client start: missing context");
        return nullptr;
    }
    const AgentxxPluginHost* host = ctx->host;

    const char* okFlag = std::getenv("AGENTXX_TEST_CLIENT_START_OK");
    if (!registerAll(host, ctx)) {
        setError(host, error_out, "client start failed during registration transaction");
        return nullptr;
    }
    if (okFlag && okFlag[0] == '1') {
        ctx->started = true;
        if (notify && notify->done) {
            notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
        }
        return ctx;
    }
    // 全部注册 + 句柄自检成功后主动失败 (触发宿主回滚)。
    setError(host, error_out, "client start failed on purpose after full registration");
    return nullptr;
}

extern "C" AGENTXX_PLUGIN_EXPORT void*
    agentxx_plugin_client_stop(void*, const AgentxxPluginOperatorNotify* notify, AgentxxPluginString*) {
    if (notify && notify->done) {
        notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, nullptr);
    }
    return nullptr;
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_client_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ProbeCtx*>(plugin_ctx);
    if (ctx && ctx->host) {
        ctx->host->vtable->free(ctx);
    }
}
