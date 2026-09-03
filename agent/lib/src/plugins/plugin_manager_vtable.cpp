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

// C ABI 内存三件套

static void* xx_alloc(size_t size) {
    return agentxx::plugin::hostMemoryAlloc(size);
}

static void xx_free(void* ptr) {
    agentxx::plugin::hostMemoryFree(ptr);
}

static char* xx_strdup(AgentxxPluginStringView s) {
    return agentxx::plugin::hostMemoryStrdup(s);
}

static int xx_register_tool(const AgentxxPluginHost* host, const AgentxxPluginToolSpec* spec) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
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
    });
}

static int xx_unregister_tool(const AgentxxPluginHost* host, AgentxxPluginStringView name) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx_plugin_sv_empty(name)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, name]() {
            return mgrPtr->unregisterTool(instPtr, name);
        });
    });
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
    return mgr->callToolAsync(inst, name, args_json, session_id, cb, ud, error_out);
}

static int xx_register_hook(const AgentxxPluginHost* host, const AgentxxPluginHookSpec* spec) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
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
    });
}

static int xx_unregister_hook(const AgentxxPluginHost* host, AgentxxPluginHookPoint point) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
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
    });
}

static AgentxxPluginSubscription* xx_subscribe(
    const AgentxxPluginHost* host,
    AgentxxPluginStringView  topic,
    void (*handler)(AgentxxPluginStringView event_json, void* ud),
    void* ud
) {
    return agentxx::plugin::guardVtableCall(nullptr, [&]() -> AgentxxPluginSubscription* {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return static_cast<AgentxxPluginSubscription*>(nullptr);
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        return ioCallSync<AgentxxPluginSubscription*>(
            mgrPtr,
            [mgrPtr, instPtr, topic, handler, ud]() {
                return mgrPtr->subscribe(instPtr, topic, handler, ud);
            }
        );
    });
}

static void xx_unsubscribe(AgentxxPluginSubscription* sub) {
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

static int xx_publish(
    const AgentxxPluginHost* host,
    AgentxxPluginStringView  topic,
    AgentxxPluginStringView  event_json
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return -1;
        }
        if (!inst->enabled) {
            XX_LOGW("Plugin `{}` publish ignored (disabled)", inst->name);
            return -1;
        }
        return mgr->publish(topic, event_json);
    });
}

static int
    xx_register_capability(const AgentxxPluginHost* host, AgentxxPluginStringView capability) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx_plugin_sv_empty(capability)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, capability]() {
            return mgrPtr->registerCapability(instPtr, capability);
        });
    });
}

static int
    xx_unregister_capability(const AgentxxPluginHost* host, AgentxxPluginStringView capability) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx_plugin_sv_empty(capability)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, capability]() {
            return mgrPtr->unregisterCapability(instPtr, capability);
        });
    });
}

static int xx_has_capability(const AgentxxPluginHost* host, AgentxxPluginStringView capability) {
    return agentxx::plugin::guardVtableCall(0, [&]() -> int {
        auto mgr = mgrOf(host);
        if (!mgr || agentxx_plugin_sv_empty(capability)) {
            return 0;
        }
        auto mgrPtr = mgr;
        return ioCallSync<int>(mgrPtr, [mgrPtr, capability]() {
            return mgrPtr->hasCapability(capability) ? 1 : 0;
        });
    });
}

static int xx_register_capability_ex(
    const AgentxxPluginHost*             host,
    AgentxxPluginStringView              capability,
    AgentxxPluginCapabilityStartFunction start,
    AgentxxPluginOperatorCancelFunction  cancel,
    void*                                ctx
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx_plugin_sv_empty(capability) || !start) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, capability, start, cancel, ctx]() {
            return mgrPtr->registerCapabilityEx(instPtr, capability, start, cancel, ctx);
        });
    });
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
    return agentxx::plugin::guardVtableCall<::AgentxxPluginOperatorHandle*>(
        nullptr,
        [&]() -> ::AgentxxPluginOperatorHandle* {
            auto mgr  = mgrOf(host);
            auto inst = instOf(host);
            if (!mgr || !inst || agentxx_plugin_sv_empty(capability)
                || agentxx_plugin_sv_empty(method)) {
                return static_cast<::AgentxxPluginOperatorHandle*>(nullptr);
            }
            return mgr->invokeCapabilityAsync(
                inst,
                capability,
                method,
                args_json,
                cb,
                ud,
                error_out
            );
        }
    );
}

