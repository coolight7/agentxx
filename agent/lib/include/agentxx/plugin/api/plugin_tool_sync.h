/*
 * agentxx/plugin/plugin_tool_sync.h —— 纯 C 同步垫片 (两件套模型)
 */
#ifndef AGENTXX_PLUGIN_TOOL_SYNC_H
#define AGENTXX_PLUGIN_TOOL_SYNC_H

#include "agentxx/plugin/api/plugin_api.h"

#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
#include <exception>
extern "C" {
#endif

typedef char* (*AgentxxSyncToolFn)(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView session_id,
    AgentxxPluginStringView tool_call_id,
    volatile int*           cancel_flag,
    char**                  error_out
);

typedef struct AgentxxSyncToolSpec {
    AgentxxPluginStringView name;
    AgentxxPluginStringView description;
    AgentxxPluginStringView parameters_json;
    AgentxxSyncToolFn       execute;
    void*                   user_data;
    long                    default_timeout_ms;
    int                     flags;
} AgentxxSyncToolSpec;

typedef struct AgentxxSyncToolShim {
    const AgentxxPluginHost*                  host;
    const struct AgentxxPluginSchedulerIface* sched;
    AgentxxSyncToolFn                         fn;
    void*                                     ud;
} AgentxxSyncToolShim;

typedef struct AgentxxSyncJob {
    AgentxxSyncToolShim         shim;
    AgentxxPluginOperatorNotify notify;
    char*                       args;
    size_t                      argsSize;
    char*                       tid;
    size_t                      tidSize;
    char*                       tcid;
    size_t                      tcidSize;
    volatile int                cancelFlag;
} AgentxxSyncJob;

static inline char* agentxx_shim_err_dup(const AgentxxPluginHost* host, const char* msg) {
    if (!host || !host->vtable || !host->vtable->strdup || !msg) {
        return NULL;
    }
    return host->vtable->strdup(msg);
}

static inline void* agentxx_sync_job_work(void* ud, volatile int* cancel_flag, char** error_out) {
    AgentxxSyncJob* job = (AgentxxSyncJob*)ud;
#ifdef __cplusplus
    try {
        return (void*)job->shim.fn(
            job->shim.ud,
            agentxx_plugin_sv(job->args, job->argsSize),
            agentxx_plugin_sv(job->tid, job->tidSize),
            agentxx_plugin_sv(job->tcid, job->tcidSize),
            cancel_flag,
            error_out
        );
    } catch (const std::exception& e) {
        if (error_out && !*error_out) {
            *error_out = agentxx_shim_err_dup(job->shim.host, e.what());
        }
        return NULL;
    } catch (...) {
        if (error_out && !*error_out) {
            *error_out = agentxx_shim_err_dup(job->shim.host, "sync tool threw unknown exception");
        }
        return NULL;
    }
#else
    return (void*)job->shim.fn(
        job->shim.ud,
        agentxx_plugin_sv(job->args, job->argsSize),
        agentxx_plugin_sv(job->tid, job->tidSize),
        agentxx_plugin_sv(job->tcid, job->tcidSize),
        cancel_flag,
        error_out
    );
#endif
}

static inline void agentxx_sync_job_done(void* ud, void* result, char* error) {
    AgentxxSyncJob* job     = (AgentxxSyncJob*)ud;
    int             st      = AGENTXX_PLUGIN_OPERATOR_OK;
    char*           payload = (char*)result;

    if (error) {
        st      = AGENTXX_PLUGIN_OPERATOR_FAILED;
        payload = error;
    } else if (!payload) {
        st = AGENTXX_PLUGIN_OPERATOR_CANCELLED;
    }

    if (job->notify.done) {
        job->notify.done(job->notify.host_ud, st, payload);
    }

    if (job->args) {
        free(job->args);
    }
    if (job->tid) {
        free(job->tid);
    }
    if (job->tcid) {
        free(job->tcid);
    }
    free(job);
}

static inline void* agentxx_sync_tool_start(
    void*                              user_data,
    AgentxxPluginStringView            args_json,
    AgentxxPluginStringView            session_id,
    AgentxxPluginStringView            tool_call_id,
    const AgentxxPluginOperatorNotify* notify,
    char**                             error_out
) {
    AgentxxSyncToolShim* shim = (AgentxxSyncToolShim*)user_data;
    if (!shim || !shim->sched || !shim->sched->offload) {
        if (error_out) {
            *error_out
                = agentxx_shim_err_dup(shim ? shim->host : NULL, "scheduler iface not available");
        }
        return NULL;
    }

    AgentxxSyncJob* job = (AgentxxSyncJob*)malloc(sizeof(AgentxxSyncJob));
    if (!job) {
        if (error_out) {
            *error_out = agentxx_shim_err_dup(shim->host, "out of memory allocating job");
        }
        return NULL;
    }
    job->shim       = *shim;
    job->notify     = *notify;
    job->cancelFlag = 0;
    job->args       = NULL;
    job->tid        = NULL;
    job->tcid       = NULL;

    if (args_json.size) {
        job->args = (char*)malloc(args_json.size);
        if (!job->args) {
            free(job);
            if (error_out) {
                *error_out = agentxx_shim_err_dup(shim->host, "out of memory allocating args");
            }
            return NULL;
        }
        memcpy(job->args, args_json.data, args_json.size);
    }
    job->argsSize = args_json.size;

    if (session_id.size) {
        job->tid = (char*)malloc(session_id.size);
        if (!job->tid) {
            if (job->args) {
                free(job->args);
            }
            free(job);
            if (error_out) {
                *error_out
                    = agentxx_shim_err_dup(shim->host, "out of memory allocating session_id");
            }
            return NULL;
        }
        memcpy(job->tid, session_id.data, session_id.size);
    }
    job->tidSize = session_id.size;

    if (tool_call_id.size) {
        job->tcid = (char*)malloc(tool_call_id.size);
        if (!job->tcid) {
            if (job->args) {
                free(job->args);
            }
            if (job->tid) {
                free(job->tid);
            }
            free(job);
            if (error_out) {
                *error_out
                    = agentxx_shim_err_dup(shim->host, "out of memory allocating tool_call_id");
            }
            return NULL;
        }
        memcpy(job->tcid, tool_call_id.data, tool_call_id.size);
    }
    job->tcidSize = tool_call_id.size;

    shim->sched->offload(
        shim->host,
        &job->cancelFlag,
        &agentxx_sync_job_work,
        &agentxx_sync_job_done,
        job
    );
    return job;
}

static inline void agentxx_sync_tool_cancel(void* user_data, void* op) {
    (void)user_data;
    if (!op) {
        return;
    }
    AgentxxSyncJob* job = (AgentxxSyncJob*)op;
    job->cancelFlag     = 1;
}

static inline int agentxx_register_sync_tool(
    const AgentxxPluginHost*   host,
    const AgentxxSyncToolSpec* sync_spec,
    AgentxxSyncToolShim*       out_shim
) {
    if (!host || !host->vtable || !sync_spec || !out_shim || !sync_spec->execute) {
        return -1;
    }
    const AgentxxPluginToolsIface* tools = AGENTXX_PLUGIN_QUERY_IFACE(
        host,
        AgentxxPluginToolsIface,
        AGENTXX_PLUGIN_IFACE_AGENT_TOOLS
    );
    const AgentxxPluginSchedulerIface* sched = AGENTXX_PLUGIN_QUERY_IFACE(
        host,
        AgentxxPluginSchedulerIface,
        AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER
    );
    if (!tools || !tools->register_tool || !sched) {
        return -1;
    }

    out_shim->host  = host;
    out_shim->sched = sched;
    out_shim->fn    = sync_spec->execute;
    out_shim->ud    = sync_spec->user_data;

    AgentxxPluginToolSpec spec;
    spec.name               = sync_spec->name;
    spec.description        = sync_spec->description;
    spec.parameters_json    = sync_spec->parameters_json;
    spec.execute_start      = &agentxx_sync_tool_start;
    spec.execute_cancel     = &agentxx_sync_tool_cancel;
    spec.user_data          = out_shim;
    spec.default_timeout_ms = sync_spec->default_timeout_ms;
    spec.flags              = sync_spec->flags;

    return tools->register_tool(host, &spec);
}

typedef char* (*AgentxxInlineToolFn)(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView session_id,
    AgentxxPluginStringView tool_call_id,
    char**                  error_out
);

typedef struct AgentxxInlineToolSpec {
    AgentxxPluginStringView name;
    AgentxxPluginStringView description;
    AgentxxPluginStringView parameters_json;
    AgentxxInlineToolFn     execute;
    void*                   user_data;
    long                    default_timeout_ms;
    int                     flags;
} AgentxxInlineToolSpec;

typedef struct AgentxxInlineToolShim {
    const AgentxxPluginHost* host;
    AgentxxInlineToolFn      fn;
    void*                    ud;
} AgentxxInlineToolShim;

static inline void* agentxx_inline_tool_start(
    void*                              user_data,
    AgentxxPluginStringView            args_json,
    AgentxxPluginStringView            session_id,
    AgentxxPluginStringView            tool_call_id,
    const AgentxxPluginOperatorNotify* notify,
    char**                             error_out
) {
    AgentxxInlineToolShim* shim = (AgentxxInlineToolShim*)user_data;
    if (!shim || !shim->fn) {
        if (error_out) {
            *error_out = agentxx_shim_err_dup(shim ? shim->host : NULL, "invalid inline tool shim");
        }
        return NULL;
    }

    char* result = NULL;
#ifdef __cplusplus
    try {
        result = shim->fn(shim->ud, args_json, session_id, tool_call_id, error_out);
    } catch (const std::exception& e) {
        if (error_out && !*error_out) {
            *error_out = agentxx_shim_err_dup(shim->host, e.what());
        }
        result = NULL;
    } catch (...) {
        if (error_out && !*error_out) {
            *error_out = agentxx_shim_err_dup(shim->host, "inline tool threw unknown exception");
        }
        result = NULL;
    }
#else
    result = shim->fn(shim->ud, args_json, session_id, tool_call_id, error_out);
#endif

    if (error_out && *error_out) {
        if (notify && notify->done) {
            // 执行期失败：经 notify 上报，error_out 清零避免宿主 double-free / 误判为 start 失败
            char* errPayload = *error_out;
            *error_out       = NULL;
            notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_FAILED, errPayload);
        }
    } else {
        if (notify && notify->done) {
            notify->done(notify->host_ud, AGENTXX_PLUGIN_OPERATOR_OK, result);
        }
    }
    return NULL;
}

