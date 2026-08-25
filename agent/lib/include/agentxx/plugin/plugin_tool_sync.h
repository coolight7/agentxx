/*
 * agentxx/plugin/plugin_tool_sync.h —— 纯 C 同步垫片 (header-only, 无任何依赖)
 *
 * 定位: 统一异步操作模型下的同步代码便捷层 —— 把传统"一个同步执行函数"
 * 的工具/钩子包装成异步三件套注册, 插件作者无需了解异步协议即可编写工具。
 *
 * - 【非跨边界 ABI】: 纯头文件内联设施, 编译进插件本体; 不使用也可直接按
 *   plugin_api.h 手写三件套 (纯 C 路径不受影响)
 * - 工具两种模式:
 *   * 内联完成型 agentxx_register_inline_tool: 快同步 (<~1ms, 如 datetime/
 *     echo) —— 在宿主 io 线程直接算完并通知完成, 零线程切换开销
 *   * 阻塞委托型 agentxx_register_sync_tool: 慢同步 (文件遍历/HTTP/子进程)
 *     —— 经 scheduler.offload 委托宿主阻塞池线程执行, 宿主 io 线程只等
 *     完成通知 (与内置慢工具行为一致); 取消标志经 offload 传入执行函数,
 *     长任务可周期轮询协作退出
 * - 钩子便捷注册 agentxx_register_sync_hook: 快同步钩子 (日志/统计等)
 * - 内存约定与手写路径一致: 执行函数返回的结果/错误字符串必须 host->alloc
 *   分配 (所有权移交宿主)
 * - 取消语义约定 (阻塞委托型): 执行函数收到 cancel_flag (1=已取消), 可周期
 *   轮询协作退出; 退出时"返回 NULL 且不设置 error_out"表示已取消 (映射为
 *   AGENTXX_OP_CANCELLED); 也允许忽略取消继续完成 (上报 OK/FAILED)
 * - 适配器上下文为进程级堆分配 (每工具/钩子一次, 随进程存活, 数量级 =
 *   注册数, 可忽略); 需要严格释放的场景请手写三件套
 */
#ifndef AGENTXX_PLUGIN_TOOL_SYNC_H
#define AGENTXX_PLUGIN_TOOL_SYNC_H

#include "agentxx/plugin/plugin_api.h"

#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 * 阻塞委托型工具 (慢同步; 经 scheduler.offload 委托宿主阻塞池)
 * ===================================================================== */

/// 同步执行函数签名 (在宿主阻塞池线程调用; 与旧版 execute 相比多一个
/// cancel_flag 形参): 返回结果 JSON (host->alloc); 失败返回 NULL 并设置
/// *error_out (host->alloc); 已取消返回 NULL 且不设置 error_out
typedef char* (*AgentxxSyncToolFn)(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView thread_id,
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
    long                    default_timeout_ms; ///< 0 = 不限制
    int                     flags;              ///< AGENTXX_TOOL_FLAG_*
} AgentxxSyncToolSpec;

/// 注册阻塞委托型工具。返回 0 成功。
static inline int agentxx_register_sync_tool(const AgentxxHost* host, const AgentxxSyncToolSpec* spec);

/* ---- 实现细节 (插件不得直接使用) ---- */

/// 每工具适配器 (注册时分配, 随进程存活)
typedef struct AgentxxSyncAdapter {
    const AgentxxHost*           host;
    const struct AgentxxSchedulerIface* sched;
    AgentxxSyncToolFn            fn;
    void*                        ud;
} AgentxxSyncAdapter;

/// 单次调用任务 (start 分配, done 释放; 生命周期覆盖整个 offload 过程;
/// 参数字符串在 offload 前拷贝 —— 视图借用仅覆盖 start 调用)
typedef struct AgentxxSyncJob {
    AgentxxSyncAdapter adapter;
    AgentxxOpNotify    notify;
    char*              args;
    size_t             argsSize;
    char*              tid;
    size_t             tidSize;
    char*              tcid;
    size_t             tcidSize;
    volatile int       cancelFlag;
} AgentxxSyncJob;