static ::AgentxxPluginOperatorHandle* xx_register_task(
    const AgentxxPluginHost*            host,
    AgentxxPluginOperatorCancelFunction cancel_fn,
    void*                               cancel_ud,
    AgentxxPluginOperatorNotify*        notify,
    char**                              error_out
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

static char* xx_list_plugins(const AgentxxPluginHost* host) {
    return agentxx::plugin::guardVtableCall(nullptr, [&]() -> char* {
        auto mgr = mgrOf(host);
        if (!mgr) {
            return static_cast<char*>(nullptr);
        }
        auto mgrPtr = mgr;
        auto json   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() {
            return mgrPtr->listPluginsJson();
        });
        return host->vtable->strdup(agentxx_plugin_sv(json.data(), json.size()));
    });
}

static char* xx_get_plugin(const AgentxxPluginHost* host, AgentxxPluginStringView name) {
    return agentxx::plugin::guardVtableCall(nullptr, [&]() -> char* {
        auto mgr = mgrOf(host);
        if (!mgr || agentxx_plugin_sv_empty(name)) {
            return static_cast<char*>(nullptr);
        }
        auto        mgrPtr = mgr;
        std::string pluginName{name.data, name.size};
        auto        json = ioCallSync<std::string>(mgrPtr, [mgrPtr, pluginName]() {
            return mgrPtr->getPluginJson(pluginName);
        });
        if (json.empty()) {
            return static_cast<char*>(nullptr);
        }
        return host->vtable->strdup(agentxx_plugin_sv(json.data(), json.size()));
    });
}

static char* xx_get_own_info(const AgentxxPluginHost* host) {
    return agentxx::plugin::guardVtableCall<char*>(nullptr, [&]() -> char* {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return static_cast<char*>(nullptr);
        }
        auto        mgrPtr  = mgr;
        std::string ownName = inst->name;
        auto        json    = ioCallSync<std::string>(mgrPtr, [mgrPtr, ownName]() {
            return mgrPtr->getPluginJson(ownName);
        });
        if (json.empty()) {
            return static_cast<char*>(nullptr);
        }
        return host->vtable->strdup(agentxx_plugin_sv(json.data(), json.size()));
    });
}

static char* xx_get_share_store(
    const AgentxxPluginHost* host,
    AgentxxPluginStringView  session_id,
    long long                id
) {
    return agentxx::plugin::guardVtableCall<char*>(nullptr, [&]() -> char* {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return static_cast<char*>(nullptr);
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        return ioCallSync<char*>(mgrPtr, [mgrPtr, instPtr, session_id, id]() -> char* {
            return mgrPtr->getShareStore(instPtr, session_id, id);
        });
    });
}

static long long xx_add_share_store(
    const AgentxxPluginHost* host,
    AgentxxPluginStringView  session_id,
    AgentxxPluginStringView  content
) {
    return agentxx::plugin::guardVtableCall<int64_t>(-1, [&]() -> int64_t {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        return ioCallSync<int64_t>(mgrPtr, [mgrPtr, instPtr, session_id, content]() -> int64_t {
            return mgrPtr->addShareStore(instPtr, session_id, content);
        });
    });
}

static void xx_emit_message_tip(
    const AgentxxPluginHost* host,
    AgentxxPluginStringView  session_id,
    AgentxxPluginStringView  text,
    int                      level
) {
    agentxx::plugin::guardVtableCallVoid([&]() {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        ioCallSyncVoid(mgrPtr, [mgrPtr, instPtr, session_id, text, level]() {
            mgrPtr->emitMessageTip(instPtr, session_id, text, level);
        });
    });
}

// =====================================================================
// graph 接口表 (agentxx.agent.graph)
// =====================================================================

static int xx_register_node_type(
    const AgentxxPluginHost*             host,
    const AgentxxPluginGraphNodeTypeSpec* spec
) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || !spec || agentxx_plugin_sv_empty(spec->type) || !spec->run_start) {
            return -1;
        }
        auto                        mgrPtr   = mgr;
        auto                        instPtr  = inst;
        AgentxxPluginGraphNodeTypeSpec specCopy = *spec;
        return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, specCopy]() {
            return mgrPtr->registerGraphNodeType(instPtr, &specCopy);
        });
    });
}

static int xx_unregister_node_type(const AgentxxPluginHost* host, AgentxxPluginStringView type) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx_plugin_sv_empty(type)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, type]() {
            return mgrPtr->unregisterGraphNodeType(instPtr, type);
        });
    });
}