static inline int agentxx_register_inline_tool(
    const AgentxxPluginHost*     host,
    const AgentxxInlineToolSpec* inline_spec,
    AgentxxInlineToolShim*       out_shim
) {
    if (!host || !host->vtable || !inline_spec || !out_shim || !inline_spec->execute) {
        return -1;
    }
    const AgentxxPluginToolsIface* tools = AGENTXX_PLUGIN_QUERY_IFACE(
        host,
        AgentxxPluginToolsIface,
        AGENTXX_PLUGIN_IFACE_AGENT_TOOLS
    );
    if (!tools || !tools->register_tool) {
        return -1;
    }

    out_shim->host = host;
    out_shim->fn   = inline_spec->execute;
    out_shim->ud   = inline_spec->user_data;

    AgentxxPluginToolSpec spec;
    spec.name               = inline_spec->name;
    spec.description        = inline_spec->description;
    spec.parameters_json    = inline_spec->parameters_json;
    spec.execute_start      = &agentxx_inline_tool_start;
    spec.execute_cancel     = NULL;
    spec.user_data          = out_shim;
    spec.default_timeout_ms = inline_spec->default_timeout_ms;
    spec.flags              = inline_spec->flags;

    return tools->register_tool(host, &spec);
}

typedef int (*AgentxxSyncHookFn)(
    void*                   user_data,
    AgentxxPluginHookPoint  point,
    AgentxxPluginStringView node_input_json,
    char**                  error_out
);

