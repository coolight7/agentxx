/*
 * agentxx/plugin/plugin_api.h —— 插件系统纯 C ABI 契约 (唯一跨版本稳定接口)
 *
 * 设计要点:
 * - 纯 C 头: 插件可用任意编译器/任意语言 (C/C++/Rust...) 实现, 与宿主
 *   STL/异常/RTTI ABI 完全解耦; 插件编译无需链接 libagentxx
 * - 跨 CRT 堆边界: 所有跨边界内存统一由宿主分配/释放 (见 AgentxxHostVtable
 *   alloc/free/strdup), 插件返回的字符串必须经 host->alloc 分配
 * - 每插件一个 AgentxxHost (opaque 指向宿主侧插件实例): 注册/订阅自动关联
 *   到调用它的插件, 插件卸载时宿主自动清理其全部注册残留
 * - 线程约定:
 *   - entry/register/unregister/subscribe/unsubscribe/publish/emit_message_tip/
 *     get_share_store 必须在宿主 io 线程调用 (插件入口与钩子回调即在此线程)
 *   - execute 回调运行在宿主线程池, 内仅可调用 call_tool / log (线程安全);
 *     其余 API 需经宿主 post 到 io 线程 (二期提供 async 桥)
 * - 回调快速返回约定: 事件订阅回调与钩子回调在 io 线程同步调用, 必须快速
 *   返回, 不得阻塞 (长时间任务请经 call_tool 或自行投递到独立线程)
 *
 * 版本策略: 修改本契约时递增 AGENTXX_PLUGIN_API_VERSION; 宿主拒绝
 * api_version 不匹配的插件 (仅拒绝, 不崩溃)
 */
#ifndef AGENTXX_PLUGIN_API_H
#define AGENTXX_PLUGIN_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AGENTXX_PLUGIN_API_VERSION 1

/* ==================== 插件元信息 ==================== */

typedef struct AgentxxPluginInfo {
    int         api_version;  ///< 必须 == AGENTXX_PLUGIN_API_VERSION
    const char* name;         ///< 唯一标识, 如 "agentxx_plugin_js" (宿主内静态字符串, 无需释放)
    const char* version;
    const char* description;
} AgentxxPluginInfo;

/* ==================== 工具定义 ==================== */

#define AGENTXX_TOOL_FLAG_NONE          0
#define AGENTXX_TOOL_FLAG_AUTO_SUMMARY  (1 << 0) ///< 输出超限时自动压缩 (经 share_store 卸载)

typedef struct AgentxxToolSpec {
    const char* name;             ///< 须全局唯一 (与内置工具/MCP 工具同名将注册失败)
    const char* description;
    const char* parameters_json;  ///< JSON Schema 字符串 (json object)
    /// 同步执行回调 (宿主线程池线程):
    /// - args_json: 参数对象 JSON (含宿主注入的 thread_id / tool_call_id)
    /// - 返回: 结果 JSON 字符串, 必须用 host->alloc 分配 (宿主 free);
    ///   失败时返回 NULL 并经 error_out 输出错误信息 (同样 host->alloc)
    /// - 回调内可调用 call_tool / log; 不得阻塞宿主 io 线程
    char* (*execute)(void* user_data, const char* args_json, const char* thread_id,
                     const char* tool_call_id, char** error_out);
    void* user_data;
    long  default_timeout_ms;     ///< 0 = 不限制 (宿主按调用方取消语义执行)
    int   flags;                  ///< AGENTXX_TOOL_FLAG_*
} AgentxxToolSpec;

/* ==================== 中间件钩子 ==================== */

/// 钩子点 (与宿主 7 个中间件钩子一一对应)
typedef enum AgentxxHookPoint {
    AGENTXX_HOOK_AGENT_START = 0, ///< 会话轮次开始
    AGENTXX_HOOK_AGENT_END,       ///< 会话轮次结束
    AGENTXX_HOOK_MODEL_START,     ///< LLM 调用开始
    AGENTXX_HOOK_MODEL_RUN,       ///< LLM 调用执行 (重试时多次触发)
    AGENTXX_HOOK_MODEL_END,       ///< LLM 调用结束
    AGENTXX_HOOK_TOOL_START,      ///< 工具分发开始
    AGENTXX_HOOK_TOOL_END,        ///< 工具分发结束
    AGENTXX_HOOK_COUNT
} AgentxxHookPoint;

/// 钩子回调 (io 线程同步调用, 必须快速返回):
/// - node_input_json: 节点输入摘要 ({"thread_id", "node", "messages_count", ...})
/// - out_json: 预留, 一期恒为 NULL (回调不得写入)
/// - 返回 0 成功; 非 0 失败并经 error_out 输出错误 (host->alloc 分配)
typedef int (*AgentxxHookFn)(void* user_data, AgentxxHookPoint point,
                             const char* node_input_json, char** out_json,
                             char** error_out);

/* ==================== 事件订阅 ==================== */

typedef struct AgentxxSubscription AgentxxSubscription;