static inline void* agentxx_sync_job_work(void* ud, volatile int* cancel_flag, char** error_out) {
    AgentxxSyncJob* job = (AgentxxSyncJob*)ud;
    /* cancel_flag 与 job->cancelFlag 为同一指针 (offload 契约: 调用方持有);
     * 执行函数返回 char* (host->alloc), 作为通用指针移交 done */
    return (void*)job->adapter.fn(
        job->adapter.ud,
        agentxx_plugin_sv(job->args, job->argsSize),
        agentxx_plugin_sv(job->tid, job->tidSize),
        agentxx_plugin_sv(job->tcid, job->tcidSize),
        cancel_flag,
        error_out
    );
}

static inline void agentxx_sync_job_done(void* ud, void* result, char* error) {
    AgentxxSyncJob* job = (AgentxxSyncJob*)ud;
    /* 所有权移交通知器: result/error 均为 host->alloc, 由宿主 free */
    int   status  = AGENTXX_OP_OK;
    char* payload = NULL;
    if (error) {
        status  = AGENTXX_OP_FAILED;
        payload = error;
    } else if (!result) {
        status = AGENTXX_OP_CANCELLED; ///< 约定: NULL 结果且无错误 = 已取消
    } else {
        payload = (char*)result;
    }
    job->notify.done(job->notify.host_ud, status, payload);
    free(job->args);
    free(job->tid);
    free(job->tcid);
    free(job);
}

static inline void* agentxx_sync_adapter_start(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView thread_id,
    AgentxxPluginStringView tool_call_id,
    const AgentxxOpNotify*  notify,
    char**                  error_out
) {
    AgentxxSyncAdapter* adapter = (AgentxxSyncAdapter*)user_data;
    AgentxxSyncJob*     job     = (AgentxxSyncJob*)calloc(1, sizeof(AgentxxSyncJob));
    if (!job) {
        if (error_out && adapter->host) {
            *error_out = adapter->host->vtable->strdup("sync tool: out of memory");
        }
        return NULL;
    }
    job->adapter  = *adapter;
    job->notify   = *notify;
    job->argsSize = agentxx_plugin_sv_empty(args_json) ? 2 : args_json.size;
    job->tidSize  = thread_id.size;
    job->tcidSize = tool_call_id.size;
    job->args     = (char*)malloc(job->argsSize ? job->argsSize : 1);
    job->tid      = (char*)malloc(job->tidSize ? job->tidSize : 1);
    job->tcid     = (char*)malloc(job->tcidSize ? job->tcidSize : 1);
    if (!job->args || !job->tid || !job->tcid) {
        free(job->args);
        free(job->tid);
        free(job->tcid);
        free(job);
        if (error_out && adapter->host) {
            *error_out = adapter->host->vtable->strdup("sync tool: out of memory");
        }
        return NULL;
    }
    if (agentxx_plugin_sv_empty(args_json)) {
        memcpy(job->args, "{}", 2);
    } else {
        memcpy(job->args, args_json.data, args_json.size);
    }
    if (thread_id.size) {
        memcpy(job->tid, thread_id.data, thread_id.size);
    }
    if (tool_call_id.size) {
        memcpy(job->tcid, tool_call_id.data, tool_call_id.size);
    }
    /* offload 后立即返回句柄 (execute_poll 留 NULL, 只等完成通知) */
    adapter->sched->offload(
        adapter->host,
        &job->cancelFlag,
        &agentxx_sync_job_work,
        &agentxx_sync_job_done,
        job
    );
    return job;
}

static inline void agentxx_sync_adapter_cancel(void* user_data, void* op) {
    (void)user_data;
    AgentxxSyncJob* job = (AgentxxSyncJob*)op;
    if (job) {
        job->cancelFlag = 1;
    }
}

