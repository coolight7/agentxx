#include "agentxx/plugin/plugin_manager.h"

#include "agentxx/agent/config_static.h"
#include "agentxx/agent/io/agent_io.h"
#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/agent/resource_applier.h"
#include "agentxx/middlewares/planning.h"
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

static void* xx_alloc(size_t size) {
    return ::malloc(size);
}

static void xx_free(void* ptr) {
    ::free(ptr);
}

static char* xx_strdup(const char* s) {
    if (!s) {
        return nullptr;
    }
    size_t n = std::strlen(s) + 1;
    char*  p = static_cast<char*>(::malloc(n));
    if (p) {
        std::memcpy(p, s, n);
    }
    return p;
}

static int xx_register_tool(const AgentxxPluginHost* host, const AgentxxPluginToolSpec* spec) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !spec || agentxx_plugin_sv_empty(spec->name)) {
        return -1;
    }
    auto                  mgrPtr   = mgr;
    auto                  instPtr  = inst;
    AgentxxPluginToolSpec specCopy = *spec;
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, specCopy]() {
        return mgrPtr->registerTool(instPtr, &specCopy);
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int xx_unregister_tool(const AgentxxPluginHost* host, AgentxxPluginStringView name) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(name)) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string toolName{name.data, name.size};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, toolName]() {
        return mgrPtr->unregisterTool(instPtr, toolName.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

static void xx_op_cancel(::AgentxxPluginOperatorHandle* op) {
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

static ::AgentxxPluginOperatorHandle* xx_call_tool_async(
    const AgentxxPluginHost*      host,
    AgentxxPluginStringView       name,
    AgentxxPluginStringView       args_json,
    AgentxxPluginStringView       session_id,
    AgentxxPluginOperatorCallback cb,
    void*                         ud,
    char**                        error_out
) {
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    std::string toolName{name.data ? name.data : "", name.size};
    std::string args{args_json.data ? args_json.data : "", args_json.size};
    std::string tid{session_id.data ? session_id.data : "", session_id.size};
    return mgr->callToolAsync(inst, toolName.c_str(), args.c_str(), tid.c_str(), cb, ud, error_out);
}

static int xx_register_hook(const AgentxxPluginHost* host, const AgentxxPluginHookSpec* spec) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !spec || spec->point < 0 || spec->point >= AGENTXX_PLUGIN_HOOK_COUNT
        || !spec->hook_start) {
        return -1;
    }
    auto                  mgrPtr   = mgr;
    auto                  instPtr  = inst;
    AgentxxPluginHookSpec specCopy = *spec;
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, specCopy]() {
        return mgrPtr->registerHook(instPtr, &specCopy);
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int xx_unregister_hook(const AgentxxPluginHost* host, AgentxxPluginHookPoint point) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || point < 0 || point >= AGENTXX_PLUGIN_HOOK_COUNT) {
        return -1;
    }
    auto mgrPtr  = mgr;
    auto instPtr = inst;
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, point]() {
        return mgrPtr->unregisterHook(instPtr, point);
    });
    XX_PLUGIN_CATCH_END(-1)
}

static AgentxxPluginSubscription* xx_subscribe(
    const AgentxxPluginHost* host,
    AgentxxPluginStringView  topic,
    void (*handler)(AgentxxPluginStringView event_json, void* ud),
    void* ud
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string topicStr{topic.data ? topic.data : "", topic.size};
    return ioCallSync<AgentxxPluginSubscription*>(
        mgrPtr,
        [mgrPtr, instPtr, topicStr, handler, ud]() {
            return mgrPtr->subscribe(instPtr, topicStr.c_str(), handler, ud);
        }
    );
    XX_PLUGIN_CATCH_END(nullptr)
}

static void xx_unsubscribe(AgentxxPluginSubscription* sub) {
    XX_PLUGIN_CATCH_BEGIN
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
    XX_PLUGIN_CATCH_END_VOID()
}

