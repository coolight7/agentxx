#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/plugin/op_driver.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/agent/resource_applier.h"
#include "agentxx/plugin/plugin_common.h"
#include "agentxx/util/log.h"
#include "fmt/format.h"

#include <cstring>

namespace agentxx {
namespace plugin {

// =====================================================================
// vtable 入口公共前置
// =====================================================================

using HostCall = PluginHostCall<PluginInstance, PluginManager>;

/// 解析宿主控制块，并持有实例/管理器强引用与 admission lease。
///
/// - 实例已卸载/已关闭，或传入的 host 指针不是本宿主发放的视图 -> 返回空上下文
///   （实例与管理器均为 nullptr），入口按失败返回，不访问已释放对象，也不会把
///   调用转交给后来加载的同名实例；
/// - `allowClosing=true` 用于只读查询：关闭过程中仍允许执行（lease 保证卸载会
///   等它返回）。注册、投递新工作等入口必须用默认值，Closing/Disabled 后拒绝；
/// - 返回的上下文按值捕获进投递闭包后，卸载的 idle 等待会覆盖"已排队但尚未在
///   IO 线程执行"的阶段，见 [ioCallSyncKeep]。
static HostCall enterHost(const AgentxxPluginHost* host, bool allowClosing = false) {
    return enterPluginHost<PluginInstance, PluginManager>(host, allowClosing);
}

// C ABI 内存两件套

static void* AGENTXX_PLUGIN_CALL xx_alloc(uint64_t size) {
    return agentxx::plugin::hostMemoryAlloc(size);
}

static void AGENTXX_PLUGIN_CALL xx_free(void* ptr) {
    agentxx::plugin::hostMemoryFree(ptr);
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_register_tool(const AgentxxPluginHost* host, const AgentxxPluginToolSpec* spec) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || !spec || agentxx::plugin::PluginStringView::empty(&spec->name)) {
            return -1;
        }
        auto                  mgrPtr   = mgr;
        auto                  instPtr  = inst;
        AgentxxPluginToolSpec specCopy = *spec;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, specCopy]() {
            return mgrPtr->registerTool(instPtr, &specCopy);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_unregister_tool(const AgentxxPluginHost* host, const AgentxxPluginStringView* name) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(name)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto nameVal = *name;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, nameVal]() {
            return mgrPtr->unregisterTool(instPtr, nameVal);
        });
    });
}

static void AGENTXX_PLUGIN_CALL xx_op_cancel(::AgentxxPluginOperatorHandle* op) {
    cancelPluginOperation(op);
}

static ::AgentxxPluginOperatorHandle* AGENTXX_PLUGIN_CALL xx_call_tool_async(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* name,
    const AgentxxPluginStringView* args_json,
    const AgentxxPluginStringView* session_id,
    AgentxxPluginOperatorCallback  cb,
    void*                          ud,
    AgentxxPluginString*           error_out
) {
    return guardVtableCall<::AgentxxPluginOperatorHandle*>(nullptr, [&]() {
        auto call = enterHost(host);
        auto* inst = call.instance();
        auto* mgr = call.manager();
        if (!mgr || !inst || !name) {
            hostMemorySetString(error_out, "call_tool_async: plugin runtime unavailable");
            return static_cast<::AgentxxPluginOperatorHandle*>(nullptr);
        }
        return mgr->callToolAsync(inst, *name, args_json ? *args_json : AgentxxPluginStringView{},
                                  session_id ? *session_id : AgentxxPluginStringView{}, cb, ud, error_out);
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_register_hook(const AgentxxPluginHost* host, const AgentxxPluginHookSpec* spec) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || !spec || spec->point < 0 || spec->point >= AGENTXX_PLUGIN_HOOK_COUNT
            || !spec->hook_start) {
            return -1;
        }
        auto                  mgrPtr   = mgr;
        auto                  instPtr  = inst;
        AgentxxPluginHookSpec specCopy = *spec;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, specCopy]() {
            return mgrPtr->registerHook(instPtr, &specCopy);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_unregister_hook(const AgentxxPluginHost* host, int32_t point) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || point < 0 || point >= AGENTXX_PLUGIN_HOOK_COUNT) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto pt      = static_cast<AgentxxPluginHookPoint>(point);
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, pt]() {
            return mgrPtr->unregisterHook(instPtr, pt);
        });
    });
}

static AgentxxPluginSubscription* AGENTXX_PLUGIN_CALL xx_subscribe(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* topic,
    void(AGENTXX_PLUGIN_CALL* handler)(const AgentxxPluginStringView* event_json, void* ud),
    void* ud
) {
    return agentxx::plugin::guardVtableCall(nullptr, [&]() -> AgentxxPluginSubscription* {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || !topic || !handler) {
            return static_cast<AgentxxPluginSubscription*>(nullptr);
        }
        auto mgrPtr   = mgr;
        auto instPtr  = inst;
        auto topicVal = *topic;
        return ioCallSyncKeep<AgentxxPluginSubscription*>(
            call, mgrPtr,
            [mgrPtr, instPtr, topicVal, handler, ud]() {
                return mgrPtr->subscribe(instPtr, topicVal, handler, ud);
            }
        );
    });
}

