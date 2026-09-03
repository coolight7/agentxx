/*
 * agentxx/plugin/plugin_tool_sync.h —— 线程池执行辅助 (API v1 重构版)
 * - 提供辅助函数方便实现将同步函数委托到线程池执行、对接插件框架的异步接口风格
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

#pragma pack(push, 8)

typedef char* (AGENTXX_PLUGIN_CALL *AgentxxSyncToolFn)(
    void*                          user_data,
    const AgentxxPluginStringView* args_json,
    const AgentxxPluginStringView* session_id,
    const AgentxxPluginStringView* tool_call_id,
    volatile int32_t*              cancel_flag,
    AgentxxPluginString*           error_out
);

typedef struct AgentxxSyncToolSpec {
    AgentxxPluginStringView name;
    AgentxxPluginStringView description;
    AgentxxPluginStringView parameters_json;
    AgentxxSyncToolFn       execute;
    void*                   user_data;
    int64_t                 default_timeout_ms;
    int32_t                 flags;
    uint32_t                _reserved;
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
    AgentxxPluginStringView     args;
    AgentxxPluginStringView     tid;
    AgentxxPluginStringView     tcid;
    volatile int32_t            cancelFlag;
    uint32_t                    _reserved;
} AgentxxSyncJob;

#pragma pack(pop)

static inline AgentxxPluginString agentxx_shim_err_dup(const AgentxxPluginHost* host, const char* msg) {
    if (!host || !host->vtable || !msg) {
        AgentxxPluginString s;
        s.data = NULL;
        s.size = 0;
        return s;
    }
    return agentxx_plugin_string_from_cstr(host, msg);
}

static inline void* AGENTXX_PLUGIN_CALL agentxx_sync_job_work(void* ud, volatile int32_t* cancel_flag, AgentxxPluginString* error_out) {
    AgentxxSyncJob* job = (AgentxxSyncJob*)ud;
#ifdef __cplusplus
    try {
        return (void*)job->shim.fn(
            job->shim.ud,
            &job->args,
            &job->tid,
            &job->tcid,
            cancel_flag,
            error_out
        );
    } catch (const std::exception& e) {
        if (error_out && !error_out->data) {
            *error_out = agentxx_shim_err_dup(job->shim.host, e.what());
        }
        return NULL;
    } catch (...) {
        if (error_out && !error_out->data) {
            *error_out = agentxx_shim_err_dup(job->shim.host, "sync tool threw unknown exception");
        }
        return NULL;
    }
#else
    return (void*)job->shim.fn(
        job->shim.ud,
        &job->args,
        &job->tid,
        &job->tcid,
        cancel_flag,
        error_out
    );
#endif
}

static inline void AGENTXX_PLUGIN_CALL agentxx_sync_job_done(void* ud, void* result, const AgentxxPluginStringView* error) {
    AgentxxSyncJob* job = (AgentxxSyncJob*)ud;
    int32_t st = AGENTXX_PLUGIN_OPERATOR_OK;
    AgentxxPluginStringView payload = agentxx_plugin_sv(NULL, 0);

    if (!agentxx_plugin_sv_empty(error)) {
        st = AGENTXX_PLUGIN_OPERATOR_FAILED;
        payload = *error;
    } else if (result) {
        payload = agentxx_plugin_sv_cstr((const char*)result);
    } else {
        st = AGENTXX_PLUGIN_OPERATOR_CANCELLED;
    }

    if (job->notify.done) {
        job->notify.done(job->notify.host_ud, st, &payload);
    }

    if (result && job->shim.host && job->shim.host->vtable && job->shim.host->vtable->free) {
        job->shim.host->vtable->free(result);
    }

    if (job->args.data) {
        free((void*)job->args.data);
    }
    if (job->tid.data) {
        free((void*)job->tid.data);
    }
    if (job->tcid.data) {
        free((void*)job->tcid.data);
    }
    free(job);
}

static inline void* AGENTXX_PLUGIN_CALL agentxx_sync_tool_start(
    void*                              user_data,
    const AgentxxPluginStringView*     args_json,
    const AgentxxPluginStringView*     session_id,
    const AgentxxPluginStringView*     tool_call_id,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error_out
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
    job->args       = agentxx_plugin_sv(NULL, 0);
    job->tid        = agentxx_plugin_sv(NULL, 0);
    job->tcid       = agentxx_plugin_sv(NULL, 0);

    if (args_json && args_json->size) {
        char* buf = (char*)malloc((size_t)args_json->size);
        if (!buf) {
            free(job);
            if (error_out) {
                *error_out = agentxx_shim_err_dup(shim->host, "out of memory allocating args");
            }
            return NULL;
        }
        memcpy(buf, args_json->data, (size_t)args_json->size);
        job->args = agentxx_plugin_sv(buf, args_json->size);
    }

    if (session_id && session_id->size) {
        char* buf = (char*)malloc((size_t)session_id->size);
        if (!buf) {
            if (job->args.data) {
                free((void*)job->args.data);
            }
            free(job);
            if (error_out) {
                *error_out
                    = agentxx_shim_err_dup(shim->host, "out of memory allocating session_id");
            }
            return NULL;
        }
        memcpy(buf, session_id->data, (size_t)session_id->size);
        job->tid = agentxx_plugin_sv(buf, session_id->size);
    }

    if (tool_call_id && tool_call_id->size) {
        char* buf = (char*)malloc((size_t)tool_call_id->size);
        if (!buf) {
            if (job->args.data) {
                free((void*)job->args.data);
            }
            if (job->tid.data) {
                free((void*)job->tid.data);
            }
            free(job);
            if (error_out) {
                *error_out
                    = agentxx_shim_err_dup(shim->host, "out of memory allocating tool_call_id");
            }
            return NULL;
        }
        memcpy(buf, tool_call_id->data, (size_t)tool_call_id->size);
        job->tcid = agentxx_plugin_sv(buf, tool_call_id->size);
    }

    shim->sched->offload(
        shim->host,
        &job->cancelFlag,
        &agentxx_sync_job_work,
        &agentxx_sync_job_done,
        job
    );
    return job;
}

static inline void AGENTXX_PLUGIN_CALL agentxx_sync_tool_cancel(void* user_data, void* op) {
    (void)user_data;
    if (!op) {
        return;
    }
    AgentxxSyncJob* job = (AgentxxSyncJob*)op;
    job->cancelFlag     = 1;
}

static inline int32_t agentxx_register_sync_tool(
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
    spec._reserved          = 0;

    return tools->register_tool(host, &spec);
}

#pragma pack(push, 8)

typedef char* (AGENTXX_PLUGIN_CALL *AgentxxInlineToolFn)(
    void*                          user_data,
    const AgentxxPluginStringView* args_json,
    const AgentxxPluginStringView* session_id,
    const AgentxxPluginStringView* tool_call_id,
    AgentxxPluginString*           error_out
);

typedef struct AgentxxInlineToolSpec {
    AgentxxPluginStringView name;
    AgentxxPluginStringView description;
    AgentxxPluginStringView parameters_json;
    AgentxxInlineToolFn     execute;
    void*                   user_data;
    int64_t                 default_timeout_ms;
    int32_t                 flags;
    uint32_t                _reserved;
} AgentxxInlineToolSpec;

typedef struct AgentxxInlineToolShim {
    const AgentxxPluginHost* host;
    AgentxxInlineToolFn      fn;
    void*                    ud;
} AgentxxInlineToolShim;

#pragma pack(pop)

static inline void* AGENTXX_PLUGIN_CALL agentxx_inline_tool_start(
    void*                              user_data,
    const AgentxxPluginStringView*     args_json,
    const AgentxxPluginStringView*     session_id,
    const AgentxxPluginStringView*     tool_call_id,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error_out
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
        if (error_out && !error_out->data) {
            *error_out = agentxx_shim_err_dup(shim->host, e.what());
        }
        result = NULL;
    } catch (...) {
        if (error_out && !error_out->data) {
            *error_out = agentxx_shim_err_dup(shim->host, "inline tool threw unknown exception");
        }
        result = NULL;
    }
#else
    result = shim->fn(shim->ud, args_json, session_id, tool_call_id, error_out);
#endif

    if (error_out && error_out->data) {
        if (notify && notify->done) {
            AgentxxPluginString errPayload = *error_out;
            error_out->data                = NULL;
            error_out->size                = 0;
            AgentxxPluginStringView errSv  = agentxx_plugin_string_to_sv(&errPayload);
            notify->done(
                notify->host_ud,
                AGENTXX_PLUGIN_OPERATOR_FAILED,
                &errSv
            );
            if (shim->host) {
                agentxx_plugin_string_free(shim->host, &errPayload);
            }
        }
    } else {
        if (notify && notify->done) {
            AgentxxPluginStringView resSv = agentxx_plugin_sv_cstr(result);
            notify->done(
                notify->host_ud,
                AGENTXX_PLUGIN_OPERATOR_OK,
                &resSv
            );
            if (result && shim->host && shim->host->vtable && shim->host->vtable->free) {
                shim->host->vtable->free(result);
            }
        }
    }
    return NULL;
}

static inline int32_t agentxx_register_inline_tool(
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
    spec._reserved          = 0;

    return tools->register_tool(host, &spec);
}

#pragma pack(push, 8)

typedef int32_t (AGENTXX_PLUGIN_CALL *AgentxxSyncHookFn)(
    void*                          user_data,
    int32_t                        point,
    const AgentxxPluginStringView* node_input_json,
    AgentxxPluginString*           error_out
);

typedef struct AgentxxSyncHookShim {
    const AgentxxPluginHost* host;
    AgentxxSyncHookFn        fn;
    void*                    ud;
} AgentxxSyncHookShim;

#pragma pack(pop)

static inline void* AGENTXX_PLUGIN_CALL agentxx_sync_hook_start(
    void*                              user_data,
    int32_t                            point,
    const AgentxxPluginStringView*     node_input_json,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error_out
) {
    AgentxxSyncHookShim* shim = (AgentxxSyncHookShim*)user_data;
    if (!shim || !shim->fn) {
        if (error_out) {
            *error_out = agentxx_shim_err_dup(shim ? shim->host : NULL, "invalid hook shim");
        }
        return NULL;
    }
    int32_t rc = 0;
#ifdef __cplusplus
    try {
        rc = shim->fn(shim->ud, point, node_input_json, error_out);
    } catch (const std::exception& e) {
        if (error_out && !error_out->data) {
            *error_out = agentxx_shim_err_dup(shim->host, e.what());
        }
        rc = -1;
    } catch (...) {
        if (error_out && !error_out->data) {
            *error_out = agentxx_shim_err_dup(shim->host, "hook threw unknown exception");
        }
        rc = -1;
    }
#else
    rc = shim->fn(shim->ud, point, node_input_json, error_out);
#endif

    if (notify && notify->done) {
        AgentxxPluginStringView errSv = agentxx_plugin_sv(NULL, 0);
        if (error_out && error_out->data) {
            errSv = agentxx_plugin_string_to_sv(error_out);
        }
        notify->done(
            notify->host_ud,
            rc == 0 ? AGENTXX_PLUGIN_OPERATOR_OK : AGENTXX_PLUGIN_OPERATOR_FAILED,
            &errSv
        );
        if (error_out && error_out->data && shim->host) {
            agentxx_plugin_string_free(shim->host, error_out);
        }
    }
    return NULL;
}

static inline int32_t agentxx_register_sync_hook(
    const AgentxxPluginHost* host,
    int32_t                  point,
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
    spec._reserved   = 0;
    spec.hook_start  = &agentxx_sync_hook_start;
    spec.hook_cancel = NULL;
    spec.user_data   = out_shim;

    return hooks->register_hook(host, &spec);
}

#ifdef __cplusplus
}
#endif

#endif /* AGENTXX_PLUGIN_TOOL_SYNC_H */