static int xx_publish(
    const AgentxxPluginHost* host,
    AgentxxPluginStringView  topic,
    AgentxxPluginStringView  event_json
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return -1;
    }
    if (!inst->enabled) {
        XX_LOGW("Plugin `{}` publish ignored (disabled)", inst->name);
        return -1;
    }
    std::string topicStr{topic.data ? topic.data : "", topic.size};
    std::string payload{event_json.data ? event_json.data : "", event_json.size};
    return mgr->publish(topicStr.c_str(), payload.c_str());
    XX_PLUGIN_CATCH_END(-1)
}

static int
    xx_register_capability(const AgentxxPluginHost* host, AgentxxPluginStringView capability) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(capability)) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string cap{capability.data, capability.size};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, cap]() {
        return mgrPtr->registerCapability(instPtr, cap.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int
    xx_unregister_capability(const AgentxxPluginHost* host, AgentxxPluginStringView capability) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(capability)) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string cap{capability.data, capability.size};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, cap]() {
        return mgrPtr->unregisterCapability(instPtr, cap.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int xx_has_capability(const AgentxxPluginHost* host, AgentxxPluginStringView capability) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr || agentxx_plugin_sv_empty(capability)) {
        return 0;
    }
    std::string cap{capability.data, capability.size};
    auto        mgrPtr = mgr;
    return ioCallSync<int>(mgrPtr, [mgrPtr, cap]() {
        return mgrPtr->hasCapability(cap.c_str()) ? 1 : 0;
    });
    XX_PLUGIN_CATCH_END(0)
}

static int xx_register_capability_ex(
    const AgentxxPluginHost*             host,
    AgentxxPluginStringView              capability,
    AgentxxPluginCapabilityStartFunction start,
    AgentxxPluginOperatorCancelFunction  cancel,
    void*                                ctx
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(capability) || !start) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string cap{capability.data, capability.size};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, cap, start, cancel, ctx]() {
        return mgrPtr->registerCapabilityEx(instPtr, cap.c_str(), start, cancel, ctx);
    });
    XX_PLUGIN_CATCH_END(-1)
}

static ::AgentxxPluginOperatorHandle* xx_invoke_capability_async(
    const AgentxxPluginHost*      host,
    AgentxxPluginStringView       capability,
    AgentxxPluginStringView       method,
    AgentxxPluginStringView       args_json,
    AgentxxPluginOperatorCallback cb,
    void*                         ud,
    char**                        error_out
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(capability) || agentxx_plugin_sv_empty(method)) {
        return nullptr;
    }
    std::string cap{capability.data, capability.size};
    std::string m{method.data, method.size};
    std::string args{args_json.data ? args_json.data : "", args_json.size};
    if (args.empty()) {
        args = "{}";
    }
    return mgr
        ->invokeCapabilityAsync(inst, cap.c_str(), m.c_str(), args.c_str(), cb, ud, error_out);
    XX_PLUGIN_CATCH_END(nullptr)
}