static inline int agentxx_register_sync_tool(const AgentxxHost* host, const AgentxxSyncToolSpec* spec) {
    if (!host || !spec || !spec->execute) {
        return -1;
    }
    const AgentxxToolsIface* tools
        = AGENTXX_QUERY_IFACE(host, AgentxxToolsIface, AGENTXX_IFACE_AGENT_TOOLS);
    const AgentxxSchedulerIface* sched
        = AGENTXX_QUERY_IFACE(host, AgentxxSchedulerIface, AGENTXX_IFACE_AGENT_SCHEDULER);
    if (!tools || !tools->register_tool || !sched || !sched->offload) {
        return -1;
    }
    AgentxxSyncAdapter* adapter = (AgentxxSyncAdapter*)malloc(sizeof(AgentxxSyncAdapter));
    if (!adapter) {
        return -1;
    }
    adapter->host  = host;
    adapter->sched = sched;
    adapter->fn    = spec->execute;
    adapter->ud    = spec->user_data;

    AgentxxToolSpec s;
    memset(&s, 0, sizeof(s));
    s.name               = spec->name;
    s.description        = spec->description;
    s.parameters_json    = spec->parameters_json;
    s.execute_start      = &agentxx_sync_adapter_start;
    s.execute_poll       = NULL; ///< 只等完成通知
    s.execute_cancel     = &agentxx_sync_adapter_cancel;
    s.user_data          = adapter;
    s.default_timeout_ms = spec->default_timeout_ms;
    s.flags              = spec->flags;
    int rc               = tools->register_tool(host, &s);
    if (rc != 0) {
        free(adapter);
    }
    return rc;
}

/* =====================================================================
 * 内联完成型工具 (快同步; 宿主 io 线程直接执行)
 * ===================================================================== */

/// 快同步执行函数签名 (在【宿主 io 线程】直接调用, 必须 <~1ms 完成;
/// 无取消标志 —— 内联完成的操作不存在可取消窗口):
/// 返回结果 JSON (host->alloc); 失败返回 NULL 并设置 *error_out (host->alloc)
typedef char* (*AgentxxInlineToolFn)(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView thread_id,
    AgentxxPluginStringView tool_call_id,
    char**                  error_out
);

typedef struct AgentxxInlineToolSpec {
    AgentxxPluginStringView name;
    AgentxxPluginStringView description;
    AgentxxPluginStringView parameters_json;
    AgentxxInlineToolFn     execute;
    void*                   user_data;
    long                    default_timeout_ms; ///< 一般填 0 (快操作无超时意义)
    int                     flags;
} AgentxxInlineToolSpec;

/// 每工具适配器 (fn + ud + host; 注册时分配)
typedef struct AgentxxInlineAdapter {
    const AgentxxHost*  host;
    AgentxxInlineToolFn fn;
    void*               ud;
} AgentxxInlineAdapter;

static inline void* agentxx_inline_adapter_start(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView thread_id,
    AgentxxPluginStringView tool_call_id,
    const AgentxxOpNotify*  notify,
    char**                  error_out
) {
    AgentxxInlineAdapter* adapter = (AgentxxInlineAdapter*)user_data;
    char*                 result  = adapter->fn(adapter->ud, args_json, thread_id, tool_call_id, error_out);
    if (error_out && *error_out) {
        notify->done(notify->host_ud, AGENTXX_OP_FAILED, *error_out);
        return NULL;
    }
    if (!result) {
        /* 无错误串的 NULL 结果视为失败 (内联完成型无取消语义) */
        static const char kNullMsg[] = "inline tool returned null";
        char*             msg        = adapter->host->vtable->strdup(kNullMsg);
        notify->done(notify->host_ud, AGENTXX_OP_FAILED, msg);
        return NULL;
    }
    /* 结果已是 host->alloc 字符串, 所有权直接移交宿主 */
    notify->done(notify->host_ud, AGENTXX_OP_OK, result);
    return NULL; ///< 内联完成 (宿主不再 poll)
}