/* ==================== 宿主函数表 ==================== */

typedef struct AgentxxHost AgentxxHost;

typedef struct AgentxxHostVtable {
    /* ---- 内存 (跨 CRT 堆边界的唯一分配通道) ---- */
    void* (*alloc)(size_t size);
    void  (*free)(void* ptr);
    char* (*strdup)(const char* s);

    /* ---- 工具注册 (热插拔) ---- */
    /// 注册工具; 名称冲突返回非 0
    int (*register_tool)(const AgentxxHost* host, const AgentxxToolSpec* spec);
    /// 注销工具 (按名称); 不存在返回非 0
    int (*unregister_tool)(const AgentxxHost* host, const char* name);

    /* ---- 中间件钩子 (热插拔, 轮次边界生效) ---- */
    int (*register_hook)(const AgentxxHost* host, AgentxxHookPoint point,
                         AgentxxHookFn fn, void* user_data);
    int (*unregister_hook)(const AgentxxHost* host, AgentxxHookPoint point,
                           AgentxxHookFn fn, void* user_data);

    /* ---- 事件 (topic 自动加 "plugin." 前缀, 载荷为 JSON 字符串) ---- */
    /// 订阅; 返回句柄 (宿主侧持有, 插件卸载时自动退订)
    AgentxxSubscription* (*subscribe)(const AgentxxHost* host, const char* topic,
                                      void (*handler)(const char* event_json, void* ud),
                                      void* ud);
    void (*unsubscribe)(AgentxxSubscription* sub);
    /// 发布 (异步投递, 立即返回)
    int (*publish)(const AgentxxHost* host, const char* topic, const char* event_json);

    /* ---- 能力注册表 (插件互查/委派, 如 "interpreter.js") ---- */
    int (*register_capability)(const AgentxxHost* host, const char* capability);
    int (*unregister_capability)(const AgentxxHost* host, const char* capability);
    int (*has_capability)(const AgentxxHost* host, const char* capability);

    /* ---- 会话/上下文访问 ---- */
    /// 调用插件工具 (仅限插件注册的工具, 不暴露宿主内置工具; 在调用方线程
    /// 同步执行 —— 工具回调(线程池)内可安全调用, 宿主 io 线程内调用会阻塞 io 线程)
    /// 返回结果 JSON 字符串 (host->alloc); 失败返回 NULL 并经 error_out 输出
    char* (*call_tool)(const AgentxxHost* host, const char* name, const char* args_json,
                       const char* thread_id, char** error_out);
    /// 读取会话级 share_store 条目 (仅 io 线程); 不存在返回 NULL
    char* (*get_share_store)(const AgentxxHost* host, const char* thread_id, long long id);
    /// 向会话 UI 推送提示消息 (仅 io 线程); level: 0=info 1=warning 2=error
    void (*emit_message_tip)(const AgentxxHost* host, const char* thread_id,
                             const char* text, int level);
    /// 日志 (线程安全); level 与宿主 XX_LOG 级别对应 (0=trace 1=debug 2=info 3=warn 4=error)
    void (*log)(const AgentxxHost* host, int level, const char* msg);
} AgentxxHostVtable;

struct AgentxxHost {
    const AgentxxHostVtable* vtable; ///< 函数表 (宿主静态)
    void* opaque;                    ///< 宿主内部 (指向插件实例状态, 插件不得使用)
};

/* ==================== 插件入口符号 (dlsym) ==================== */

/// 可选: 查询插件元信息 (加载前调用, 用于版本/信息校验; 未导出则跳过)
typedef const AgentxxPluginInfo* (*AgentxxPluginGetInfoFn)(void);

/// 必需: 插件入口 (宿主 io 线程调用)
/// - host: 本插件专属宿主句柄 (opaque 已关联本插件)
/// - plugin_ctx: 输出插件私有上下文 (透传给 unload)
/// - 返回 0 成功; 非 0 加载失败 (宿主 dlclose 并报告错误)
typedef int (*AgentxxPluginEntryFn)(const AgentxxHost* host, void** plugin_ctx);

/// 可选: 插件卸载通知 (宿主等全部在途回调完成后调用; 用于插件业务清理;
/// 宿主会在此之前自动反注册该插件的一切工具/钩子/订阅/能力)
typedef void (*AgentxxPluginUnloadFn)(void* plugin_ctx);

#define AGENTXX_PLUGIN_SYMBOL_GET_INFO "agentxx_plugin_get_info"
#define AGENTXX_PLUGIN_SYMBOL_ENTRY    "agentxx_plugin_entry"
#define AGENTXX_PLUGIN_SYMBOL_UNLOAD   "agentxx_plugin_unload"

/* ==================== 便捷宏 (插件侧使用) ==================== */

/// 在插件侧分配跨边界字符串 (必须用它, 不能直接用 malloc/strdup)
#define AGENTXX_STRDUP(host, s) ((host)->vtable->strdup((host), (s)))

#ifdef __cplusplus
}
#endif

#endif /* AGENTXX_PLUGIN_API_H */