static void AGENTXX_PLUGIN_CALL xx_unsubscribe(AgentxxPluginSubscription* sub) {
    agentxx::plugin::guardVtableCallVoid([&]() {
        if (!sub) {
            return;
        }
        auto inst = sub->inst.lock();
        auto mgr  = inst ? inst->manager.lock() : nullptr;
        if (mgr) {
            ioCallSyncVoid(mgr.get(), [mgr, sub]() {
                mgr->unsubscribe(sub);
            });
        } else {
            sub->alive.store(false, std::memory_order_release);
            sub->inst.reset();
        }
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_publish(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* topic,
    const AgentxxPluginStringView* event_json
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || !topic || !event_json) {
            return -1;
        }
        if (!inst->enabled) {
            XX_LOGW("Plugin `{}` publish ignored (disabled)", inst->name);
            return -1;
        }
        return mgr->publish(*topic, *event_json);
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_register_capability(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* capability
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(capability)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto capVal  = *capability;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, capVal]() {
            return mgrPtr->registerCapability(instPtr, capVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_unregister_capability(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* capability
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(capability)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto capVal  = *capability;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, capVal]() {
            return mgrPtr->unregisterCapability(instPtr, capVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_has_capability(const AgentxxPluginHost* host, const AgentxxPluginStringView* capability) {
    return agentxx::plugin::guardVtableCall(0, [&]() -> int32_t {
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr = call.manager();
        if (!mgr || agentxx::plugin::PluginStringView::empty(capability)) {
            return 0;
        }
        auto mgrPtr = mgr;
        auto capVal = *capability;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, capVal]() {
            return mgrPtr->hasCapability(capVal) ? 1 : 0;
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_register_capability_ex(
    const AgentxxPluginHost*             host,
    const AgentxxPluginStringView*       capability,
    AgentxxPluginCapabilityStartFunction start,
    AgentxxPluginOperatorCancelFunction  cancel,
    void*                                ctx
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(capability) || !start) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto capVal  = *capability;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, capVal, start, cancel, ctx]() {
            return mgrPtr->registerCapabilityEx(instPtr, capVal, start, cancel, ctx);
        });
    });
}

static ::AgentxxPluginOperatorHandle* AGENTXX_PLUGIN_CALL xx_invoke_capability_async(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* capability,
    const AgentxxPluginStringView* method,
    const AgentxxPluginStringView* args_json,
    AgentxxPluginOperatorCallback  cb,
    void*                          ud,
    AgentxxPluginString*           error_out
) {
    return agentxx::plugin::guardVtableCall<::AgentxxPluginOperatorHandle*>(
        nullptr,
        [&]() -> ::AgentxxPluginOperatorHandle* {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(capability)
                || agentxx::plugin::PluginStringView::empty(method)) {
                return static_cast<::AgentxxPluginOperatorHandle*>(nullptr);
            }
            AgentxxPluginStringView args
                = args_json ? *args_json : agentxx::plugin::PluginStringView::from("{}", 2);
            return mgr->invokeCapabilityAsync(inst, *capability, *method, args, cb, ud, error_out);
        }
    );
}

static ::AgentxxPluginOperatorHandle* AGENTXX_PLUGIN_CALL xx_register_task(
    const AgentxxPluginHost*            host,
    AgentxxPluginOperatorCancelFunction cancel_fn,
    void*                               cancel_ud,
    AgentxxPluginOperatorNotify*        notify,
    AgentxxPluginString*                error_out
) {
    return agentxx::plugin::guardVtableCall<::AgentxxPluginOperatorHandle*>(
        nullptr,
        [&]() -> ::AgentxxPluginOperatorHandle* {
            auto call = enterHost(host);
            auto mgr  = call.manager();
            auto inst = call.instance();
            if (!mgr || !inst || !notify) {
                return static_cast<::AgentxxPluginOperatorHandle*>(nullptr);
            }
            auto mgrPtr  = mgr;
            auto instPtr = inst;
            return ioCallSyncKeep<::AgentxxPluginOperatorHandle*>(
                call, mgrPtr,
                [mgrPtr, instPtr, cancel_fn, cancel_ud, notify, error_out]() {
                    return mgrPtr->registerTask(instPtr, cancel_fn, cancel_ud, notify, error_out);
                }
            );
        }
    );
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_list_plugins(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr = call.manager();
        if (!mgr) {
            return -1;
        }
        auto mgrPtr = mgr;
        auto json   = ioCallSyncKeep<std::string>(call, mgrPtr, [mgrPtr]() {
            return mgrPtr->listPluginsJson();
        });
        hostMemorySetString(out, json);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_get_plugin(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* name,
    AgentxxPluginString*           out
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr = call.manager();
        if (!mgr || agentxx::plugin::PluginStringView::empty(name)) {
            return -1;
        }
        auto        mgrPtr = mgr;
        std::string pluginName{name->data, static_cast<size_t>(name->size)};
        auto        json = ioCallSyncKeep<std::string>(call, mgrPtr, [mgrPtr, pluginName]() {
            return mgrPtr->getPluginJson(pluginName);
        });
        if (json.empty()) {
            return -1;
        }
        hostMemorySetString(out, json);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_get_own_info(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst) {
            return -1;
        }
        auto        mgrPtr  = mgr;
        std::string ownName = inst->name;
        auto        json    = ioCallSyncKeep<std::string>(call, mgrPtr, [mgrPtr, ownName]() {
            return mgrPtr->getPluginJson(ownName);
        });
        if (json.empty()) {
            return -1;
        }
        hostMemorySetString(out, json);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_get_share_store(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* session_id,
    int64_t                        id,
    AgentxxPluginString*           out
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(session_id)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto sid     = *session_id;
        *out         = ioCallSyncKeep<AgentxxPluginString>(
            call, mgrPtr,
            [mgrPtr, instPtr, sid, id]() -> AgentxxPluginString {
                return mgrPtr->getShareStore(instPtr, sid, id);
            }
        );
        return (out->data != nullptr) ? 0 : -1;
    });
}

static int64_t AGENTXX_PLUGIN_CALL xx_add_share_store(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* session_id,
    const AgentxxPluginStringView* content
) {
    return agentxx::plugin::guardVtableCall<int64_t>(-1, [&]() -> int64_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || !session_id || !content) {
            return -1;
        }
        auto mgrPtr     = mgr;
        auto instPtr    = inst;
        auto sid        = *session_id;
        auto contentVal = *content;
        return ioCallSyncKeep<int64_t>(call, mgrPtr, [mgrPtr, instPtr, sid, contentVal]() -> int64_t {
            return mgrPtr->addShareStore(instPtr, sid, contentVal);
        });
    });
}

static void AGENTXX_PLUGIN_CALL xx_emit_message_tip(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* session_id,
    const AgentxxPluginStringView* text,
    int32_t                        level
) {
    agentxx::plugin::guardVtableCallVoid([&]() {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || !session_id || !text) {
            return;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto sid     = *session_id;
        auto txt     = *text;
        ioCallSyncVoidKeep(call, mgrPtr, [mgrPtr, instPtr, sid, txt, level]() {
            mgrPtr->emitMessageTip(instPtr, sid, txt, level);
        });
    });
}

// =====================================================================
// graph 接口表 (agentxx.agent.graph)
// =====================================================================

static int32_t AGENTXX_PLUGIN_CALL xx_register_node_type(
    const AgentxxPluginHost*              host,
    const AgentxxPluginGraphNodeTypeSpec* spec
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || !spec || agentxx::plugin::PluginStringView::empty(&spec->type)
            || !spec->run_start) {
            return -1;
        }
        auto                           mgrPtr   = mgr;
        auto                           instPtr  = inst;
        AgentxxPluginGraphNodeTypeSpec specCopy = *spec;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, specCopy]() {
            return mgrPtr->registerGraphNodeType(instPtr, &specCopy);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_unregister_node_type(const AgentxxPluginHost* host, const AgentxxPluginStringView* type) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(type)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto typeVal = *type;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, typeVal]() {
            return mgrPtr->unregisterGraphNodeType(instPtr, typeVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_get_graph_json(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst) {
            return -1;
        }
        auto mgrPtr = mgr;
        auto json   = ioCallSyncKeep<std::string>(call, mgrPtr, [mgrPtr]() -> std::string {
            return mgrPtr->getGraphJson();
        });
        if (json.empty()) {
            return -1;
        }
        hostMemorySetString(out, json);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_get_graph_name(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst) {
            return -1;
        }
        auto mgrPtr = mgr;
        auto name   = ioCallSyncKeep<std::string>(call, mgrPtr, [mgrPtr]() -> std::string {
            auto        json   = mgrPtr->getGraphJson();
            std::string result = "agentxx.default";
            if (!json.empty()) {
                try {
                    auto j = agentxx::util::Json::parse(json);
                    if (j.is_object() && j.contains("name") && j["name"].is_string()) {
                        result = j["name"].get<std::string>();
                    }
                } catch (...) {
                }
            }
            return result;
        });
        hostMemorySetString(out, name);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_set_graph_json(const AgentxxPluginHost* host, const AgentxxPluginStringView* graph_json) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(graph_json)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto jsonVal = *graph_json;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, jsonVal]() {
            return mgrPtr->setGraphJson(instPtr, jsonVal);
        });
    });
}

