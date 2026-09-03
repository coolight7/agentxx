#include "agentxx/plugin/plugin_manager.h"

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

static PluginInstance* instOf(const AgentxxPluginHost* host) {
    return (host && host->opaque) ? static_cast<PluginInstance*>(host->opaque) : nullptr;
}

static PluginManager* mgrOf(const AgentxxPluginHost* host) {
    auto inst = instOf(host);
    return inst ? inst->manager.lock().get() : nullptr;
}

// C ABI 内存两件套

static void* AGENTXX_PLUGIN_CALL xx_alloc(uint64_t size) {
    return agentxx::plugin::hostMemoryAlloc(size);
}

static void AGENTXX_PLUGIN_CALL xx_free(void* ptr) {
    agentxx::plugin::hostMemoryFree(ptr);
}

static int32_t AGENTXX_PLUGIN_CALL xx_register_tool(const AgentxxPluginHost* host, const AgentxxPluginToolSpec* spec) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || !spec || agentxx::plugin::PluginStringView::empty(&spec->name)) {
            return -1;
        }
        auto                  mgrPtr   = mgr;
        auto                  instPtr  = inst;
        AgentxxPluginToolSpec specCopy = *spec;
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, instPtr, specCopy]() {
            return mgrPtr->registerTool(instPtr, &specCopy);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_unregister_tool(const AgentxxPluginHost* host, const AgentxxPluginStringView* name) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(name)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto nameVal = *name;
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, instPtr, nameVal]() {
            return mgrPtr->unregisterTool(instPtr, nameVal);
        });
    });
}

static void AGENTXX_PLUGIN_CALL xx_op_cancel(::AgentxxPluginOperatorHandle* op) {
    if (!op) {
        return;
    }
    bool exp = false;
    if (op->cancelled.compare_exchange_strong(exp, true, std::memory_order_acq_rel)) {
        if (op->cancelFn) {
            try {
                op->cancelFn();
            } catch (...) {
            }
        }
    }
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
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !name) {
        return nullptr;
    }
    AgentxxPluginStringView args = args_json ? *args_json : agentxx::plugin::PluginStringView::from("{}", 2);
    AgentxxPluginStringView sid  = session_id ? *session_id : agentxx::plugin::PluginStringView::from("", 0);
    return mgr->callToolAsync(inst, *name, args, sid, cb, ud, error_out);
}

static int32_t AGENTXX_PLUGIN_CALL xx_register_hook(const AgentxxPluginHost* host, const AgentxxPluginHookSpec* spec) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || !spec || spec->point < 0 || spec->point >= AGENTXX_PLUGIN_HOOK_COUNT
            || !spec->hook_start) {
            return -1;
        }
        auto                  mgrPtr   = mgr;
        auto                  instPtr  = inst;
        AgentxxPluginHookSpec specCopy = *spec;
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, instPtr, specCopy]() {
            return mgrPtr->registerHook(instPtr, &specCopy);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_unregister_hook(const AgentxxPluginHost* host, int32_t point) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || point < 0 || point >= AGENTXX_PLUGIN_HOOK_COUNT) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto pt      = static_cast<AgentxxPluginHookPoint>(point);
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, instPtr, pt]() {
            return mgrPtr->unregisterHook(instPtr, pt);
        });
    });
}