static inline int agentxx_register_inline_tool(const AgentxxHost* host, const AgentxxInlineToolSpec* spec) {
    if (!host || !spec || !spec->execute) {
        return -1;
    }
    const AgentxxToolsIface* tools
        = AGENTXX_QUERY_IFACE(host, AgentxxToolsIface, AGENTXX_IFACE_AGENT_TOOLS);
    if (!tools || !tools->register_tool) {
        return -1;
    }
    AgentxxInlineAdapter* adapter = (AgentxxInlineAdapter*)malloc(sizeof(AgentxxInlineAdapter));
    if (!adapter) {
        return -1;
    }
    adapter->host = host;
    adapter->fn   = spec->execute;
    adapter->ud   = spec->user_data;

    AgentxxToolSpec s;
    memset(&s, 0, sizeof(s));
    s.name               = spec->name;
    s.description        = spec->description;
    s.parameters_json    = spec->parameters_json;
    s.execute_start      = &agentxx_inline_adapter_start;
    s.execute_poll       = NULL;
    s.execute_cancel     = NULL;
    s.user_data          = adapter;
    s.default_timeout_ms = spec->default_timeout_ms;
    s.flags              = spec->flags;
    int rc               = tools->register_tool(host, &s);
    if (rc != 0) {
        free(adapter);
    }
    return rc;
}

/* =====================================================================
 * 快同步钩子垫片
 * ===================================================================== */

/// 快同步钩子处理函数 (宿主 io 线程直接调用, 必须快速返回):
/// 返回 0 成功; 非 0 失败并设置 *error_out (host->alloc)
typedef int (*AgentxxSyncHookFn)(
    void*                   user_data,
    AgentxxHookPoint        point,
    AgentxxPluginStringView node_input_json,
    char**                  error_out
);

/// 每钩子适配器
typedef struct AgentxxSyncHookAdapter {
    AgentxxSyncHookFn fn;
    void*             ud;
} AgentxxSyncHookAdapter;

static inline void* agentxx_sync_hook_adapter_start(
    void*                   user_data,
    AgentxxHookPoint        point,
    AgentxxPluginStringView node_input_json,
    const AgentxxOpNotify*  notify,
    char**                  error_out
) {
    AgentxxSyncHookAdapter* adapter = (AgentxxSyncHookAdapter*)user_data;
    int rc                          = adapter->fn(adapter->ud, point, node_input_json, error_out);
    if (error_out && *error_out) {
        notify->done(notify->host_ud, AGENTXX_OP_FAILED, *error_out);
        return NULL;
    }
    if (rc != 0) {
        notify->done(notify->host_ud, AGENTXX_OP_FAILED, NULL);
        return NULL;
    }
    notify->done(notify->host_ud, AGENTXX_OP_OK, NULL);
    return NULL; ///< 内联完成
}

/// 注册快同步钩子 (每插件每钩子点至多一个, 重复注册覆盖旧值)。返回 0 成功。
/// 注销: hooksIface->unregister_hook(host, point, user_data_of_registration)
/// 注: 垫片注册的钩子其注册 user_data 为内部适配器指针, 注销按 point 匹配即可
static inline int agentxx_register_sync_hook(
    const AgentxxHost* host,
    AgentxxHookPoint   point,
    AgentxxSyncHookFn  fn,
    void*              user_data
) {
    if (!host || !fn) {
        return -1;
    }
    const AgentxxHooksIface* hooks
        = AGENTXX_QUERY_IFACE(host, AgentxxHooksIface, AGENTXX_IFACE_AGENT_HOOKS);
    if (!hooks || !hooks->register_hook) {
        return -1;
    }
    AgentxxSyncHookAdapter* adapter = (AgentxxSyncHookAdapter*)malloc(sizeof(AgentxxSyncHookAdapter));
    if (!adapter) {
        return -1;
    }
    adapter->fn = fn;
    adapter->ud = user_data;

    AgentxxHookSpec s;
    memset(&s, 0, sizeof(s));
    s.point       = point;
    s.hook_start  = &agentxx_sync_hook_adapter_start;
    s.hook_poll   = NULL;
    s.hook_cancel = NULL;
    s.user_data   = adapter;
    int rc        = hooks->register_hook(host, &s);
    if (rc != 0) {
        free(adapter);
    }
    return rc;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AGENTXX_PLUGIN_TOOL_SYNC_H */
