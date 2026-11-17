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
///     5. 事件订阅 (`agentxx.client.ready` 频道)
///   句柄自检保证"走到最后一步"意味着前面的注册真实生效, 主程序回滚必须
///   全部撤销; 测试随后断言无任何残留。
/// - 已设置该环境变量: start 直接成功返回 (供第二次加载验证回滚后同名
///   UI 项/命令/订阅可完整重新注册)。
///
/// 仅使用纯 C ABI 头 (client_plugin_api.h); 不依赖 libagentxx。
#include "agentxx/plugin/api/client_plugin_api.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>

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

void PLUGINXX_CALL onReadyEvent(const PluginxxStringView*, void*) {
    // 回滚用例只关心订阅是否被撤销; handler 本身不做任何事。
}

int32_t PLUGINXX_CALL probeCommandExecute(
    void*                         userData,
    const PluginxxStringView*     args,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               errorOut
) {
    (void)userData;
    (void)args;
    (void)errorOut;
    // 同步完成但不给动作 (空正文 = 本次没有动作)
    if (notify && notify->done) {
        const PluginxxStringView empty{nullptr, 0};
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &empty);
    }
    return 0;
}

/// 一次表调用的完成结果
///
/// 表成员是异步操作契约: 工作体在主程序的 client io 线程执行。本插件的 start 事务
/// 可能在别的线程 (加载线程) 里跑, 所以这里等完成通知而不是假设同步完成。
struct CallResult {
    std::mutex              mutex;
    std::condition_variable cv;
    char                    handleText[32] = {0}; ///< 句柄正文 (进程内地址的十进制文本)
    bool                    done           = false;
    int32_t                 status         = PLUGINXX_OPERATOR_FAILED;
};

void PLUGINXX_CALL callDone(void* ud, int32_t status, const PluginxxStringView* payload) {
    auto* result = static_cast<CallResult*>(ud);
    {
        std::lock_guard<std::mutex> lock(result->mutex);
        result->status = status;
        if (payload && payload->data && payload->size > 0
            && payload->size < sizeof(result->handleText)) {
            std::memcpy(result->handleText, payload->data, static_cast<size_t>(payload->size));
            result->handleText[payload->size] = '\0';
        }
        result->done = true;
    }
    result->cv.notify_all();
}

/// 等完成通知 (调用方可能已在 io 线程内联完成, 此时立即返回)
bool waitCall(CallResult& call) {
    std::unique_lock<std::mutex> lock(call.mutex);
    return call.cv.wait_for(lock, std::chrono::seconds{10}, [&call] {
        return call.done;
    });
}

/// 结果正文 → 句柄指针 (调用方持锁; 空/非法给 nullptr)
void* handleOfLocked(const CallResult& result) {
    if (!result.done || result.status != PLUGINXX_OPERATOR_OK || result.handleText[0] == '\0') {
        return nullptr;
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(std::strtoull(result.handleText, nullptr, 10)));
}