static AgentxxPluginSubscription* AGENTXX_PLUGIN_CALL xx_subscribe(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* topic,
    void (AGENTXX_PLUGIN_CALL *handler)(const AgentxxPluginStringView* event_json, void* ud),
    void* ud
) {
    return agentxx::plugin::guardVtableCall(nullptr, [&]() -> AgentxxPluginSubscription* {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || !topic || !handler) {
            return static_cast<AgentxxPluginSubscription*>(nullptr);
        }
        auto mgrPtr   = mgr;
        auto instPtr  = inst;
        auto topicVal = *topic;
        return ioCallSync<AgentxxPluginSubscription*>(
            mgrPtr,
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
        if (!sub->inst) {
            return;
        }
        auto mgr = sub->inst->manager.lock().get();
        if (mgr) {
            ioCallSyncVoid(mgr, [mgr, sub]() {
                mgr->unsubscribe(sub);
            });
        }
        sub->inst = nullptr;
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_publish(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* topic,
    const AgentxxPluginStringView* event_json
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
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
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(capability)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto capVal  = *capability;
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, instPtr, capVal]() {
            return mgrPtr->registerCapability(instPtr, capVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_unregister_capability(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* capability
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(capability)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto capVal  = *capability;
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, instPtr, capVal]() {
            return mgrPtr->unregisterCapability(instPtr, capVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_has_capability(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* capability
) {
    return agentxx::plugin::guardVtableCall(0, [&]() -> int32_t {
        auto mgr = mgrOf(host);
        if (!mgr || agentxx::plugin::PluginStringView::empty(capability)) {
            return 0;
        }
        auto mgrPtr = mgr;
        auto capVal = *capability;
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, capVal]() {
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
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(capability) || !start) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto capVal  = *capability;
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, instPtr, capVal, start, cancel, ctx]() {
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
            auto mgr  = mgrOf(host);
            auto inst = instOf(host);
            if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(capability)
                || agentxx::plugin::PluginStringView::empty(method)) {
                return static_cast<::AgentxxPluginOperatorHandle*>(nullptr);
            }
            AgentxxPluginStringView args = args_json ? *args_json : agentxx::plugin::PluginStringView::from("{}", 2);
            return mgr->invokeCapabilityAsync(
                inst,
                *capability,
                *method,
                args,
                cb,
                ud,
                error_out
            );
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
            auto mgr  = mgrOf(host);
            auto inst = instOf(host);
            if (!mgr || !inst || !notify) {
                return static_cast<::AgentxxPluginOperatorHandle*>(nullptr);
            }
            auto mgrPtr  = mgr;
            auto instPtr = inst;
            return ioCallSync<::AgentxxPluginOperatorHandle*>(
                mgrPtr,
                [mgrPtr, instPtr, cancel_fn, cancel_ud, notify, error_out]() {
                    return mgrPtr->registerTask(
                        instPtr,
                        cancel_fn,
                        cancel_ud,
                        notify,
                        error_out
                    );
                }
            );
        }
    );
}

static int32_t AGENTXX_PLUGIN_CALL xx_list_plugins(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto mgr = mgrOf(host);
        if (!mgr) {
            return -1;
        }
        auto mgrPtr = mgr;
        auto json   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() {
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
        auto mgr = mgrOf(host);
        if (!mgr || agentxx::plugin::PluginStringView::empty(name)) {
            return -1;
        }
        auto        mgrPtr = mgr;
        std::string pluginName{name->data, static_cast<size_t>(name->size)};
        auto        json = ioCallSync<std::string>(mgrPtr, [mgrPtr, pluginName]() {
            return mgrPtr->getPluginJson(pluginName);
        });
        if (json.empty()) {
            return -1;
        }
        hostMemorySetString(out, json);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_get_own_info(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return -1;
        }
        auto        mgrPtr  = mgr;
        std::string ownName = inst->name;
        auto        json    = ioCallSync<std::string>(mgrPtr, [mgrPtr, ownName]() {
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
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(session_id)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto sid     = *session_id;
        *out = ioCallSync<AgentxxPluginString>(mgrPtr, [mgrPtr, instPtr, sid, id]() -> AgentxxPluginString {
            return mgrPtr->getShareStore(instPtr, sid, id);
        });
        return (out->data != nullptr) ? 0 : -1;
    });
}

static int64_t AGENTXX_PLUGIN_CALL xx_add_share_store(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* session_id,
    const AgentxxPluginStringView* content
) {
    return agentxx::plugin::guardVtableCall<int64_t>(-1, [&]() -> int64_t {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || !session_id || !content) {
            return -1;
        }
        auto mgrPtr     = mgr;
        auto instPtr    = inst;
        auto sid        = *session_id;
        auto contentVal = *content;
        return ioCallSync<int64_t>(mgrPtr, [mgrPtr, instPtr, sid, contentVal]() -> int64_t {
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
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || !session_id || !text) {
            return;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto sid     = *session_id;
        auto txt     = *text;
        ioCallSyncVoid(mgrPtr, [mgrPtr, instPtr, sid, txt, level]() {
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
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || !spec || agentxx::plugin::PluginStringView::empty(&spec->type) || !spec->run_start) {
            return -1;
        }
        auto                           mgrPtr   = mgr;
        auto                           instPtr  = inst;
        AgentxxPluginGraphNodeTypeSpec specCopy = *spec;
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, instPtr, specCopy]() {
            return mgrPtr->registerGraphNodeType(instPtr, &specCopy);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_unregister_node_type(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* type
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(type)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto typeVal = *type;
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, instPtr, typeVal]() {
            return mgrPtr->unregisterGraphNodeType(instPtr, typeVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_get_graph_json(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return -1;
        }
        auto mgrPtr = mgr;
        auto json   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() -> std::string {
            return mgrPtr->getGraphJson();
        });
        if (json.empty()) {
            return -1;
        }
        hostMemorySetString(out, json);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_get_graph_name(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return -1;
        }
        auto mgrPtr = mgr;
        auto name   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() -> std::string {
            auto json   = mgrPtr->getGraphJson();
            std::string result = "agentxx.default";
            if (!json.empty()) {
                try {
                    auto j = neograph::json::parse(json);
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

static int32_t AGENTXX_PLUGIN_CALL xx_set_graph_json(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* graph_json
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(graph_json)) {
            return -1;
        }
        auto mgrPtr   = mgr;
        auto instPtr  = inst;
        auto jsonVal  = *graph_json;
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, instPtr, jsonVal]() {
            return mgrPtr->setGraphJson(instPtr, jsonVal);
        });
    });
}

static void* AGENTXX_PLUGIN_CALL xx_sleep(
    const AgentxxPluginHost* host,
    int64_t                  ms,
    void (AGENTXX_PLUGIN_CALL *cb)(void* ud),
    void* ud
) {
    return agentxx::plugin::guardVtableCall<void*>(nullptr, [&]() -> void* {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || !cb) {
            return static_cast<void*>(nullptr);
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        return ioCallSync<void*>(mgrPtr, [mgrPtr, instPtr, ms, cb, ud]() {
            return mgrPtr->sleep(instPtr, ms, cb, ud);
        });
    });
}

static void AGENTXX_PLUGIN_CALL xx_cancel_sleep(const AgentxxPluginHost* host, void* timer) {
    agentxx::plugin::guardVtableCallVoid([&]() {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || !timer) {
            return;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        ioCallSyncVoid(mgrPtr, [mgrPtr, instPtr, timer]() {
            mgrPtr->cancelSleep(instPtr, timer);
        });
    });
}

static void AGENTXX_PLUGIN_CALL xx_offload(
    const AgentxxPluginHost* host,
    volatile int32_t*        cancel_flag,
    void* (AGENTXX_PLUGIN_CALL *work)(void* ud, volatile int32_t* cancel_flag, AgentxxPluginString* error_out),
    void (AGENTXX_PLUGIN_CALL *done)(void* ud, void* result, const AgentxxPluginStringView* error),
    void* ud
) {
    agentxx::plugin::guardVtableCallVoid([&]() {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || !work) {
            return;
        }
        mgr->offload(inst, cancel_flag, work, done, ud);
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_is_io_thread(const AgentxxPluginHost* host) {
    auto mgr = mgrOf(host);
    return (mgr && mgr->isIoThread()) ? 1 : 0;
}

static void AGENTXX_PLUGIN_CALL xx_post_to_io(
    const AgentxxPluginHost* host,
    void (AGENTXX_PLUGIN_CALL *fn)(void* ud),
    void* ud
) {
    agentxx::plugin::guardVtableCallVoid([&]() {
        auto mgr = mgrOf(host);
        if (!mgr || !fn) {
            return;
        }
        mgr->postToIoAsync([fn, ud]() {
            fn(ud);
        });
    });
}

static void AGENTXX_PLUGIN_CALL xx_pump_io(const AgentxxPluginHost* host) {
    agentxx::plugin::guardVtableCallVoid([&]() {
        auto mgr = mgrOf(host);
        if (mgr) {
            mgr->runPendingIoTasks();
        }
    });
}

static void AGENTXX_PLUGIN_CALL xx_log(
    const AgentxxPluginHost*       host,
    int32_t                        level,
    const AgentxxPluginStringView* msg
) {
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
    std::string text = (msg && msg->data) ? std::string(msg->data, static_cast<size_t>(msg->size)) : std::string{};
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
    auto inst = instOf(host);
    if (!inst || agentxx::plugin::PluginStringView::empty(json) || agentxx::plugin::PluginStringView::empty(key)) {
        return -1;
    }
    try {
        std::string jsonStr{json->data, static_cast<size_t>(json->size)};
        std::string keyStr{key->data, static_cast<size_t>(key->size)};
        auto        j = neograph::json::parse(jsonStr);
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
    auto inst = instOf(host);
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

static int32_t AGENTXX_PLUGIN_CALL xx_get_config(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto mgr = mgrOf(host);
        if (!mgr) {
            return -1;
        }
        auto mgrPtr = mgr;
        auto json   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() {
            return mgrPtr->getConfigJson();
        });
        if (json.empty()) {
            return -1;
        }
        hostMemorySetString(out, json);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_get_plugin_args(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto json    = ioCallSync<std::string>(mgrPtr, [mgrPtr, instPtr]() {
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
        auto mgr = mgrOf(host);
        if (!mgr || agentxx::plugin::PluginStringView::empty(tool_name)) {
            return -1;
        }
        auto        mgrPtr = mgr;
        std::string name{tool_name->data, static_cast<size_t>(tool_name->size)};
        auto        json = ioCallSync<std::string>(mgrPtr, [mgrPtr, name]() {
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
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return -1;
        }
        auto        mgrPtr = mgr;
        std::string tid = (thread_id && thread_id->data) ? std::string(thread_id->data, static_cast<size_t>(thread_id->size)) : std::string{};
        auto        dir = ioCallSync<std::string>(mgrPtr, [mgrPtr, tid]() {
            return mgrPtr->getSessionWorkDir(tid);
        });
        if (dir.empty()) {
            return -1;
        }
        hostMemorySetString(out, dir);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_get_plugin_config_path(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto path    = ioCallSync<std::string>(mgrPtr, [mgrPtr, instPtr]() {
            return mgrPtr->getPluginConfigPath(instPtr);
        });
        if (path.empty()) {
            return -1;
        }
        hostMemorySetString(out, path);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_get_prompt(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto mgr = mgrOf(host);
        if (!mgr) {
            return -1;
        }
        auto mgrPtr = mgr;
        auto json   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() {
            return mgrPtr->getPromptJson();
        });
        if (json.empty()) {
            return -1;
        }
        hostMemorySetString(out, json);
        return 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_set_prompt(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* prompt_json
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(prompt_json)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto pJson   = *prompt_json;
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, instPtr, pJson]() {
            return mgrPtr->setPromptJson(instPtr, pJson);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_model_get_config(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto mgr = mgrOf(host);
        if (!mgr) {
            return -1;
        }
        auto mgrPtr = mgr;
        auto json   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() {
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
        auto mgr = mgrOf(host);
        if (!mgr || agentxx::plugin::PluginStringView::empty(thread_id)) {
            return 0;
        }
        auto        mgrPtr = mgr;
        std::string tid{thread_id->data, static_cast<size_t>(thread_id->size)};
        return ioCallSync<bool>(
                   mgrPtr,
                   [mgrPtr, tid]() {
                       return mgrPtr->isSessionCancelled(tid);
                   }
               )
                   ? 1
                   : 0;
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_register_skill_dir(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* path
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(path)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto pathVal = *path;
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, instPtr, pathVal]() {
            return mgrPtr->registerSkillDir(instPtr, pathVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_unregister_skill_dir(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* path
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(path)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto pathVal = *path;
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, instPtr, pathVal]() {
            return mgrPtr->unregisterSkillDir(instPtr, pathVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_register_memory_file(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* path
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(path)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto pathVal = *path;
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, instPtr, pathVal]() {
            return mgrPtr->registerMemoryFile(instPtr, pathVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_unregister_memory_file(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* path
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(path)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto pathVal = *path;
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, instPtr, pathVal]() {
            return mgrPtr->unregisterMemoryFile(instPtr, pathVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_register_mcp_server(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* spec_json
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(spec_json)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto specVal = *spec_json;
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, instPtr, specVal]() {
            return mgrPtr->registerMcpServer(instPtr, specVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_unregister_mcp_server(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* name_space
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx::plugin::PluginStringView::empty(name_space)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto nsVal   = *name_space;
        return ioCallSync<int32_t>(mgrPtr, [mgrPtr, instPtr, nsVal]() {
            return mgrPtr->unregisterMcpServer(instPtr, nsVal);
        });
    });
}

static int32_t AGENTXX_PLUGIN_CALL xx_get_own_resources(const AgentxxPluginHost* host, AgentxxPluginString* out) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int32_t {
        if (!out) {
            return -1;
        }
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto json    = ioCallSync<std::string>(mgrPtr, [mgrPtr, instPtr]() {
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
    /* _reserved */ 0,
    /* register_tool */ xx_register_tool,
    /* unregister_tool */ xx_unregister_tool,
    /* call_tool_async */ xx_call_tool_async,
    /* op_cancel */ xx_op_cancel,
};

static const AgentxxPluginHooksIface g_ifaceHooks = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_VERSION,
    /* _reserved */ 0,
    /* register_hook */ xx_register_hook,
    /* unregister_hook */ xx_unregister_hook,
};

static const AgentxxPluginEventsIface g_ifaceEvents = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_EVENTS_VERSION,
    /* _reserved */ 0,
    /* subscribe */ xx_subscribe,
    /* unsubscribe */ xx_unsubscribe,
    /* publish */ xx_publish,
};

static const AgentxxPluginCapabilitiesIface g_ifaceCapabilities = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES_VERSION,
    /* _reserved */ 0,
    /* register_capability */ xx_register_capability,
    /* register_capability_ex */ xx_register_capability_ex,
    /* unregister_capability */ xx_unregister_capability,
    /* has_capability */ xx_has_capability,
    /* invoke_capability_async */ xx_invoke_capability_async,
    /* op_cancel */ xx_op_cancel,
};

static const AgentxxPluginSchedulerIface g_ifaceScheduler = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER_VERSION,
    /* _reserved */ 0,
    /* is_io_thread */ xx_is_io_thread,
    /* post_to_io */ xx_post_to_io,
    /* pump_io */ xx_pump_io,
    /* sleep */ xx_sleep,
    /* cancel_sleep */ xx_cancel_sleep,
    /* offload */ xx_offload,
};

static const AgentxxPluginSessionIface g_ifaceSession = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_SESSION_VERSION,
    /* _reserved */ 0,
    /* get_share_store */ xx_get_share_store,
    /* emit_message_tip */ xx_emit_message_tip,
    /* add_share_store */ xx_add_share_store,
};

static const AgentxxPluginsIface g_ifacePlugins = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS_VERSION,
    /* _reserved */ 0,
    /* list_plugins */ xx_list_plugins,
    /* get_plugin */ xx_get_plugin,
    /* get_own_info */ xx_get_own_info,
};

static const AgentxxPluginConfigIface g_ifaceConfig = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_CONFIG_VERSION,
    /* _reserved */ 0,
    /* get_config */ xx_get_config,
    /* get_plugin_args */ xx_get_plugin_args,
    /* get_tool_prompt */ xx_get_tool_prompt,
    /* get_session_work_dir */ xx_get_session_work_dir,
    /* get_plugin_config_path */ xx_get_plugin_config_path,
};

static const AgentxxPluginPromptIface g_ifacePrompt = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_VERSION,
    /* _reserved */ 0,
    /* get_prompt */ xx_get_prompt,
    /* set_prompt */ xx_set_prompt,
};

static const AgentxxPluginJsonIface g_ifaceJson = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_JSON_VERSION,
    /* _reserved */ 0,
    /* json_get_string */ xx_json_get_string,
    /* json_escape */ xx_json_escape,
};

static const AgentxxPluginLogIface g_ifaceLog = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_LOG_VERSION,
    /* _reserved */ 0,
    /* log */ xx_log,
};

static const AgentxxPluginResourcesIface g_ifaceResources = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_VERSION,
    /* _reserved */ 0,
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
    /* _reserved */ 0,
    /* get_config */ xx_model_get_config,
};

static const AgentxxPluginCancelIface g_ifaceCancel = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_CANCEL_VERSION,
    /* _reserved */ 0,
    /* is_cancelled */ xx_cancel_is_cancelled,
};

static const AgentxxPluginGraphIface g_ifaceGraph = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_GRAPH_VERSION,
    /* _reserved */ 0,
    /* register_node_type */ xx_register_node_type,
    /* unregister_node_type */ xx_unregister_node_type,
    /* get_graph_json */ xx_get_graph_json,
    /* get_graph_name */ xx_get_graph_name,
    /* set_graph_json */ xx_set_graph_json,
};

static const AgentxxPluginTasksIface g_ifaceTasks = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_TASKS_VERSION,
    /* _reserved */ 0,
    /* register_task */ xx_register_task,
    /* cancel_task */ xx_op_cancel,
};

const void* AGENTXX_PLUGIN_CALL xx_query_interface(const AgentxxPluginHost*, const AgentxxPluginStringView* iid);

static const AgentxxHostVtable g_hostVtable = {
    /* alloc */ xx_alloc,
    /* free */ xx_free,
    /* query_interface */ xx_query_interface,
};

const void* AGENTXX_PLUGIN_CALL xx_query_interface(const AgentxxPluginHost*, const AgentxxPluginStringView* iid) {
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
        auto j          = neograph::json::parse(std::string_view{specJson.data, static_cast<size_t>(specJson.size)});
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
        neograph::json a = neograph::json::array();
        for (const auto& s : v) {
            a.push_back(s);
        }
        return a;
    };
    neograph::json out;
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
    neograph::json out;
    out["dataDir"]     = c->agentConfig->dataDir;
    out["projectRoot"] = c->agentConfig->workDir;
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
    neograph::json out;
    out["depict"]       = it->second.depict;
    neograph::json args = neograph::json::object();
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

int PluginManager::setPromptJson(PluginInstance* inst, AgentxxPluginStringView prompt_json) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&prompt_json)) {
        return -1;
    }
    auto c = agentContext_.lock();
    if (!c || !c->agentConfig) {
        return -1;
    }
    try {
        auto j = neograph::json::parse(std::string_view{prompt_json.data, static_cast<size_t>(prompt_json.size)});
        if (!j.is_object()) {
            return -1;
        }

        if (!inst->promptBackup.backedUpSystem) {
            inst->promptBackup.backedUpSystem = true;
            inst->promptBackup.systemPrompt   = c->agentConfig->prompt.systemPrompt;
        }

        auto backupAppendKey = [&](const std::string& key) {
            if (inst->promptBackup.appendSystemPrompts.find(key)
                != inst->promptBackup.appendSystemPrompts.end()) {
                return;
            }
            auto it = c->agentConfig->prompt.appendSystemPrompts.find(key);
            if (it != c->agentConfig->prompt.appendSystemPrompts.end()) {
                inst->promptBackup.appendSystemPrompts[key] = it->second;
            } else {
                inst->promptBackup.appendSystemPrompts[key] = std::nullopt;
            }
        };

        if (j.contains("appendSystemPrompts") && j["appendSystemPrompts"].is_object()) {
            for (const auto& [key, _] : j["appendSystemPrompts"].items()) {
                backupAppendKey(key);
            }
        }

        if (j.contains("toolPrompt") && j["toolPrompt"].is_object()) {
            for (const auto& [toolName, _] : j["toolPrompt"].items()) {
                if (std::find(
                        inst->promptBackup.backedUpTools.begin(),
                        inst->promptBackup.backedUpTools.end(),
                        toolName
                    )
                    == inst->promptBackup.backedUpTools.end()) {
                    inst->promptBackup.backedUpTools.push_back(toolName);
                    auto it = c->agentConfig->prompt.toolPrompt.find(toolName);
                    if (it != c->agentConfig->prompt.toolPrompt.end()) {
                        inst->promptBackup.toolPrompt[toolName] = it->second;
                    } else {
                        inst->promptBackup.toolPrompt[toolName] = std::nullopt;
                    }
                }
            }
        }

        c->agentConfig->prompt.mergeFromJson(j);
        return 0;
    } catch (...) {
        return -1;
    }
}

void PluginManager::restorePromptBackup(PluginInstance* inst) {
    if (!inst) {
        return;
    }
    auto c = agentContext_.lock();
    if (!c || !c->agentConfig) {
        return;
    }
    auto& pb = inst->promptBackup;

    if (pb.backedUpSystem) {
        pb.backedUpSystem                   = false;
        c->agentConfig->prompt.systemPrompt = pb.systemPrompt.value_or("");
    }
    for (const auto& [key, orig] : pb.appendSystemPrompts) {
        if (orig.has_value()) {
            c->agentConfig->prompt.appendSystemPrompts[key] = *orig;
        } else {
            c->agentConfig->prompt.appendSystemPrompts.erase(key);
        }
    }
    pb.appendSystemPrompts.clear();

    for (const auto& [toolName, origPrompt] : pb.toolPrompt) {
        if (origPrompt.has_value()) {
            c->agentConfig->prompt.toolPrompt[toolName] = *origPrompt;
        } else {
            c->agentConfig->prompt.toolPrompt.erase(toolName);
        }
    }
    pb.toolPrompt.clear();
    pb.backedUpTools.clear();
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
    const auto&    cfg = *c->agentConfig;
    neograph::json out;
    out["baseUrl"]                       = cfg.model.baseUrl;
    out["apiKey"]                        = cfg.model.apiKey;
    out["modelName"]                     = cfg.model.modelName;
    out["websearchApiUrl"]               = cfg.websearchApiUrl;
    out["websearchConvertHtml2markdown"] = cfg.websearchConvertHtml2markdown;
    if (cfg.websearchModel) {
        neograph::json wm;
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

AgentxxPluginString PluginManager::getShareStore(PluginInstance* inst, AgentxxPluginStringView session_id, int64_t id) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&session_id) || id < 0) {
        return AgentxxPluginString{nullptr, 0};
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return AgentxxPluginString{nullptr, 0};
    }
    std::string sid = svToStr(session_id);
    auto        val = ctx->middlewareHandleContext->getShareStoreItemValue(sid, static_cast<size_t>(id));
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
    return static_cast<int64_t>(
        ctx->middlewareHandleContext->addShareStoreItemValue(sid, text)
    );
}

void PluginManager::emitMessageTip(
    PluginInstance*         inst,
    AgentxxPluginStringView session_id,
    AgentxxPluginStringView text,
    int32_t                 level
) {
    if (!inst || agentxx::plugin::PluginStringView::empty(&session_id) || agentxx::plugin::PluginStringView::empty(&text)) {
        return;
    }
    auto ctx = agentContext_.lock();
    if (!ctx) {
        return;
    }
    std::string sid = svToStr(session_id);
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