typedef struct AgentxxSyncHookShim {
    const AgentxxPluginHost* host;
    AgentxxSyncHookFn        fn;
    void*                    ud;
} AgentxxSyncHookShim;

static inline void* agentxx_sync_hook_start(
    void*                              user_data,
    AgentxxPluginHookPoint             point,
    AgentxxPluginStringView            node_input_json,
    const AgentxxPluginOperatorNotify* notify,
    char**                             error_out
) {
    AgentxxSyncHookShim* shim = (AgentxxSyncHookShim*)user_data;
    if (!shim || !shim->fn) {
        if (error_out) {
            *error_out = agentxx_shim_err_dup(shim ? shim->host : NULL, "invalid hook shim");
        }
        return NULL;
    }
    int rc = 0;
#ifdef __cplusplus
    try {
        rc = shim->fn(shim->ud, point, node_input_json, error_out);
    } catch (const std::exception& e) {
        if (error_out && !*error_out) {
            *error_out = agentxx_shim_err_dup(shim->host, e.what());
        }
        rc = -1;
    } catch (...) {
        if (error_out && !*error_out) {
            *error_out = agentxx_shim_err_dup(shim->host, "hook threw unknown exception");
        }
        rc = -1;
    }
#else
    rc = shim->fn(shim->ud, point, node_input_json, error_out);
#endif

    if (notify && notify->done) {
        notify->done(
            notify->host_ud,
            rc == 0 ? AGENTXX_PLUGIN_OPERATOR_OK : AGENTXX_PLUGIN_OPERATOR_FAILED,
            NULL
        );
    }
    return NULL;
}

static inline int agentxx_register_sync_hook(
    const AgentxxPluginHost* host,
    AgentxxPluginHookPoint   point,
    AgentxxSyncHookFn        fn,
    void*                    user_data,
    AgentxxSyncHookShim*     out_shim
) {
    if (!host || !host->vtable || !fn || !out_shim || point < 0
        || point >= AGENTXX_PLUGIN_HOOK_COUNT) {
        return -1;
    }
    const AgentxxPluginHooksIface* hooks = AGENTXX_PLUGIN_QUERY_IFACE(
        host,
        AgentxxPluginHooksIface,
        AGENTXX_PLUGIN_IFACE_AGENT_HOOKS
    );
    if (!hooks || !hooks->register_hook) {
        return -1;
    }

    out_shim->host = host;
    out_shim->fn   = fn;
    out_shim->ud   = user_data;

    AgentxxPluginHookSpec spec;
    spec.point       = point;
    spec.hook_start  = &agentxx_sync_hook_start;
    spec.hook_cancel = NULL;
    spec.user_data   = out_shim;

    return hooks->register_hook(host, &spec);
}

#ifdef __cplusplus
}
#endif

#endif /* AGENTXX_PLUGIN_TOOL_SYNC_H */