static ::AgentxxPluginOperatorHandle* AGENTXX_PLUGIN_CALL xx_sleep(
    const AgentxxPluginHost*       host,
    int64_t                        ms,
    AgentxxPluginOperatorCallback  cb,
    void*                          ud,
    AgentxxPluginString*           error_out
) {
    return agentxx::plugin::guardVtableCall<::AgentxxPluginOperatorHandle*>(nullptr, [&]() {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || !cb) {
            hostMemorySetString(error_out, "scheduler sleep: plugin runtime unavailable");
            return static_cast<::AgentxxPluginOperatorHandle*>(nullptr);
        }
        auto manager = inst->manager.lock();
        auto admission = std::make_shared<PluginInstance::InflightGuard>(inst->self.lock());
        if (!*admission) {
            hostMemorySetString(error_out, "scheduler sleep: plugin is closing");
            return static_cast<::AgentxxPluginOperatorHandle*>(nullptr);
        }
        return ioCallSyncKeep<::AgentxxPluginOperatorHandle*>(
            call, manager.get(),
            [manager, admission, ms, cb, ud, error_out]() {
                return manager->sleep(admission->inst.get(), ms, cb, ud, error_out);
            }
        );
    });
}

static ::AgentxxPluginOperatorHandle* AGENTXX_PLUGIN_CALL xx_offload(
    const AgentxxPluginHost* host,
    void*(AGENTXX_PLUGIN_CALL* work)(
        void*, const AgentxxPluginCancelToken*, AgentxxPluginString*
    ),
    void(AGENTXX_PLUGIN_CALL* done)(
        void*, int32_t, void*, const AgentxxPluginStringView*
    ),
    void*                ud,
    AgentxxPluginString* error_out
) {
    return agentxx::plugin::guardVtableCall<::AgentxxPluginOperatorHandle*>(nullptr, [&]() {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || !work) {
            hostMemorySetString(error_out, "scheduler offload: plugin runtime unavailable");
            return static_cast<::AgentxxPluginOperatorHandle*>(nullptr);
        }
        auto manager = inst->manager.lock();
        auto admission = std::make_shared<PluginInstance::InflightGuard>(inst->self.lock());
        if (!*admission) {
            hostMemorySetString(error_out, "scheduler offload: plugin is closing");
            return static_cast<::AgentxxPluginOperatorHandle*>(nullptr);
        }
        return ioCallSyncKeep<::AgentxxPluginOperatorHandle*>(
            call, manager.get(),
            [manager, admission, work, done, ud, error_out]() {
                return manager->offload(admission->inst.get(), work, done, ud, error_out);
            }
        );
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_is_io_thread(const AgentxxPluginHost* host) {
    // 只读查询：关闭过程中仍允许回答（返回 0 表示"不是 IO 线程"，后续投递会安全失败）。
    auto call = enterHost(host, /*allowClosing=*/true);
    auto mgr  = call.manager();
    return (mgr && mgr->isIoThread()) ? 1 : 0;
}

static int32_t AGENTXX_PLUGIN_CALL xx_post_to_io(
    const AgentxxPluginHost* host,
    void(AGENTXX_PLUGIN_CALL* fn)(void* ud),
    void* ud
) {
    return agentxx::plugin::guardVtableCall<int32_t>(-1, [&]() -> int32_t {
        auto  call = enterHost(host);
        auto* inst = call.instance();
        auto mgr   = call.mgr;
        auto owner = inst ? inst->self.lock() : nullptr;
        if (!mgr || !owner || !fn) {
            return -1;
        }
        auto admission = std::make_shared<PluginInstance::InflightGuard>(owner);
        if (!*admission) {
            return -1;
        }
        return ioCallSyncKeep<int32_t>(
            call, mgr.get(),
            [mgr, admission, fn, ud]() -> int32_t {
                return mgr->postCallback(admission->inst.get(), fn, ud) ? 0 : -1;
            }
        );
    });
}

static void AGENTXX_PLUGIN_CALL
    xx_log(const AgentxxPluginHost* host, int32_t level, const AgentxxPluginStringView* msg) {
    (void)host;
    using agentxx::util::LogLevel;
    LogLevel lv = LogLevel::Info;
    switch (level) {
        case 0:
            lv = LogLevel::Trace;
            break;
        case 1:
            lv = LogLevel::Debug;
            break;
        case 2:
            lv = LogLevel::Info;
            break;
        case 3:
            lv = LogLevel::Warn;
            break;
        case 4:
            lv = LogLevel::Error;
            break;
        default:
            break;
    }
    std::string text = (msg && msg->data) ? std::string(msg->data, static_cast<size_t>(msg->size))
                                          : std::string{};
    agentxx::util::xxLogPrint(lv, text);
}

static int32_t AGENTXX_PLUGIN_CALL xx_json_get_string(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* json,
    const AgentxxPluginStringView* key,
    AgentxxPluginString*           out
) {
    if (!out) {
        return -1;
    }
    auto call = enterHost(host, /*allowClosing=*/true);
    auto inst = call.instance();
    if (!inst || agentxx::plugin::PluginStringView::empty(json)
        || agentxx::plugin::PluginStringView::empty(key)) {
        return -1;
    }
    try {
        std::string jsonStr{json->data, static_cast<size_t>(json->size)};
        std::string keyStr{key->data, static_cast<size_t>(key->size)};
        auto        j = agentxx::util::Json::parse(jsonStr);
        if (j.is_object() && j.contains(keyStr) && j[keyStr].is_string()) {
            std::string val = j[keyStr].get<std::string>();
            hostMemorySetString(out, val);
            return 0;
        }
    } catch (...) {
    }
    return -1;
}

static int32_t AGENTXX_PLUGIN_CALL xx_json_escape(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* s,
    AgentxxPluginString*           out
) {
    if (!out) {
        return -1;
    }
    auto call = enterHost(host, /*allowClosing=*/true);
    auto inst = call.instance();
    if (!inst || agentxx::plugin::PluginStringView::empty(s)) {
        return -1;
    }
    std::string strOut;
    strOut.reserve(static_cast<size_t>(s->size + 2));
    strOut += '"';
    for (size_t i = 0; i < s->size; ++i) {
        const unsigned char c = static_cast<unsigned char>(s->data[i]);
        switch (c) {
            case '"':
                strOut += "\\\"";
                break;
            case '\\':
                strOut += "\\\\";
                break;
            case '\b':
                strOut += "\\b";
                break;
            case '\f':
                strOut += "\\f";
                break;
            case '\n':
                strOut += "\\n";
                break;
            case '\r':
                strOut += "\\r";
                break;
            case '\t':
                strOut += "\\t";
                break;
            default:
                if (c < 0x20) {
                    strOut += fmt::format("\\u{:04x}", c);
                } else {
                    strOut += static_cast<char>(c);
                }
                break;
        }
    }
    strOut += '"';
    hostMemorySetString(out, strOut);
    return 0;
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_get_config(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr = call.manager();
        if (!mgr) {
            return -1;
        }
        auto mgrPtr = mgr;
        auto json   = ioCallSyncKeep<std::string>(call, mgrPtr, [mgrPtr]() {
            return mgrPtr->getConfigJson();
        });
        if (json.empty()) {
            return -1;
        }
        hostMemorySetString(out, json);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_get_plugin_args(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto json    = ioCallSyncKeep<std::string>(call, mgrPtr, [mgrPtr, instPtr]() {
            return mgrPtr->getPluginArgsJson(instPtr);
        });
        hostMemorySetString(out, json);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_get_tool_prompt(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* tool_name,
    AgentxxPluginString*           out
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr = call.manager();
        if (!mgr || agentxx::plugin::PluginStringView::empty(tool_name)) {
            return -1;
        }
        auto        mgrPtr = mgr;
        std::string name{tool_name->data, static_cast<size_t>(tool_name->size)};
        auto        json = ioCallSyncKeep<std::string>(call, mgrPtr, [mgrPtr, name]() {
            return mgrPtr->getToolPromptJson(name);
        });
        if (json.empty()) {
            return -1;
        }
        hostMemorySetString(out, json);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_get_session_work_dir(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* thread_id,
    AgentxxPluginString*           out
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst) {
            return -1;
        }
        auto        mgrPtr = mgr;
        std::string tid    = (thread_id && thread_id->data)
                                 ? std::string(thread_id->data, static_cast<size_t>(thread_id->size))
                                 : std::string{};
        auto        dir    = ioCallSyncKeep<std::string>(call, mgrPtr, [mgrPtr, tid]() {
            return mgrPtr->getSessionWorkDir(tid);
        });
        if (dir.empty()) {
            return -1;
        }
        hostMemorySetString(out, dir);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_get_plugin_config_path(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto path    = ioCallSyncKeep<std::string>(call, mgrPtr, [mgrPtr, instPtr]() {
            return mgrPtr->getPluginConfigPath(instPtr);
        });
        if (path.empty()) {
            return -1;
        }
        hostMemorySetString(out, path);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_get_language(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr = call.manager();
        if (!mgr) {
            return -1;
        }
        auto mgrPtr = mgr;
        auto lang   = ioCallSyncKeep<std::string>(call, mgrPtr, [mgrPtr]() {
            return mgrPtr->getLanguage();
        });
        if (lang.empty()) {
            lang = "en";
        }
        hostMemorySetString(out, lang);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_set_language(const AgentxxPluginHost* host, const AgentxxPluginStringView* language) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr = call.manager();
        if (!mgr) {
            return -1;
        }
        std::string lang   = (language && language->data)
                                 ? std::string(language->data, static_cast<size_t>(language->size))
                                 : std::string{};
        auto        mgrPtr = mgr;
        ioCallSyncVoidKeep(call, mgrPtr, [mgrPtr, lang]() {
            mgrPtr->setLanguage(lang);
        });
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_get_prompt(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr = call.manager();
        if (!mgr) {
            return -1;
        }
        auto mgrPtr = mgr;
        auto json   = ioCallSyncKeep<std::string>(call, mgrPtr, [mgrPtr]() {
            return mgrPtr->getPromptJson();
        });
        if (json.empty()) {
            return -1;
        }
        hostMemorySetString(out, json);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_set_prompt(const AgentxxPluginHost* host, const AgentxxPluginStringView* prompt_json) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(prompt_json)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto pJson   = *prompt_json;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, pJson]() {
            return mgrPtr->setPromptJson(instPtr, pJson);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_model_get_config(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr = call.manager();
        if (!mgr) {
            return -1;
        }
        auto mgrPtr = mgr;
        auto json   = ioCallSyncKeep<std::string>(call, mgrPtr, [mgrPtr]() {
            return mgrPtr->getModelConfigJson();
        });
        if (json.empty()) {
            return -1;
        }
        hostMemorySetString(out, json);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_cancel_is_cancelled(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* thread_id
) {
    return agentxx::plugin::guardVtableCall(0, [&]() -> int32_t {
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr = call.manager();
        if (!mgr || agentxx::plugin::PluginStringView::empty(thread_id)) {
            return 0;
        }
        auto        mgrPtr = mgr;
        std::string tid{thread_id->data, static_cast<size_t>(thread_id->size)};
        return ioCallSyncKeep<bool>(
                   call, mgrPtr,
                   [mgrPtr, tid]() {
                       return mgrPtr->isSessionCancelled(tid);
                   }
               )
                   ? 1
                   : 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_register_skill_dir(const AgentxxPluginHost* host, const AgentxxPluginStringView* path) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(path)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto pathVal = *path;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, pathVal]() {
            return mgrPtr->registerSkillDir(instPtr, pathVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_unregister_skill_dir(const AgentxxPluginHost* host, const AgentxxPluginStringView* path) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(path)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto pathVal = *path;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, pathVal]() {
            return mgrPtr->unregisterSkillDir(instPtr, pathVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_register_memory_file(const AgentxxPluginHost* host, const AgentxxPluginStringView* path) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(path)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto pathVal = *path;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, pathVal]() {
            return mgrPtr->registerMemoryFile(instPtr, pathVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_unregister_memory_file(const AgentxxPluginHost* host, const AgentxxPluginStringView* path) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(path)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto pathVal = *path;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, pathVal]() {
            return mgrPtr->unregisterMemoryFile(instPtr, pathVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_register_mcp_server(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* spec_json
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(spec_json)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto specVal = *spec_json;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, specVal]() {
            return mgrPtr->registerMcpServer(instPtr, specVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_unregister_mcp_server(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* name_space
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto call = enterHost(host);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(name_space)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto nsVal   = *name_space;
        return ioCallSyncKeep<int32_t>(call, mgrPtr, [mgrPtr, instPtr, nsVal]() {
            return mgrPtr->unregisterMcpServer(instPtr, nsVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL
    xx_get_own_resources(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto call = enterHost(host, /*allowClosing=*/true);
        auto mgr  = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto json    = ioCallSyncKeep<std::string>(call, mgrPtr, [mgrPtr, instPtr]() {
            return mgrPtr->ownResourcesJson(instPtr);
        });
        if (json.empty()) {
            return -1;
        }
        hostMemorySetString(out, json);
        return 0;
    });
}

static const AgentxxPluginToolsIface g_ifaceTools = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION,
    /* struct_size */ sizeof(AgentxxPluginToolsIface),
    /* register_tool */ xx_register_tool,
    /* unregister_tool */ xx_unregister_tool,
    /* call_tool_async */ xx_call_tool_async,
    /* op_cancel */ xx_op_cancel,
};

static const AgentxxPluginHooksIface g_ifaceHooks = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_VERSION,
    /* struct_size */ sizeof(AgentxxPluginHooksIface),
    /* register_hook */ xx_register_hook,
    /* unregister_hook */ xx_unregister_hook,
};

static const AgentxxPluginEventsIface g_ifaceEvents = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_EVENTS_VERSION,
    /* struct_size */ sizeof(AgentxxPluginEventsIface),
    /* subscribe */ xx_subscribe,
    /* unsubscribe */ xx_unsubscribe,
    /* publish */ xx_publish,
};

static const AgentxxPluginCapabilitiesIface g_ifaceCapabilities = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES_VERSION,
    /* struct_size */ sizeof(AgentxxPluginCapabilitiesIface),
    /* register_capability */ xx_register_capability,
    /* register_capability_ex */ xx_register_capability_ex,
    /* unregister_capability */ xx_unregister_capability,
    /* has_capability */ xx_has_capability,
    /* invoke_capability_async */ xx_invoke_capability_async,
    /* op_cancel */ xx_op_cancel,
};

static const AgentxxPluginSchedulerIface g_ifaceScheduler = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER_VERSION,
    /* struct_size */ sizeof(AgentxxPluginSchedulerIface),
    /* is_io_thread */ xx_is_io_thread,
    /* post_to_io */ xx_post_to_io,
    /* sleep */ xx_sleep,
    /* op_cancel */ xx_op_cancel,
    /* offload */ xx_offload,
};

static const AgentxxPluginSessionIface g_ifaceSession = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_SESSION_VERSION,
    /* struct_size */ sizeof(AgentxxPluginSessionIface),
    /* get_share_store */ xx_get_share_store,
    /* emit_message_tip */ xx_emit_message_tip,
    /* add_share_store */ xx_add_share_store,
};

static const AgentxxPluginsIface g_ifacePlugins = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS_VERSION,
    /* struct_size */ sizeof(AgentxxPluginsIface),
    /* list_plugins */ xx_list_plugins,
    /* get_plugin */ xx_get_plugin,
    /* get_own_info */ xx_get_own_info,
};

static const AgentxxPluginConfigIface g_ifaceConfig = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_CONFIG_VERSION,
    /* struct_size */ sizeof(AgentxxPluginConfigIface),
    /* get_config */ xx_get_config,
    /* get_plugin_args */ xx_get_plugin_args,
    /* get_tool_prompt */ xx_get_tool_prompt,
    /* get_session_work_dir */ xx_get_session_work_dir,
    /* get_plugin_config_path */ xx_get_plugin_config_path,
    /* get_language */ xx_get_language,
    /* set_language */ xx_set_language,
};

static const AgentxxPluginPromptIface g_ifacePrompt = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_VERSION,
    /* struct_size */ sizeof(AgentxxPluginPromptIface),
    /* get_prompt */ xx_get_prompt,
    /* set_prompt */ xx_set_prompt,
};

static const AgentxxPluginJsonIface g_ifaceJson = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_JSON_VERSION,
    /* struct_size */ sizeof(AgentxxPluginJsonIface),
    /* json_get_string */ xx_json_get_string,
    /* json_escape */ xx_json_escape,
};

static const AgentxxPluginLogIface g_ifaceLog = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_LOG_VERSION,
    /* struct_size */ sizeof(AgentxxPluginLogIface),
    /* log */ xx_log,
};

static const AgentxxPluginResourcesIface g_ifaceResources = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_VERSION,
    /* struct_size */ sizeof(AgentxxPluginResourcesIface),
    /* register_skill_dir */ xx_register_skill_dir,
    /* unregister_skill_dir */ xx_unregister_skill_dir,
    /* register_memory_file */ xx_register_memory_file,
    /* unregister_memory_file */ xx_unregister_memory_file,
    /* register_mcp_server */ xx_register_mcp_server,
    /* unregister_mcp_server */ xx_unregister_mcp_server,
    /* get_own_resources */ xx_get_own_resources,
};

static const AgentxxPluginModelIface g_ifaceModel = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_MODEL_VERSION,
    /* struct_size */ sizeof(AgentxxPluginModelIface),
    /* get_config */ xx_model_get_config,
};

static const AgentxxPluginCancelIface g_ifaceCancel = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_CANCEL_VERSION,
    /* struct_size */ sizeof(AgentxxPluginCancelIface),
    /* is_cancelled */ xx_cancel_is_cancelled,
};

static const AgentxxPluginGraphIface g_ifaceGraph = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_GRAPH_VERSION,
    /* struct_size */ sizeof(AgentxxPluginGraphIface),
    /* register_node_type */ xx_register_node_type,
    /* unregister_node_type */ xx_unregister_node_type,
    /* get_graph_json */ xx_get_graph_json,
    /* get_graph_name */ xx_get_graph_name,
    /* set_graph_json */ xx_set_graph_json,
};

static const AgentxxPluginTasksIface g_ifaceTasks = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_TASKS_VERSION,
    /* struct_size */ sizeof(AgentxxPluginTasksIface),
    /* register_task */ xx_register_task,
    /* cancel_task */ xx_op_cancel,
};

const void* AGENTXX_PLUGIN_CALL
    xx_query_interface(const AgentxxPluginHost*, const AgentxxPluginStringView* iid);

static const AgentxxHostVtable g_hostVtable = {
    /* alloc */ xx_alloc,
    /* free */ xx_free,
    /* query_interface */ xx_query_interface,
};

const void* AGENTXX_PLUGIN_CALL
    xx_query_interface(const AgentxxPluginHost*, const AgentxxPluginStringView* iid) {
    if (!iid || !iid->data) {
        return nullptr;
    }
    const std::string_view n{iid->data, static_cast<size_t>(iid->size)};
    if (n == "__vtable") {
        return &g_hostVtable;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_TOOLS) {
        return &g_ifaceTools;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_HOOKS) {
        return &g_ifaceHooks;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_EVENTS) {
        return &g_ifaceEvents;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES) {
        return &g_ifaceCapabilities;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER) {
        return &g_ifaceScheduler;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_SESSION) {
        return &g_ifaceSession;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS) {
        return &g_ifacePlugins;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_CONFIG) {
        return &g_ifaceConfig;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_PROMPT) {
        return &g_ifacePrompt;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_JSON) {
        return &g_ifaceJson;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_LOG) {
        return &g_ifaceLog;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES) {
        return &g_ifaceResources;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_MODEL) {
        return &g_ifaceModel;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_CANCEL) {
        return &g_ifaceCancel;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_GRAPH) {
        return &g_ifaceGraph;
    }
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_TASKS) {
        return &g_ifaceTasks;
    }
    return nullptr;
}

// ==================== 宿主状态访问辅助实现 ====================

static std::shared_ptr<agentxx::agent::AgentResourceApplier> getResourceApplier(
    const std::weak_ptr<agentxx::agent::AgentContext>& ctxWeak,
    std::string_view                                   opLabel
) {
    auto ctx = ctxWeak.lock();
    if (!ctx) {
        return nullptr;
    }
    if (!ctx->resourceApplier) {
        XX_LOGW(
            "Plugin resource op `{}` ignored: no resource applier installed (BaseAgent?)",
            opLabel
        );
        return nullptr;
    }
    return ctx->resourceApplier;
}

int PluginManager::registerSkillDir(PluginInstance* inst, AgentxxPluginStringView path) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&path)) {
        return -1;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW(
            "Plugin `{}` registerSkillDir rejected: instance is closing or disabled",
            inst->name
        );
        return -1;
    }
    if (inst->resourcesFrozen) {
        XX_LOGW(
            "Plugin `{}` registerSkillDir rejected: resources frozen (immutable after init)",
            inst->name
        );
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "register_skill_dir");
    if (!ap) {
        return -1;
    }
    std::string pathStr = svToStr(path);
    std::string err;
    if (!ap->addSkillDir(inst->name, pathStr, err)) {
        XX_LOGW("Plugin `{}` register skill dir failed: {}", inst->name, err);
        return -1;
    }
    XX_LOGI("Plugin `{}` registered skill dir `{}`", inst->name, pathStr);
    return 0;
}

int PluginManager::unregisterSkillDir(PluginInstance* inst, AgentxxPluginStringView path) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&path)) {
        return -1;
    }
    if (inst->resourcesFrozen) {
        XX_LOGW(
            "Plugin `{}` unregisterSkillDir rejected: resources frozen (immutable after init)",
            inst->name
        );
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "unregister_skill_dir");
    if (!ap) {
        return -1;
    }
    std::string pathStr = svToStr(path);
    if (!ap->removeSkillDir(inst->name, pathStr)) {
        return -1;
    }
    XX_LOGI("Plugin `{}` unregistered skill dir `{}`", inst->name, pathStr);
    return 0;
}