static char* xx_get_graph_json(const AgentxxPluginHost* host) {
    return agentxx::plugin::guardVtableCall<char*>(nullptr, [&]() -> char* {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return nullptr;
        }
        auto mgrPtr = mgr;
        return ioCallSync<char*>(mgrPtr, [mgrPtr]() -> char* {
            auto json = mgrPtr->getGraphJson();
            if (json.empty()) {
                return nullptr;
            }
            return agentxx::plugin::hostMemoryStrdup(json.c_str());
        });
    });
}

static char* xx_get_graph_name(const AgentxxPluginHost* host) {
    return agentxx::plugin::guardVtableCall<char*>(nullptr, [&]() -> char* {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return nullptr;
        }
        auto        mgrPtr = mgr;
        std::string name   = "agentxx.default";
        return ioCallSync<char*>(mgrPtr, [mgrPtr, name]() -> char* {
            auto json = mgrPtr->getGraphJson();
            std::string result = name;
            if (!json.empty()) {
                try {
                    auto j = neograph::json::parse(json);
                    if (j.is_object() && j.contains("name") && j["name"].is_string()) {
                        result = j["name"].get<std::string>();
                    }
                } catch (...) {
                }
            }
            return agentxx::plugin::hostMemoryStrdup(result.c_str());
        });
    });
}

static int xx_set_graph_json(const AgentxxPluginHost* host, AgentxxPluginStringView graph_json) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx_plugin_sv_empty(graph_json)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, graph_json]() {
            return mgrPtr->setGraphJson(instPtr, graph_json);
        });
    });
}

static void* xx_sleep(const AgentxxPluginHost* host, long ms, void (*cb)(void* ud), void* ud) {
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

static void xx_cancel_sleep(const AgentxxPluginHost* host, void* timer) {
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

static void xx_offload(
    const AgentxxPluginHost* host,
    volatile int*            cancel_flag,
    void* (*work)(void* ud, volatile int* cancel_flag, char** error_out),
    void (*done)(void* ud, void* result, AgentxxPluginStringView error),
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

static int xx_is_io_thread(const AgentxxPluginHost* host) {
    auto mgr = mgrOf(host);
    return (mgr && mgr->isIoThread()) ? 1 : 0;
}

static void xx_post_to_io(const AgentxxPluginHost* host, void (*fn)(void* ud), void* ud) {
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

static void xx_pump_io(const AgentxxPluginHost* host) {
    agentxx::plugin::guardVtableCallVoid([&]() {
        auto mgr = mgrOf(host);
        if (mgr) {
            mgr->runPendingIoTasks();
        }
    });
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
            std::string val = j[keyStr].get<std::string>();
            return inst->host.vtable->strdup(agentxx_plugin_sv(val.data(), val.size()));
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
    return inst->host.vtable->strdup(agentxx_plugin_sv(out.data(), out.size()));
}

static char* xx_get_config(const AgentxxPluginHost* host) {
    return agentxx::plugin::guardVtableCall(nullptr, [&]() -> char* {
        auto mgr = mgrOf(host);
        if (!mgr) {
            return static_cast<char*>(nullptr);
        }
        auto mgrPtr = mgr;
        auto json   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() {
            return mgrPtr->getConfigJson();
        });
        if (json.empty()) {
            return static_cast<char*>(nullptr);
        }
        return host->vtable->strdup(agentxx_plugin_sv(json.data(), json.size()));
    });
}

static char* xx_get_plugin_args(const AgentxxPluginHost* host) {
    return agentxx::plugin::guardVtableCall(nullptr, [&]() -> char* {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return static_cast<char*>(nullptr);
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto json    = ioCallSync<std::string>(mgrPtr, [mgrPtr, instPtr]() {
            return mgrPtr->getPluginArgsJson(instPtr);
        });
        return host->vtable->strdup(agentxx_plugin_sv(json.data(), json.size()));
    });
}

static char* xx_get_tool_prompt(const AgentxxPluginHost* host, AgentxxPluginStringView tool_name) {
    return agentxx::plugin::guardVtableCall(nullptr, [&]() -> char* {
        auto mgr = mgrOf(host);
        if (!mgr || agentxx_plugin_sv_empty(tool_name)) {
            return static_cast<char*>(nullptr);
        }
        auto        mgrPtr = mgr;
        std::string name{tool_name.data, tool_name.size};
        auto        json = ioCallSync<std::string>(mgrPtr, [mgrPtr, name]() {
            return mgrPtr->getToolPromptJson(name);
        });
        if (json.empty()) {
            return static_cast<char*>(nullptr);
        }
        return host->vtable->strdup(agentxx_plugin_sv(json.data(), json.size()));
    });
}

static char*
    xx_get_session_work_dir(const AgentxxPluginHost* host, AgentxxPluginStringView thread_id) {
    return agentxx::plugin::guardVtableCall(nullptr, [&]() -> char* {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return static_cast<char*>(nullptr);
        }
        auto        mgrPtr = mgr;
        std::string tid{thread_id.data ? thread_id.data : "", thread_id.size};
        auto        dir = ioCallSync<std::string>(mgrPtr, [mgrPtr, tid]() {
            return mgrPtr->getSessionWorkDir(tid);
        });
        if (dir.empty()) {
            return static_cast<char*>(nullptr);
        }
        return host->vtable->strdup(agentxx_plugin_sv(dir.data(), dir.size()));
    });
}

static char* xx_get_plugin_config_path(const AgentxxPluginHost* host) {
    return agentxx::plugin::guardVtableCall(nullptr, [&]() -> char* {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return static_cast<char*>(nullptr);
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto path    = ioCallSync<std::string>(mgrPtr, [mgrPtr, instPtr]() {
            return mgrPtr->getPluginConfigPath(instPtr);
        });
        if (path.empty()) {
            return static_cast<char*>(nullptr);
        }
        return host->vtable->strdup(agentxx_plugin_sv(path.data(), path.size()));
    });
}

static char* xx_get_prompt(const AgentxxPluginHost* host) {
    return agentxx::plugin::guardVtableCall(nullptr, [&]() -> char* {
        auto mgr = mgrOf(host);
        if (!mgr) {
            return static_cast<char*>(nullptr);
        }
        auto mgrPtr = mgr;
        auto json   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() {
            return mgrPtr->getPromptJson();
        });
        if (json.empty()) {
            return static_cast<char*>(nullptr);
        }
        return host->vtable->strdup(agentxx_plugin_sv(json.data(), json.size()));
    });
}

static int xx_set_prompt(const AgentxxPluginHost* host, AgentxxPluginStringView prompt_json) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx_plugin_sv_empty(prompt_json)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, prompt_json]() {
            return mgrPtr->setPromptJson(instPtr, prompt_json);
        });
    });
}