static char* xx_list_plugins(const AgentxxPluginHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr) {
        return nullptr;
    }
    auto mgrPtr = mgr;
    auto json   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() {
        return mgrPtr->listPluginsJson();
    });
    return host->vtable->strdup(json.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

static char* xx_get_plugin(const AgentxxPluginHost* host, AgentxxPluginStringView name) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr || agentxx_plugin_sv_empty(name)) {
        return nullptr;
    }
    auto        mgrPtr = mgr;
    std::string pluginName{name.data, name.size};
    auto        json = ioCallSync<std::string>(mgrPtr, [mgrPtr, pluginName]() {
        return mgrPtr->getPluginJson(pluginName);
    });
    if (json.empty()) {
        return nullptr;
    }
    return host->vtable->strdup(json.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

static char* xx_get_own_info(const AgentxxPluginHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    auto        mgrPtr  = mgr;
    std::string ownName = inst->name;
    auto        json    = ioCallSync<std::string>(mgrPtr, [mgrPtr, ownName]() {
        return mgrPtr->getPluginJson(ownName);
    });
    if (json.empty()) {
        return nullptr;
    }
    return host->vtable->strdup(json.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

static char* xx_get_share_store(
    const AgentxxPluginHost* host,
    AgentxxPluginStringView  session_id,
    long long                id
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string tid{session_id.data ? session_id.data : "", session_id.size};
    return ioCallSync<char*>(mgrPtr, [mgrPtr, instPtr, tid, id]() {
        return mgrPtr->getShareStore(instPtr, tid.c_str(), id);
    });
    XX_PLUGIN_CATCH_END(nullptr)
}

static long long xx_add_share_store(
    const AgentxxPluginHost* host,
    AgentxxPluginStringView  session_id,
    AgentxxPluginStringView  content
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string tid{session_id.data ? session_id.data : "", session_id.size};
    std::string txt{content.data ? content.data : "", content.size};
    return ioCallSync<long long>(mgrPtr, [mgrPtr, instPtr, tid, txt]() {
        return mgrPtr->addShareStore(instPtr, tid.c_str(), txt.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

static void xx_emit_message_tip(
    const AgentxxPluginHost* host,
    AgentxxPluginStringView  session_id,
    AgentxxPluginStringView  text,
    int                      level
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string tid{session_id.data ? session_id.data : "", session_id.size};
    std::string msg{text.data ? text.data : "", text.size};
    ioCallSyncVoid(mgrPtr, [mgrPtr, instPtr, tid, msg, level]() {
        mgrPtr->emitMessageTip(instPtr, tid.c_str(), msg.c_str(), level);
    });
    XX_PLUGIN_CATCH_END_VOID()
}

static void* xx_sleep(const AgentxxPluginHost* host, long ms, void (*cb)(void* ud), void* ud) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !cb) {
        return nullptr;
    }
    auto mgrPtr  = mgr;
    auto instPtr = inst;
    return ioCallSync<void*>(mgrPtr, [mgrPtr, instPtr, ms, cb, ud]() {
        return mgrPtr->sleep(instPtr, ms, cb, ud);
    });
    XX_PLUGIN_CATCH_END(nullptr)
}

static void xx_cancel_sleep(const AgentxxPluginHost* host, void* timer) {
    XX_PLUGIN_CATCH_BEGIN
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
    XX_PLUGIN_CATCH_END_VOID()
}

static void xx_offload(
    const AgentxxPluginHost* host,
    volatile int*            cancel_flag,
    void* (*work)(void* ud, volatile int* cancel_flag, char** error_out),
    void (*done)(void* ud, void* result, char* error),
    void* ud
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || !work) {
        return;
    }
    mgr->offload(inst, cancel_flag, work, done, ud);
    XX_PLUGIN_CATCH_END_VOID()
}

static int xx_is_io_thread(const AgentxxPluginHost* host) {
    auto mgr = mgrOf(host);
    return (mgr && mgr->isIoThread()) ? 1 : 0;
}

static void xx_post_to_io(const AgentxxPluginHost* host, void (*fn)(void* ud), void* ud) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr || !fn) {
        return;
    }
    mgr->postToIoAsync([fn, ud]() {
        fn(ud);
    });
    XX_PLUGIN_CATCH_END_VOID()
}

static void xx_pump_io(const AgentxxPluginHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (mgr) {
        mgr->runPendingIoTasks();
    }
    XX_PLUGIN_CATCH_END_VOID()
}

static void xx_log(const AgentxxPluginHost* host, int level, AgentxxPluginStringView msg) {
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
    agentxx::util::xxLogPrint(lv, std::string{msg.data ? msg.data : "", msg.size});
}

static char* xx_json_get_string(
    const AgentxxPluginHost* host,
    AgentxxPluginStringView  json,
    AgentxxPluginStringView  key
) {
    auto inst = instOf(host);
    if (!inst || agentxx_plugin_sv_empty(json) || agentxx_plugin_sv_empty(key)) {
        return nullptr;
    }
    try {
        std::string jsonStr{json.data, json.size};
        std::string keyStr{key.data, key.size};
        auto        j = neograph::json::parse(jsonStr);
        if (j.is_object() && j.contains(keyStr) && j[keyStr].is_string()) {
            return inst->host.vtable->strdup(j[keyStr].get<std::string>().c_str());
        }
    } catch (...) {
    }
    return nullptr;
}

static char* xx_json_escape(const AgentxxPluginHost* host, AgentxxPluginStringView s) {
    auto inst = instOf(host);
    if (!inst || agentxx_plugin_sv_empty(s)) {
        return nullptr;
    }
    std::string out;
    out.reserve(s.size + 2);
    out += '"';
    for (size_t i = 0; i < s.size; ++i) {
        const unsigned char c = static_cast<unsigned char>(s.data[i]);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    out += fmt::format("\\u{:04x}", c);
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    out += '"';
    return inst->host.vtable->strdup(out.c_str());
}

static char* xx_get_config(const AgentxxPluginHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr) {
        return nullptr;
    }
    auto mgrPtr = mgr;
    auto json   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() {
        return mgrPtr->getConfigJson();
    });
    if (json.empty()) {
        return nullptr;
    }
    return host->vtable->strdup(json.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

static char* xx_get_plugin_args(const AgentxxPluginHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    auto mgrPtr  = mgr;
    auto instPtr = inst;
    auto json    = ioCallSync<std::string>(mgrPtr, [mgrPtr, instPtr]() {
        return mgrPtr->getPluginArgsJson(instPtr);
    });
    return host->vtable->strdup(json.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

static char* xx_get_tool_prompt(const AgentxxPluginHost* host, AgentxxPluginStringView tool_name) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr || agentxx_plugin_sv_empty(tool_name)) {
        return nullptr;
    }
    auto        mgrPtr = mgr;
    std::string name{tool_name.data, tool_name.size};
    auto        json = ioCallSync<std::string>(mgrPtr, [mgrPtr, name]() {
        return mgrPtr->getToolPromptJson(name);
    });
    if (json.empty()) {
        return nullptr;
    }
    return host->vtable->strdup(json.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

static char* xx_get_work_dir(const AgentxxPluginHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    auto mgrPtr = mgr;
    auto dir    = ioCallSync<std::string>(mgrPtr, [mgrPtr]() {
        return mgrPtr->getSessionWorkDir();
    });
    if (dir.empty()) {
        return nullptr;
    }
    return host->vtable->strdup(dir.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

static char*
    xx_get_session_work_dir(const AgentxxPluginHost* host, AgentxxPluginStringView thread_id) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    auto        mgrPtr = mgr;
    std::string tid{thread_id.data ? thread_id.data : "", thread_id.size};
    auto        dir = ioCallSync<std::string>(mgrPtr, [mgrPtr, tid]() {
        return mgrPtr->getSessionWorkDir(tid);
    });
    if (dir.empty()) {
        return nullptr;
    }
    return host->vtable->strdup(dir.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

static char* xx_get_prompt(const AgentxxPluginHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr) {
        return nullptr;
    }
    auto mgrPtr = mgr;
    auto json   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() {
        return mgrPtr->getPromptJson();
    });
    if (json.empty()) {
        return nullptr;
    }
    return host->vtable->strdup(json.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

static int xx_set_prompt(const AgentxxPluginHost* host, AgentxxPluginStringView prompt_json) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(prompt_json)) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string jsonStr{prompt_json.data, prompt_json.size};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, jsonStr]() {
        return mgrPtr->setPromptJson(instPtr, jsonStr.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

static char* xx_model_get_config(const AgentxxPluginHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr) {
        return nullptr;
    }
    auto mgrPtr = mgr;
    auto json   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() {
        return mgrPtr->getModelConfigJson();
    });
    if (json.empty()) {
        return nullptr;
    }
    return host->vtable->strdup(json.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

static int
    xx_cancel_is_cancelled(const AgentxxPluginHost* host, AgentxxPluginStringView thread_id) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr || agentxx_plugin_sv_empty(thread_id)) {
        return 0;
    }
    auto        mgrPtr = mgr;
    std::string tid{thread_id.data, thread_id.size};
    return ioCallSync<bool>(
               mgrPtr,
               [mgrPtr, tid]() {
                   return mgrPtr->isSessionCancelled(tid);
               }
           )
               ? 1
               : 0;
    XX_PLUGIN_CATCH_END(0)
}

static int xx_planning_set_planning(
    const AgentxxPluginHost* host,
    AgentxxPluginStringView  thread_id,
    AgentxxPluginStringView  roadmap,
    AgentxxPluginStringView  todos_json,
    AgentxxPluginStringView  notes
) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr = mgrOf(host);
    if (!mgr || agentxx_plugin_sv_empty(thread_id)) {
        return -1;
    }
    auto        mgrPtr = mgr;
    std::string tid    = {thread_id.data, thread_id.size};
    std::string road   = roadmap.data ? std::string{roadmap.data, roadmap.size} : std::string{};
    std::string todos
        = todos_json.data ? std::string{todos_json.data, todos_json.size} : std::string{};
    std::string noteStr = notes.data ? std::string{notes.data, notes.size} : std::string{};
    return ioCallSync<int>(mgrPtr, [mgrPtr, tid, road, todos, noteStr]() {
        return mgrPtr->setSessionPlanning(tid, road, todos, noteStr);
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int xx_register_skill_dir(const AgentxxPluginHost* host, AgentxxPluginStringView path) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(path)) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string p{path.data, path.size};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, p]() {
        return mgrPtr->registerSkillDir(instPtr, p.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int xx_unregister_skill_dir(const AgentxxPluginHost* host, AgentxxPluginStringView path) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(path)) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string p{path.data, path.size};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, p]() {
        return mgrPtr->unregisterSkillDir(instPtr, p.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int xx_register_memory_file(const AgentxxPluginHost* host, AgentxxPluginStringView path) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(path)) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string p{path.data, path.size};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, p]() {
        return mgrPtr->registerMemoryFile(instPtr, p.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int xx_unregister_memory_file(const AgentxxPluginHost* host, AgentxxPluginStringView path) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(path)) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string p{path.data, path.size};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, p]() {
        return mgrPtr->unregisterMemoryFile(instPtr, p.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int
    xx_register_mcp_server(const AgentxxPluginHost* host, AgentxxPluginStringView spec_json) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(spec_json)) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string spec{spec_json.data, spec_json.size};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, spec]() {
        return mgrPtr->registerMcpServer(instPtr, spec.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

static int
    xx_unregister_mcp_server(const AgentxxPluginHost* host, AgentxxPluginStringView name_space) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst || agentxx_plugin_sv_empty(name_space)) {
        return -1;
    }
    auto        mgrPtr  = mgr;
    auto        instPtr = inst;
    std::string ns{name_space.data, name_space.size};
    return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, ns]() {
        return mgrPtr->unregisterMcpServer(instPtr, ns.c_str());
    });
    XX_PLUGIN_CATCH_END(-1)
}

static char* xx_get_own_resources(const AgentxxPluginHost* host) {
    XX_PLUGIN_CATCH_BEGIN
    auto mgr  = mgrOf(host);
    auto inst = instOf(host);
    if (!mgr || !inst) {
        return nullptr;
    }
    auto mgrPtr  = mgr;
    auto instPtr = inst;
    auto json    = ioCallSync<std::string>(mgrPtr, [mgrPtr, instPtr]() {
        return mgrPtr->ownResourcesJson(instPtr);
    });
    if (json.empty()) {
        return nullptr;
    }
    return host->vtable->strdup(json.c_str());
    XX_PLUGIN_CATCH_END(nullptr)
}

static const AgentxxPluginToolsIface g_ifaceTools = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION,
    /* register_tool */ xx_register_tool,
    /* unregister_tool */ xx_unregister_tool,
    /* call_tool_async */ xx_call_tool_async,
    /* op_cancel */ xx_op_cancel,
};

static const AgentxxPluginHooksIface g_ifaceHooks = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_VERSION,
    /* register_hook */ xx_register_hook,
    /* unregister_hook */ xx_unregister_hook,
};

static const AgentxxPluginEventsIface g_ifaceEvents = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_EVENTS_VERSION,
    /* subscribe */ xx_subscribe,
    /* unsubscribe */ xx_unsubscribe,
    /* publish */ xx_publish,
};

static const AgentxxPluginCapabilitiesIface g_ifaceCapabilities = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES_VERSION,
    /* register_capability */ xx_register_capability,
    /* register_capability_ex */ xx_register_capability_ex,
    /* unregister_capability */ xx_unregister_capability,
    /* has_capability */ xx_has_capability,
    /* invoke_capability_async */ xx_invoke_capability_async,
    /* op_cancel */ xx_op_cancel,
};