int PluginManager::registerMemoryFile(PluginInstance* inst, AgentxxPluginStringView path) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&path)) {
        return -1;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW(
            "Plugin `{}` registerMemoryFile rejected: instance is closing or disabled",
            inst->name
        );
        return -1;
    }
    if (inst->resourcesFrozen) {
        XX_LOGW(
            "Plugin `{}` registerMemoryFile rejected: resources frozen (immutable after init)",
            inst->name
        );
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "register_memory_file");
    if (!ap) {
        return -1;
    }
    std::string pathStr = svToStr(path);
    std::string err;
    if (!ap->addMemoryFile(inst->name, pathStr, err)) {
        XX_LOGW("Plugin `{}` register memory file failed: {}", inst->name, err);
        return -1;
    }
    XX_LOGI("Plugin `{}` registered memory file `{}`", inst->name, pathStr);
    return 0;
}

int PluginManager::unregisterMemoryFile(PluginInstance* inst, AgentxxPluginStringView path) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&path)) {
        return -1;
    }
    if (inst->resourcesFrozen) {
        XX_LOGW(
            "Plugin `{}` unregisterMemoryFile rejected: resources frozen (immutable after init)",
            inst->name
        );
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "unregister_memory_file");
    if (!ap) {
        return -1;
    }
    std::string pathStr = svToStr(path);
    if (!ap->removeMemoryFile(inst->name, pathStr)) {
        return -1;
    }
    XX_LOGI("Plugin `{}` unregistered memory file `{}`", inst->name, pathStr);
    return 0;
}

