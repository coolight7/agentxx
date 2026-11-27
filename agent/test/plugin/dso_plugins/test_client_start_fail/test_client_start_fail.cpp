/// test_client_start_fail_plugin —— 客户端加载期 start 失败回滚用例的测试专用插件
///
/// 行为 (受环境变量控制, 供同一 DSO 先失败后成功的两段式用例):
/// - 未设置 `AGENTXX_TEST_CLIENT_START_OK`:
///   start 依次完成全部 UI 注册并逐一自检 (注册后立即推一份内容,
///   返回失败即提前失败), 然后**主动失败** (返回 NULL + error):
///     1. 状态栏内容 `test_client_start_fail.status` (+ 推一份自检)
///     2. 侧边栏面板内容 `test_client_start_fail.panel` (+ 推一份自检)
///     3. Info 栏内容 `test_client_start_fail.info` (+ 推一份自检)
///     4. 斜杠命令 `test_client_start_fail_cmd`
///     5. 事件订阅 (`agentxx.client.ready` 频道)
///   推送自检保证"走到最后一步"意味着前面的注册真实生效, 主程序回滚必须
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

/// 元信息填表 (纯 C ABI 版的 `fillClientPluginInfo`): 只写插件侧字段
int32_t fillInfo(PluginxxInfo* out, const char* name, const char* ver, const char* desc) {
    if (!out || out->struct_size < PLUGINXX_INFO_BASELINE_SIZE) {
        return -1;
    }
    out->min_api_version = AGENTXX_CLIENT_PLUGIN_MIN_API_VERSION;
    out->flags           = 0;
    out->name            = cstrView(name);
    out->version         = cstrView(ver);
    out->description     = cstrView(desc);
    return 0;
}

/// 失败收尾: 写 error_out 并当场交回失败通知
void notifyFailure(
    const PluginxxOperatorNotify* notify,
    PluginxxString*               out,
    const char*                   text
) {
    auto msg = cstrView(text);
    if (out) {
        out->data = const_cast<char*>(msg.data);
        out->size = msg.size;
    }
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &msg);
    }
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

/// 内容生成函数: 回 `user_data` 指的那份文档 (本用例只验证注册与回滚)
void PLUGINXX_CALL echoDocGenerate(
    const PluginxxHost*           /*host*/,
    const PluginxxStringView*     /*ctx_json*/,
    void*                         user_data,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               /*error_out*/
) {
    if (!notify || !notify->done) {
        return;
    }
    const char* doc = user_data != nullptr ? static_cast<const char*>(user_data) : "{}";
    std::string payload{"{\json\:"};
    payload += doc;
    payload += "}";
    const PluginxxStringView view{
        payload.data(),
        static_cast<uint64_t>(payload.size())
    };
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &view);
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

struct ProbeCtx {
    const PluginxxHost* host    = nullptr;
    bool                started = false;
};

/// 注册一份界面内容: 声明项 (位置/标题) + 内容 (生成函数) + 推一份文档自检
bool registerContent(
    const PluginxxHost*         host,
    const AgentxxClientUiIface* ui,
    const char*                 id,
    const char*                 slot,
    const char*                 title,
    const char*                 dataJson,
    const char*                 docJson,
    PluginxxString*             error
) {
    auto fail = [host, error](const char* what) {
        setError(host, error, what);
        return false;
    };
    PluginxxUiEntrySpec entry{};
    entry.version     = 1;
    entry.struct_size = sizeof(PluginxxUiEntrySpec);
    entry.item_id     = sv(id);
    entry.type        = sv(slot);
    entry.data_json   = sv(dataJson);
    if (!submitOk([&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return ui->register_ui_item(host, &entry, cb, ud, err);
        })) {
        return fail("client start: register_ui_item failed");
    }

    PluginxxUiContentSpec spec{};
    spec.version     = 1;
    spec.struct_size = sizeof(PluginxxUiContentSpec);
    spec.id          = sv(id);
    spec.slot        = sv(slot);
    spec.title       = sv(title);
    spec.generate    = &echoDocGenerate;
    spec.user_data   = const_cast<char*>(docJson);
    if (!submitOk([&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return ui->register_ui_content(host, &spec, cb, ud, err);
        })) {
        return fail("client start: register_ui_content failed");
    }

    const auto idSv  = sv(id);
    const auto docSv = sv(docJson);
    if (!submitOk([&](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
            return ui->push_ui_content(host, &idSv, &docSv, cb, ud, err);
        })) {
        return fail("client start: push_ui_content failed");
    }
    return true;
}