static const AgentxxPluginSchedulerIface g_ifaceScheduler = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER_VERSION,
    /* is_io_thread */ xx_is_io_thread,
    /* post_to_io */ xx_post_to_io,
    /* pump_io */ xx_pump_io,
    /* sleep */ xx_sleep,
    /* cancel_sleep */ xx_cancel_sleep,
    /* offload */ xx_offload,
};

static const AgentxxPluginSessionIface g_ifaceSession = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_SESSION_VERSION,
    /* get_share_store */ xx_get_share_store,
    /* emit_message_tip */ xx_emit_message_tip,
    /* add_share_store */ xx_add_share_store,
};

static const AgentxxPluginsIface g_ifacePlugins = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS_VERSION,
    /* list_plugins */ xx_list_plugins,
    /* get_plugin */ xx_get_plugin,
    /* get_own_info */ xx_get_own_info,
};

static const AgentxxPluginConfigIface g_ifaceConfig = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_CONFIG_VERSION,
    /* get_config */ xx_get_config,
    /* get_plugin_args */ xx_get_plugin_args,
    /* get_tool_prompt */ xx_get_tool_prompt,
    /* get_work_dir */ xx_get_work_dir,
    /* get_session_work_dir */ xx_get_session_work_dir,
};

static const AgentxxPluginPromptIface g_ifacePrompt = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_VERSION,
    /* get_prompt */ xx_get_prompt,
    /* set_prompt */ xx_set_prompt,
};