int PluginManager::registerMcpServer(PluginInstance* inst, AgentxxPluginStringView specJson) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&specJson)) {
        return -1;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW(
            "Plugin `{}` registerMcpServer rejected: instance is closing or disabled",
            inst->name
        );
        return -1;
    }
    if (inst->resourcesFrozen) {
        XX_LOGW(
            "Plugin `{}` registerMcpServer rejected: resources frozen (immutable after init)",
            inst->name
        );
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "register_mcp_server");
    if (!ap) {
        return -1;
    }
    try {
        auto j = agentxx::util::Json::parse(
            std::string_view{specJson.data, static_cast<size_t>(specJson.size)}
        );
        auto ns         = j.value("namespace", std::string{});
        auto url        = j.value("url", std::string{});
        int  timeoutSec = 120;
        if (j.contains("timeout")) {
            timeoutSec = j.value("timeout", 120);
        }
        if (ns.empty() || url.empty()) {
            XX_LOGW("Plugin `{}` register_mcp_server failed: namespace/url required", inst->name);
            return -1;
        }
        agentxx::agent::McpServerConfig cfg;
        cfg.url         = url;
        cfg.toolTimeout = std::chrono::seconds{std::max(timeoutSec, 0)};
        std::string err;
        if (!ap->addMcpServer(inst->name, ns, cfg, err)) {
            XX_LOGW("Plugin `{}` register mcp server failed: {}", inst->name, err);
            return -1;
        }
        XX_LOGI("Plugin `{}` registered mcp server `{}` ({})", inst->name, ns, url);
        return 0;
    } catch (const std::exception& e) {
        XX_LOGE("Plugin `{}` register_mcp_server invalid json: {}", inst->name, e.what());
        return -1;
    }
}