/// 提交一次表调用, 只要"受理且成功" (不关心正文)
template<typename StartFn>
bool submitOk(StartFn&& start) {
    CallResult     call;
    PluginxxString err{nullptr, 0};
    if (!start(&callDone, &call, &err) || !waitCall(call)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(call.mutex);
    return call.status == PLUGINXX_OPERATOR_OK;
}

/// 提交一次表调用并取回句柄 (正文 = 地址十进制文本)
template<typename StartFn>
void* submitHandle(StartFn&& start) {
    CallResult     call;
    PluginxxString err{nullptr, 0};
    if (!start(&callDone, &call, &err) || !waitCall(call)) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(call.mutex);
    return handleOfLocked(call);
}

struct ProbeCtx {
    const PluginxxHost* host    = nullptr;
    bool                started = false;
};

/// 注册事务: 每步注册后立即用句柄操作自检; 失败时把步骤名写进 `error`
bool registerAll(const PluginxxHost* host, ProbeCtx* ctx, PluginxxString* error) {
    auto fail = [host, error](const char* what) {
        setError(host, error, what);
        return false;
    };
    const auto* ui
        = static_cast<const AgentxxClientUiIface*>(queryIface(host, AGENTXX_IFACE_CLIENT_UI));
    if (!ui || !ui->register_status_item) {
        return fail("client start: ui table missing register_status_item");
    }

    const auto statusInit = sv(R"({"text":"probe: 0"})");
    const auto statusId   = sv("test_client_start_fail.status");
    auto*      status     = static_cast<AgentxxStatusItem*>(submitHandle(
        [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return ui->register_status_item(host, &statusId, &statusInit, 0, 10, cb, ud, err);
        }
    ));
    if (!status) {
        return fail("client start: register_status_item failed");
    }
    const auto statusUpdate = sv(R"({"text":"probe: 1"})");
    if (ui->update_status_item
        && !submitOk([&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
               return ui->update_status_item(host, status, &statusUpdate, cb, ud, err);
           })) {
        return fail("client start: update_status_item failed");
    }

    if (!ui->register_panel) {
        return fail("client start: ui table missing register_panel");
    }
    const auto panelId    = sv("test_client_start_fail.panel");
    const auto panelProps = sv(R"({"title":"Probe"})");
    auto*      panel      = static_cast<AgentxxPanel*>(submitHandle(
        [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return ui->register_panel(host, &panelId, &panelProps, cb, ud, err);
        }
    ));
    if (!panel) {
        return fail("client start: register_panel failed");
    }
    const auto panelItems = sv(R"({"items":[{"kind":"text","text":"probe"}]})");
    if (ui->update_panel
        && !submitOk([&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
               return ui->update_panel(host, panel, &panelItems, cb, ud, err);
           })) {
        return fail("client start: update_panel failed");
    }

    if (!ui->register_info_section) {
        return fail("client start: ui table missing register_info_section");
    }
    const auto infoId    = sv("test_client_start_fail.info");
    const auto infoProps = sv(R"({"title":"Probe Info"})");
    auto*      info      = static_cast<AgentxxInfoSection*>(submitHandle(
        [&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return ui->register_info_section(host, &infoId, &infoProps, cb, ud, err);
        }
    ));
    if (!info) {
        return fail("client start: register_info_section failed");
    }
    const auto infoItems = sv(R"({"items":[{"kind":"text","text":"probe"}]})");
    if (ui->update_info_section
        && !submitOk([&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
               return ui->update_info_section(host, info, &infoItems, cb, ud, err);
           })) {
        return fail("client start: update_info_section failed");
    }

    if (!ui->register_command) {
        return fail("client start: ui table missing register_command");
    }
    const auto cmdName = sv("test_client_start_fail_cmd");
    const auto cmdDesc = sv("client rollback probe command");
    if (!submitOk([&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return ui->register_command(
                host,
                &cmdName,
                &cmdDesc,
                &probeCommandExecute,
                ctx,
                cb,
                ud,
                err
            );
        })) {
        return fail("client start: register_command failed");
    }

    const auto* broadcasts
        = static_cast<const PluginxxBroadcastsIface*>(queryIface(host, PLUGINXX_IFACE_BROADCASTS)
        );
    if (!broadcasts || !broadcasts->subscribe_broadcast) {
        return fail("client start: broadcasts table missing subscribe_broadcast");
    }
    const auto readyId = sv("agentxx.client.ready");
    // 表成员是异步契约: 不关心结果, 只受理 (未受理时返回 NULL)
    if (!broadcasts->subscribe_broadcast(
            host,
            nullptr,
            &readyId,
            &onReadyEvent,
            ctx,
            nullptr,
            nullptr,
            nullptr,
            nullptr
        )) {
        return fail("client start: subscribe_broadcast rejected");
    }
    return true;
}

} // namespace

extern "C" PLUGINXX_EXPORT const AgentxxClientPluginInfo* agentxx_plugin_client_get_info(void) {
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

extern "C" PLUGINXX_EXPORT int
    agentxx_plugin_client_create(const PluginxxHost* host, void** plugin_ctx) {
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

extern "C" PLUGINXX_EXPORT void* agentxx_plugin_client_start(
    void*                         plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
) {
    auto* ctx = static_cast<ProbeCtx*>(plugin_ctx);
    if (!ctx || !ctx->host) {
        setError(ctx ? ctx->host : nullptr, error_out, "client start: missing context");
        return nullptr;
    }
    const PluginxxHost* host = ctx->host;

    const char* okFlag = std::getenv("AGENTXX_TEST_CLIENT_START_OK");
    if (!registerAll(host, ctx, error_out)) {
        return nullptr;
    }
    if (okFlag && okFlag[0] == '1') {
        ctx->started = true;
        if (notify && notify->done) {
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        }
        return ctx;
    }
    // 全部注册 + 句柄自检成功后主动失败 (触发主程序回滚)。
    setError(host, error_out, "client start failed on purpose after full registration");
    return nullptr;
}

extern "C" PLUGINXX_EXPORT void*
    agentxx_plugin_client_stop(void*, const PluginxxOperatorNotify* notify, PluginxxString*) {
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return nullptr;
}

extern "C" PLUGINXX_EXPORT void agentxx_plugin_client_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ProbeCtx*>(plugin_ctx);
    if (ctx && ctx->host) {
        ctx->host->vtable->free(ctx);
    }
}