static const AgentxxPluginJsonIface g_ifaceJson = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_JSON_VERSION,
    /* json_get_string */ xx_json_get_string,
    /* json_escape */ xx_json_escape,
};

static const AgentxxPluginLogIface g_ifaceLog = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_LOG_VERSION,
    /* log */ xx_log,
};

static const AgentxxPluginResourcesIface g_ifaceResources = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_VERSION,
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
    /* get_config */ xx_model_get_config,
};

static const AgentxxPluginCancelIface g_ifaceCancel = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_CANCEL_VERSION,
    /* is_cancelled */ xx_cancel_is_cancelled,
};

static const AgentxxPluginPlanningIface g_ifacePlanning = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_PLANNING_VERSION,
    /* set_planning */ xx_planning_set_planning,
};

const void* xx_query_interface(const AgentxxPluginHost*, AgentxxPluginStringView iid);

static const AgentxxHostVtable g_hostVtable = {
    /* alloc */ xx_alloc,
    /* free */ xx_free,
    /* strdup */ xx_strdup,
    /* query_interface */ xx_query_interface,
};

const void* xx_query_interface(const AgentxxPluginHost*, AgentxxPluginStringView iid) {
    if (!iid.data) {
        return nullptr;
    }
    const std::string_view n{iid.data, iid.size};
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
    if (n == AGENTXX_PLUGIN_IFACE_AGENT_PLANNING) {
        return &g_ifacePlanning;
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

int PluginManager::registerSkillDir(PluginInstance* inst, const char* path) {
    if (!inst || !path) {
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "register_skill_dir");
    if (!ap) {
        return -1;
    }
    std::string err;
    if (!ap->addSkillDir(inst->name, path, err)) {
        XX_LOGW("Plugin `{}` register skill dir failed: {}", inst->name, err);
        return -1;
    }
    XX_LOGI("Plugin `{}` registered skill dir `{}`", inst->name, path);
    return 0;
}

int PluginManager::unregisterSkillDir(PluginInstance* inst, const char* path) {
    if (!inst || !path) {
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "unregister_skill_dir");
    if (!ap) {
        return -1;
    }
    if (!ap->removeSkillDir(inst->name, path)) {
        return -1;
    }
    XX_LOGI("Plugin `{}` unregistered skill dir `{}`", inst->name, path);
    return 0;
}

int PluginManager::registerMemoryFile(PluginInstance* inst, const char* path) {
    if (!inst || !path) {
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "register_memory_file");
    if (!ap) {
        return -1;
    }
    std::string err;
    if (!ap->addMemoryFile(inst->name, path, err)) {
        XX_LOGW("Plugin `{}` register memory file failed: {}", inst->name, err);
        return -1;
    }
    XX_LOGI("Plugin `{}` registered memory file `{}`", inst->name, path);
    return 0;
}

int PluginManager::unregisterMemoryFile(PluginInstance* inst, const char* path) {
    if (!inst || !path) {
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "unregister_memory_file");
    if (!ap) {
        return -1;
    }
    if (!ap->removeMemoryFile(inst->name, path)) {
        return -1;
    }
    XX_LOGI("Plugin `{}` unregistered memory file `{}`", inst->name, path);
    return 0;
}

int PluginManager::registerMcpServer(PluginInstance* inst, const char* specJson) {
    if (!inst || !specJson) {
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "register_mcp_server");
    if (!ap) {
        return -1;
    }
    try {
        auto j          = neograph::json::parse(specJson);
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

int PluginManager::unregisterMcpServer(PluginInstance* inst, const char* nameSpace) {
    if (!inst || !nameSpace) {
        return -1;
    }
    auto ap = getResourceApplier(agentContext_, "unregister_mcp_server");
    if (!ap) {
        return -1;
    }
    if (!ap->removeMcpServer(inst->name, nameSpace)) {
        return -1;
    }
    XX_LOGI("Plugin `{}` unregistered mcp server `{}`", inst->name, nameSpace);
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
#if defined(_WIN32)
    out["platform"] = "windows";
#elif defined(__APPLE__)
    out["platform"] = "macos";
#else
    out["platform"] = "linux";
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

int PluginManager::setPromptJson(PluginInstance* inst, const char* prompt_json) {
    if (!inst || !prompt_json || !*prompt_json) {
        return -1;
    }
    auto c = agentContext_.lock();
    if (!c || !c->agentConfig) {
        return -1;
    }
    try {
        auto j = neograph::json::parse(prompt_json);
        if (!j.is_object()) {
            return -1;
        }

        if (!inst->promptBackup.backedUpSystem) {
            inst->promptBackup.backedUpSystem       = true;
            inst->promptBackup.systemPrompt         = c->agentConfig->prompt.systemPrompt;
            inst->promptBackup.systemPlanningPrompt = c->agentConfig->prompt.systemPlanningPrompt;
            inst->promptBackup.systemSkillPrompt    = c->agentConfig->prompt.systemSkillPrompt;
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
        pb.backedUpSystem                           = false;
        c->agentConfig->prompt.systemPrompt         = pb.systemPrompt.value_or("");
        c->agentConfig->prompt.systemPlanningPrompt = pb.systemPlanningPrompt.value_or("");
        c->agentConfig->prompt.systemSkillPrompt    = pb.systemSkillPrompt.value_or("");
    }

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

int PluginManager::setSessionPlanning(
    const std::string& threadId,
    const std::string& roadmap,
    const std::string& todosJson,
    const std::string& notes
) {
    if (threadId.empty() || roadmap.empty()) {
        return -1;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return -1;
    }

    std::shared_ptr<agentxx::middleware::PlanningMiddlewareHandle> planHandle;
    for (const auto& h : ctx->middlewareHandleContext->handles) {
        if (h && (h->name == "PlanningMiddlewareHandle" || h->name == "planning_middleware")) {
            planHandle
                = std::dynamic_pointer_cast<agentxx::middleware::PlanningMiddlewareHandle>(h);
            if (planHandle) {
                break;
            }
        }
    }
    if (!planHandle && ctx->planningMiddleware) {
        planHandle = ctx->planningMiddleware;
    }
    if (!planHandle) {
        return -1;
    }

    auto it = planHandle->states.find(threadId);
    std::shared_ptr<agentxx::middleware::PlanningMiddlewareState> state;
    if (it != planHandle->states.end()) {
        state = std::dynamic_pointer_cast<agentxx::middleware::PlanningMiddlewareState>(it->second);
    }
    if (!state) {
        state = std::make_shared<agentxx::middleware::PlanningMiddlewareState>();
        planHandle->states[threadId] = state;
    }

    neograph::json j = neograph::json::object();
    j["roadmap"]     = roadmap;
    if (!todosJson.empty()) {
        try {
            j["todos"] = neograph::json::parse(todosJson);
        } catch (...) {
            return -1;
        }
    }
    if (!notes.empty()) {
        j["notes"] = notes;
    }
    state->plannings[threadId] = std::move(j);
    return 0;
}

char* PluginManager::getShareStore(PluginInstance* inst, const char* session_id, long long id) {
    if (!inst || !session_id) {
        return nullptr;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return nullptr;
    }
    auto it = ctx->middlewareHandleContext->shareStore.find(session_id);
    if (it == ctx->middlewareHandleContext->shareStore.end()) {
        return nullptr;
    }
    auto itemIt = it->second.store.find(static_cast<size_t>(id));
    if (itemIt == it->second.store.end()) {
        return nullptr;
    }
    return inst->host.vtable->strdup(itemIt->second.c_str());
}

long long PluginManager::addShareStore(
    PluginInstance* inst,
    const char*     session_id,
    const char*     content
) {
    if (!inst || !session_id || !content) {
        return -1;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return -1;
    }
    try {
        size_t id = ctx->middlewareHandleContext->addShareStoreItemValue(session_id, content);
        return static_cast<long long>(id);
    } catch (...) {
        return -1;
    }
}

void PluginManager::emitMessageTip(
    PluginInstance* inst,
    const char*     session_id,
    const char*     text,
    int             level
) {
    if (!inst || !session_id || !text) {
        return;
    }
    auto ctx = agentContext_.lock();
    if (!ctx) {
        return;
    }
    auto session = ctx->getSession(session_id);
    if (!session || !session->io) {
        return;
    }
    agentxx::agent::WireDelta delta;
    delta.type    = agentxx::agent::WireDelta::Type::MessageUITip;
    delta.text    = text;
    delta.tipType = level >= 2 ? agentxx::agent::WireDelta::TipType::Error
                               : (level == 1 ? agentxx::agent::WireDelta::TipType::Warning
                                             : agentxx::agent::WireDelta::TipType::Info);
    delta.seq     = session->nextDeltaSeq();
    session->io->sendToPeer(delta);
}

} // namespace plugin
} // namespace agentxx