int PluginManager::unregisterMcpServer(PluginInstance* inst, AgentxxPluginStringView nameSpace) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&nameSpace)) {
        return -1;
    }
    if (inst->resourcesFrozen) {
        XX_LOGW(
            "Plugin `{}` unregisterMcpServer rejected: resources frozen (immutable after init)",
            inst->name
        );
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "unregister_mcp_server");
    if (!ap) {
        return -1;
    }
    std::string nsStr = svToStr(nameSpace);
    if (!ap->removeMcpServer(inst->name, nsStr)) {
        return -1;
    }
    XX_LOGI("Plugin `{}` unregistered mcp server `{}`", inst->name, nsStr);
    return 0;
}

std::string PluginManager::ownResourcesJson(const PluginInstance* inst) {
    if (!inst) {
        return {};
    }
    auto c = agentContext_.lock();
    if (!c || !c->resourceApplier) {
        return {};
    }
    auto snap    = c->resourceApplier->ownedBy(inst->name);
    auto toArray = [](const std::vector<std::string>& v) {
        agentxx::util::Json a = agentxx::util::Json::array();
        for (const auto& s : v) {
            a.push_back(s);
        }
        return a;
    };
    agentxx::util::Json out;
    out["skills"] = toArray(snap.skillDirs);
    out["memory"] = toArray(snap.memoryFiles);
    out["mcp"]    = toArray(snap.mcpNamespaces);
    return out.dump();
}

void PluginManager::applyDeclaredResources(
    PluginInstance&                        inst,
    const plugin::PluginManifestResources& resources
) {
    if (resources.skillDirs.empty() && resources.memoryFiles.empty()
        && resources.mcpServers.empty()) {
        return;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->resourceApplier) {
        XX_LOGW("Plugin `{}` declared resources skipped (no resource applier)", inst.name);
        return;
    }
    agentxx::agent::PluginResourceDecls decls;
    decls.skillDirs   = resources.skillDirs;
    decls.memoryFiles = resources.memoryFiles;
    for (const auto& [ns, d] : resources.mcpServers) {
        agentxx::agent::McpServerConfig cfg;
        cfg.url              = d.url;
        cfg.toolTimeout      = std::chrono::milliseconds{d.timeoutMs};
        decls.mcpServers[ns] = cfg;
    }
    ctx->resourceApplier->applyDecls(inst.name, decls);
}

std::string PluginManager::getConfigJson() {
    auto c = agentContext_.lock();
    if (!c || !c->agentConfig) {
        return {};
    }
    agentxx::util::Json out;
    out["dataDir"]     = c->agentConfig->dataDir;
    out["projectRoot"] = c->agentConfig->workDir;
    out["language"]    = getLanguage();
#if XX_IS_WIN_D
    out["platform"] = "windows";
#elif XX_IS_MACOS_D
    out["platform"] = "macos";
#elif XX_IS_LINUX_D
    out["platform"] = "linux";
#elif XX_IS_IOS_D
    out["platform"] = "ios";
#elif XX_IS_ANDROID_D
    out["platform"] = "android";
#endif
    return out.dump();
}