/// 注册事务: 每份内容注册后立即推一份自检; 失败时把步骤名写进 `error`
bool registerAll(const PluginxxHost* host, ProbeCtx* ctx, PluginxxString* error) {
    auto fail = [host, error](const char* what) {
        setError(host, error, what);
        return false;
    };
    const auto* ui
        = static_cast<const AgentxxClientUiIface*>(queryIface(host, AGENTXX_IFACE_CLIENT_UI));
    if (!ui || !ui->register_ui_item || !ui->register_ui_content || !ui->push_ui_content) {
        return fail("client start: ui table missing declarative content members");
    }

    if (!registerContent(
            host,
            ui,
            "test_client_start_fail.status",
            "status",
            "",
            R"({"align":0,"order":10})",
            R"({"blocks":[{"kind":"Text","text":"probe: 1","dim":true}]})",
            error
        )) {
        return false;
    }
    if (!registerContent(
            host,
            ui,
            "test_client_start_fail.panel",
            "panel",
            "Probe",
            R"({"title":"Probe"})",
            R"({"blocks":[{"kind":"text","text":"probe"}]})",
            error
        )) {
        return false;
    }
    if (!registerContent(
            host,
            ui,
            "test_client_start_fail.info",
            "info",
            "Probe Info",
            R"({"title":"Probe Info"})",
            R"({"blocks":[{"kind":"text","text":"probe"}]})",
            error
        )) {
        return false;
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

extern "C" PLUGINXX_EXPORT int32_t agentxx_client_plugin_get_info(PluginxxInfo* out) {
    return fillInfo(
        out,
        "test_client_start_fail_plugin",
        "1.0.0",
        "Test-only client plugin: fails start after full UI registration (rollback fixture)"
    );
}

extern "C" PLUGINXX_EXPORT void agentxx_client_plugin_create(
    const PluginxxHost*           host,
    void**                        plugin_ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
) {
    if (!host || !host->vtable || !plugin_ctx) {
        notifyFailure(notify, error_out, "client start: invalid create call");
        return;
    }
    // 可重入: 禁用后再启用只重新注册, 不重建上下文
    if (*plugin_ctx == nullptr) {
        auto* ctx = static_cast<ProbeCtx*>(host->vtable->alloc(sizeof(ProbeCtx)));
        if (!ctx) {
            notifyFailure(notify, error_out, "client start: out of memory");
            return;
        }
        ctx->host    = host;
        ctx->started = false;
        *plugin_ctx  = ctx;
    }

    auto* ctx = static_cast<ProbeCtx*>(*plugin_ctx);
    const char* okFlag = std::getenv("AGENTXX_TEST_CLIENT_START_OK");
    if (!registerAll(host, ctx, error_out)) {
        if (notify && notify->done) {
            auto msg = error_out && error_out->data
                           ? PluginxxStringView{error_out->data, error_out->size}
                           : PluginxxStringView{nullptr, 0};
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &msg);
        }
        return;
    }
    if (okFlag && okFlag[0] == '1') {
        ctx->started = true;
        if (notify && notify->done) {
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
        }
        return;
    }
    // 全部注册 + 句柄自检成功后主动失败 (触发主程序回滚)。
    setError(host, error_out, "client start failed on purpose after full registration");
    if (notify && notify->done) {
        auto msg = PluginxxStringView{error_out->data, error_out->size};
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &msg);
    }
}

extern "C" PLUGINXX_EXPORT void
    agentxx_client_plugin_stop(void*, const PluginxxOperatorNotify* notify, PluginxxString*) {
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
}

extern "C" PLUGINXX_EXPORT void agentxx_client_plugin_destroy(
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