static char* xx_model_get_config(const AgentxxPluginHost* host) {
    return agentxx::plugin::guardVtableCall(nullptr, [&]() -> char* {
        auto mgr = mgrOf(host);
        if (!mgr) {
            return static_cast<char*>(nullptr);
        }
        auto mgrPtr = mgr;
        auto json   = ioCallSync<std::string>(mgrPtr, [mgrPtr]() {
            return mgrPtr->getModelConfigJson();
        });
        if (json.empty()) {
            return static_cast<char*>(nullptr);
        }
        return host->vtable->strdup(agentxx_plugin_sv(json.data(), json.size()));
    });
}

static int
    xx_cancel_is_cancelled(const AgentxxPluginHost* host, AgentxxPluginStringView thread_id) {
    return agentxx::plugin::guardVtableCall(0, [&]() -> int {
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
    });
}

static int xx_register_skill_dir(const AgentxxPluginHost* host, AgentxxPluginStringView path) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx_plugin_sv_empty(path)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, path]() {
            return mgrPtr->registerSkillDir(instPtr, path);
        });
    });
}

static int xx_unregister_skill_dir(const AgentxxPluginHost* host, AgentxxPluginStringView path) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx_plugin_sv_empty(path)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, path]() {
            return mgrPtr->unregisterSkillDir(instPtr, path);
        });
    });
}

static int xx_register_memory_file(const AgentxxPluginHost* host, AgentxxPluginStringView path) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx_plugin_sv_empty(path)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, path]() {
            return mgrPtr->registerMemoryFile(instPtr, path);
        });
    });
}

static int xx_unregister_memory_file(const AgentxxPluginHost* host, AgentxxPluginStringView path) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx_plugin_sv_empty(path)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, path]() {
            return mgrPtr->unregisterMemoryFile(instPtr, path);
        });
    });
}

static int
    xx_register_mcp_server(const AgentxxPluginHost* host, AgentxxPluginStringView spec_json) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx_plugin_sv_empty(spec_json)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, spec_json]() {
            return mgrPtr->registerMcpServer(instPtr, spec_json);
        });
    });
}

static int
    xx_unregister_mcp_server(const AgentxxPluginHost* host, AgentxxPluginStringView name_space) {
    return agentxx::plugin::guardVtableCall(-1, [&]() -> int {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst || agentxx_plugin_sv_empty(name_space)) {
            return -1;
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        return ioCallSync<int>(mgrPtr, [mgrPtr, instPtr, name_space]() {
            return mgrPtr->unregisterMcpServer(instPtr, name_space);
        });
    });
}