std::string PluginManager::getLanguage() {
    auto c = agentContext_.lock();
    if (!c) {
        return "en";
    }
    return c->getLanguage();
}

void PluginManager::setLanguage(std::string_view lang) {
    auto c = agentContext_.lock();
    if (c) {
        c->setLanguage(lang);
    }
}

std::string PluginManager::getToolPromptJson(const std::string& toolName) {
    auto c = agentContext_.lock();
    if (!c || !c->agentConfig) {
        return {};
    }
    const auto& prompts = c->agentConfig->prompt.toolPrompt;
    auto        it      = prompts.find(toolName);
    if (it == prompts.end()) {
        return {};
    }
    agentxx::util::Json out;
    out["depict"]            = it->second.depict;
    agentxx::util::Json args = agentxx::util::Json::object();
    for (const auto& [k, v] : it->second.args) {
        args[k] = v;
    }
    out["args"] = std::move(args);
    return out.dump();
}

std::string PluginManager::getPromptJson() {
    auto c = agentContext_.lock();
    if (!c || !c->agentConfig) {
        return {};
    }
    return c->agentConfig->prompt.toJson().dump();
}

namespace {

/// prompt 贡献模型内部工具（见 [PluginManager::PromptKeyState]）。
///
/// 键名约定：
/// - `system`          -> agentConfig->prompt.systemPrompt
/// - `append:<key>`    -> appendSystemPrompts[key]
/// - `tool:<toolName>` -> toolPrompt[toolName]（结构化值：depict + args）
std::string promptSystemKey() {
    return "system";
}
std::string promptAppendKey(const std::string& key) {
    return "append:" + key;
}
std::string promptToolKey(const std::string& toolName) {
    return "tool:" + toolName;
}

/// 读取键当前值（`nullopt` = 键不存在）。
std::optional<PluginManager::PromptValue>
    readPromptKey(const agentxx::agent::AgentPrompt& prompt, const std::string& key) {
    PluginManager::PromptValue value;
    if (key == promptSystemKey()) {
        // systemPrompt 始终存在（空串表示没有系统提示词）。
        value.isTool = false;
        value.text   = prompt.systemPrompt;
        return value;
    }
    if (key.size() > 7 && key.compare(0, 7, "append:") == 0) {
        const std::string name = key.substr(7);
        auto              it   = prompt.appendSystemPrompts.find(name);
        if (it == prompt.appendSystemPrompts.end()) {
            return std::nullopt;
        }
        value.isTool = false;
        value.text   = it->second;
        return value;
    }
    if (key.size() > 5 && key.compare(0, 5, "tool:") == 0) {
        const std::string name = key.substr(5);
        auto              it   = prompt.toolPrompt.find(name);
        if (it == prompt.toolPrompt.end()) {
            return std::nullopt;
        }
        value.isTool = true;
        value.tool   = it->second;
        return value;
    }
    return std::nullopt;
}

/// 写入键值（`nullopt` = 删除该键；systemPrompt 视为写空串）。
void writePromptKey(
    agentxx::agent::AgentPrompt&                    prompt,
    const std::string&                              key,
    const std::optional<PluginManager::PromptValue>& value
) {
    if (key == promptSystemKey()) {
        prompt.systemPrompt = (value && !value->isTool) ? value->text : std::string{};
        return;
    }
    if (key.size() > 7 && key.compare(0, 7, "append:") == 0) {
        const std::string name = key.substr(7);
        if (value && !value->isTool) {
            prompt.appendSystemPrompts[name] = value->text;
        } else {
            prompt.appendSystemPrompts.erase(name);
        }
        return;
    }
    if (key.size() > 5 && key.compare(0, 5, "tool:") == 0) {
        const std::string name = key.substr(5);
        if (value && value->isTool && value->tool.has_value()) {
            prompt.toolPrompt[name] = *value->tool;
        } else {
            prompt.toolPrompt.erase(name);
        }
    }
}

/// 两次取值是否相同（用于发现"宿主之外"的修改并 rebase 基础值）。
bool samePromptValue(
    const std::optional<PluginManager::PromptValue>& a,
    const std::optional<PluginManager::PromptValue>& b
) {
    if (a.has_value() != b.has_value()) {
        return false;
    }
    if (!a) {
        return true;
    }
    if (a->isTool != b->isTool) {
        return false;
    }
    if (a->isTool) {
        if (a->tool.has_value() != b->tool.has_value()) {
            return false;
        }
        if (!a->tool) {
            return true;
        }
        return a->tool->depict == b->tool->depict && a->tool->args == b->tool->args;
    }
    return a->text == b->text;
}

/// 把 toolPrompt JSON 子对象合并到已有值上（与 AgentPrompt::mergeFromJson 的
/// 部分覆盖语义一致：只覆盖出现的 depict/args 子字段）。
PluginManager::PromptValue mergeToolPromptValue(
    const std::optional<PluginManager::PromptValue>& current,
    const agentxx::util::Json&                       spec
) {
    PluginManager::PromptValue value;
    value.isTool = true;
    agentxx::agent::ToolPrompt tool;
    if (current && current->isTool && current->tool.has_value()) {
        tool = *current->tool;
    }
    if (spec.is_object()) {
        if (spec.contains("depict") && spec["depict"].is_string()) {
            tool.depict = spec["depict"].get<std::string>();
        }
        if (spec.contains("args") && spec["args"].is_object()) {
            for (const auto& [argName, argValue] : spec["args"].items()) {
                if (argValue.is_string()) {
                    tool.args[std::string{argName}] = argValue.get<std::string>();
                }
            }
        }
    }
    value.tool = std::move(tool);
    return value;
}

} // namespace

/// 重新合成单个 prompt 键的有效值：
///   基础值（首次贡献前；被外部修改时 rebase）⊕ 按 sequence 顺序的全部存活贡献
void PluginManager::recomposePromptKey(const std::string& key) {
    auto c = agentContext_.lock();
    if (!c || !c->agentConfig) {
        return;
    }
    auto&      prompt  = c->agentConfig->prompt;
    auto&      state   = promptKeys_[key];
    const auto current = readPromptKey(prompt, key);

    if (!state.base.has_value() && state.contributions.empty() && !state.applied.has_value()) {
        // 首次接触该键：记录基础值
        state.base = current;
    } else if (!samePromptValue(current, state.applied)) {
        // 宿主之外（用户或其他代码）改过：以当前值为新基础，贡献在其上重新应用
        state.base = current;
    }

    std::vector<std::pair<uint64_t, const PromptValue*>> ordered;
    ordered.reserve(state.contributions.size());
    for (const auto& [owner, entry] : state.contributions) {
        (void)owner;
        ordered.emplace_back(entry.first, &entry.second);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    std::optional<PromptValue> effective = state.base;
    for (const auto& [sequence, value] : ordered) {
        (void)sequence;
        effective = *value;
    }
    writePromptKey(prompt, key, effective);
    state.applied = effective;
}

void PluginManager::removePromptContributions(std::string_view owner) {
    const std::string        ownerName{owner};
    std::vector<std::string> touched;
    for (auto& [key, state] : promptKeys_) {
        if (state.contributions.erase(ownerName) > 0) {
            touched.push_back(key);
        }
    }
    for (const auto& key : touched) {
        recomposePromptKey(key);
    }
}

int PluginManager::setPromptJson(PluginInstance* inst, AgentxxPluginStringView prompt_json) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&prompt_json)) {
        return -1;
    }
    if (!acceptsRegistration(inst)) {
        XX_LOGW("Plugin `{}` setPromptJson rejected: instance is closing or disabled", inst->name);
        return -1;
    }
    auto c = agentContext_.lock();
    if (!c || !c->agentConfig) {
        return -1;
    }
    try {
        auto j = agentxx::util::Json::parse(
            std::string_view{prompt_json.data, static_cast<size_t>(prompt_json.size)}
        );
        if (!j.is_object()) {
            return -1;
        }

        std::vector<std::string> touched;
        /// 记录/更新本 owner 对某个键的贡献，并登记待重新合成。
        auto record = [&](const std::string& key, PromptValue value) {
            auto& state = promptKeys_[key];
            state.contributions[inst->name] = {++promptSequence_, std::move(value)};
            touched.push_back(key);
        };

        if (j.contains("systemPrompt") && j["systemPrompt"].is_string()) {
            PromptValue value;
            value.isTool = false;
            value.text   = j["systemPrompt"].get<std::string>();
            record(promptSystemKey(), std::move(value));
        }
        if (j.contains("appendSystemPrompts") && j["appendSystemPrompts"].is_object()) {
            for (const auto& [key, value] : j["appendSystemPrompts"].items()) {
                if (!value.is_string()) {
                    continue;
                }
                PromptValue contribution;
                contribution.isTool = false;
                contribution.text   = value.get<std::string>();
                record(promptAppendKey(std::string{key}), std::move(contribution));
            }
        }
        if (j.contains("toolPrompt") && j["toolPrompt"].is_object()) {
            for (const auto& [toolName, spec] : j["toolPrompt"].items()) {
                if (!spec.is_object()) {
                    continue;
                }
                const std::string key = promptToolKey(std::string{toolName});
                // 与 mergeFromJson 一致：在"当前有效值"上做部分覆盖
                record(key, mergeToolPromptValue(readPromptKey(c->agentConfig->prompt, key), spec));
            }
        }

        for (const auto& key : touched) {
            recomposePromptKey(key);
        }
        return 0;
    } catch (...) {
        return -1;
    }
}

/// 兼容入口：卸载/禁用时删除该 owner 的 prompt 贡献并重新合成。
void PluginManager::restorePromptBackup(PluginInstance* inst) {
    if (!inst) {
        return;
    }
    removePromptContributions(inst->name);
}

std::string PluginManager::getPluginArgsJson(PluginInstance* inst) {
    if (!inst) {
        return "{}";
    }
    return inst->args.is_object() ? inst->args.dump() : "{}";
}

std::string PluginManager::getPluginConfigPath(PluginInstance* inst) {
    if (!inst) {
        return {};
    }
    return inst->configPath;
}

std::string PluginManager::getSessionWorkDir() {
    auto c = agentContext_.lock();
    if (!c || !c->agentConfig) {
        return {};
    }
    return c->agentConfig->resolvedWorkDir();
}

std::string PluginManager::getSessionWorkDir(const std::string& threadId) {
    auto c = agentContext_.lock();
    if (!c) {
        return {};
    }
    auto session = c->getSession(threadId);
    if (session && !session->getWorktreeBinding().path.empty()) {
        return session->getWorktreeBinding().path;
    }
    return getSessionWorkDir();
}

std::string PluginManager::getModelConfigJson() {
    auto c = agentContext_.lock();
    if (!c || !c->agentConfig) {
        return {};
    }
    const auto&         cfg = *c->agentConfig;
    agentxx::util::Json out;
    out["baseUrl"]                       = cfg.model.baseUrl;
    out["apiKey"]                        = cfg.model.apiKey;
    out["modelName"]                     = cfg.model.modelName;
    out["websearchApiUrl"]               = cfg.websearchApiUrl;
    out["websearchConvertHtml2markdown"] = cfg.websearchConvertHtml2markdown;
    if (cfg.websearchModel) {
        agentxx::util::Json wm;
        wm["baseUrl"]                 = cfg.websearchModel->baseUrl;
        wm["apiKey"]                  = cfg.websearchModel->apiKey;
        wm["modelName"]               = cfg.websearchModel->modelName;
        wm["readChunkTimeoutSeconds"] = cfg.websearchModel->maxConcurrentConnections;
        out["websearchModel"]         = wm;
    } else {
        out["websearchModel"] = nullptr;
    }
    out["ragDocsPaths"] = cfg.ragDocsPaths;
    return out.dump();
}

bool PluginManager::isSessionCancelled(const std::string& threadId) {
    auto c = agentContext_.lock();
    if (!c || threadId.empty()) {
        return false;
    }
    auto session = c->getSession(threadId);
    if (!session || !session->getCancelToken()) {
        return false;
    }
    return session->getCancelToken()->is_cancelled();
}

AgentxxPluginString PluginManager::getShareStore(
    PluginInstance*         inst,
    AgentxxPluginStringView session_id,
    int64_t                 id
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&session_id) || id < 0) {
        return AgentxxPluginString{nullptr, 0};
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return AgentxxPluginString{nullptr, 0};
    }
    std::string sid = svToStr(session_id);
    auto val = ctx->middlewareHandleContext->getShareStoreItemValue(sid, static_cast<size_t>(id));
    if (!val.has_value()) {
        return AgentxxPluginString{nullptr, 0};
    }
    return agentxx::plugin::hostMemoryCreateString(*val);
}

int64_t PluginManager::addShareStore(
    PluginInstance*         inst,
    AgentxxPluginStringView session_id,
    AgentxxPluginStringView content
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&session_id)) {
        return -1;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return -1;
    }
    std::string sid  = svToStr(session_id);
    std::string text = svToStr(content);
    return static_cast<int64_t>(ctx->middlewareHandleContext->addShareStoreItemValue(sid, text));
}

void PluginManager::emitMessageTip(
    PluginInstance*         inst,
    AgentxxPluginStringView session_id,
    AgentxxPluginStringView text,
    int32_t                 level
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&session_id)
        || agentxx::plugin::PluginStringView::empty(&text)) {
        return;
    }
    auto ctx = agentContext_.lock();
    if (!ctx) {
        return;
    }
    std::string sid     = svToStr(session_id);
    auto        session = ctx->getSession(sid);
    if (!session || !session->io) {
        return;
    }
    agentxx::agent::WireDelta delta;
    delta.type    = agentxx::agent::WireDelta::Type::MessageUITip;
    delta.text    = svToStr(text);
    delta.tipType = level >= 2 ? agentxx::agent::WireDelta::TipType::Error
                               : (level == 1 ? agentxx::agent::WireDelta::TipType::Warning
                                             : agentxx::agent::WireDelta::TipType::Info);
    delta.seq     = session->nextDeltaSeq();
    session->io->sendToPeer(delta);
}

} // namespace plugin
} // namespace agentxx