static char* xx_get_own_resources(const AgentxxPluginHost* host) {
    return agentxx::plugin::guardVtableCall(nullptr, [&]() -> char* {
        auto mgr  = mgrOf(host);
        auto inst = instOf(host);
        if (!mgr || !inst) {
            return static_cast<char*>(nullptr);
        }
        auto mgrPtr  = mgr;
        auto instPtr = inst;
        auto json    = ioCallSync<std::string>(mgrPtr, [mgrPtr, instPtr]() {
            return mgrPtr->ownResourcesJson(instPtr);
        });
        if (json.empty()) {
            return static_cast<char*>(nullptr);
        }
        return host->vtable->strdup(agentxx_plugin_sv(json.data(), json.size()));
    });
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
    /* get_session_work_dir */ xx_get_session_work_dir,
    /* get_plugin_config_path */ xx_get_plugin_config_path,
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

static const AgentxxPluginGraphIface g_ifaceGraph = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_GRAPH_VERSION,
    /* register_node_type */ xx_register_node_type,
    /* unregister_node_type */ xx_unregister_node_type,
    /* get_graph_json */ xx_get_graph_json,
    /* get_graph_name */ xx_get_graph_name,
    /* set_graph_json */ xx_set_graph_json,
};

static const AgentxxPluginTasksIface g_ifaceTasks = {
    /* version */ AGENTXX_PLUGIN_IFACE_AGENT_TASKS_VERSION,
    /* register_task */ xx_register_task,
    /* cancel_task */ xx_op_cancel,
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
    if (!inst || agentxx_plugin_sv_empty(path)) {
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
    if (!inst || agentxx_plugin_sv_empty(path)) {
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
    if (!inst || agentxx_plugin_sv_empty(path)) {
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
    if (!inst || agentxx_plugin_sv_empty(path)) {
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
    if (!inst || agentxx_plugin_sv_empty(specJson)) {
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
        auto j          = neograph::json::parse(std::string_view{specJson.data, specJson.size});
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
    if (!inst || agentxx_plugin_sv_empty(nameSpace)) {
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
    if (!inst || agentxx_plugin_sv_empty(prompt_json)) {
        return -1;
    }
    auto c = agentContext_.lock();
    if (!c || !c->agentConfig) {
        return -1;
    }
    try {
        auto j = neograph::json::parse(std::string_view{prompt_json.data, prompt_json.size});
        if (!j.is_object()) {
            return -1;
        }

        if (!inst->promptBackup.backedUpSystem) {
            inst->promptBackup.backedUpSystem = true;
            inst->promptBackup.systemPrompt   = c->agentConfig->prompt.systemPrompt;
        }

        // 通用附加系统提示词：按 key 备份原值 (不存在记 nullopt)，支持多插件并发各管各 key
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

char* PluginManager::getShareStore(PluginInstance* inst, AgentxxPluginStringView session_id, long long id) {
    if (!inst || agentxx_plugin_sv_empty(session_id)) {
        return nullptr;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return nullptr;
    }
    std::string sid = svToStr(session_id);
    auto it = ctx->middlewareHandleContext->shareStore.find(sid);
    if (it == ctx->middlewareHandleContext->shareStore.end()) {
        return nullptr;
    }
    auto itemIt = it->second.store.find(static_cast<size_t>(id));
    if (itemIt == it->second.store.end()) {
        return nullptr;
    }
    return inst->host.vtable->strdup(agentxx_plugin_sv_cstr(itemIt->second.c_str()));
}

long long PluginManager::addShareStore(
    PluginInstance*         inst,
    AgentxxPluginStringView session_id,
    AgentxxPluginStringView content
) {
    if (!inst || agentxx_plugin_sv_empty(session_id) || agentxx_plugin_sv_empty(content)) {
        return -1;
    }
    auto ctx = agentContext_.lock();
    if (!ctx || !ctx->middlewareHandleContext) {
        return -1;
    }
    try {
        std::string sid = svToStr(session_id);
        std::string c   = svToStr(content);
        size_t id = ctx->middlewareHandleContext->addShareStoreItemValue(sid, c);
        return static_cast<long long>(id);
    } catch (...) {
        return -1;
    }
}

void PluginManager::emitMessageTip(
    PluginInstance*         inst,
    AgentxxPluginStringView session_id,
    AgentxxPluginStringView text,
    int                     level
) {
    if (!inst || agentxx_plugin_sv_empty(session_id) || agentxx_plugin_sv_empty(text)) {
        return;
    }
    auto ctx = agentContext_.lock();
    if (!ctx) {
        return;
    }
    std::string sid = svToStr(session_id);
    auto session = ctx->getSession(sid);
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
