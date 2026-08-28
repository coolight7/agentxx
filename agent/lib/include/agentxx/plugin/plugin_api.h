/*
 * agentxx/plugin/plugin_api.h —— 插件系统纯 C ABI 契约 (agent 侧; 唯一跨版本稳定接口)
 *
 * ════════════════════════════════════════════════════════════════════
 * 架构: COM 风格接口表查询 (v1 全量重构, 锚定协程模型)
 * ════════════════════════════════════════════════════════════════════
 * - 核心 vtable 极简且【契约冻结】: 仅 内存三件套 (alloc/free/strdup) +
 *   query_interface —— 一切宿主能力都按稳定 IID 字符串查询独立接口表获取
 *   (COM QueryInterface 风格), 核心永不再增删成员
 * - 每个接口表为纯 C 结构体, 首字段恒为 int version (该接口自身版本,
 *   独立演进, 与全局 api_version 解耦); 表内函数指针可能为 NULL
 *   (宿主未实现该子能力), 调用前必须判空; 查询未知名称返回 NULL (安全失败)
 * - 版本策略 (双层):
 *   1) 全局 AGENTXX_PLUGIN_API_VERSION 只覆盖核心契约 (核心 vtable 形状 +
 *      Info 结构 + 入口符号 + 本头共享类型); 宿主精确匹配门禁: api_version
 *      不匹配直接拒绝加载 (无历史兼容路径)
 *   2) 接口表各自携带 version 独立演进: 新增能力 = 定义新接口表或表内
 *      追加成员并递增该表版本, 全局版本号不动、其他插件不受影响
 *
 * 设计要点:
 * - 纯 C 头: 插件可用任意编译器/任意语言 (C/C++/Rust...) 实现, 与宿主
 *   STL/异常/RTTI ABI 完全解耦; 插件编译无需链接 libagentxx
 * - 跨 CRT 堆边界: 所有"宿主分配"的跨边界内存统一由宿主 alloc/free 管理
 *   (核心 vtable), 插件返回的字符串必须经 host->alloc 分配; 而"字符串参数/
 *   字段"一律以 AgentxxPluginStringView (data + size) 传入, 是只读借用
 *   (不要求 NUL 结尾, 不要求宿主分配)
 *
 * ════════════════════════════════════════════════════════════════════
 * 统一异步操作模型 (两件套 start/cancel + 锚定协程)
 * ════════════════════════════════════════════════════════════════════
 * - 工具执行 / 中间件钩子 / 能力方法 等"可能耗时的被调方操作"一律为
 *   两件套 start/cancel —— 宿主在 io 线程启动操作, 终结由 AgentxxOpNotify.done
 *   恰好一次上报; 配合 SDK (plugin_kit.h) 的 Task 协程, 挂起与恢复均经
 *   宿主在 io 线程派发的回调完成, 协程段物理执行于宿主 io 线程, 原生交错
 * - 被调方不需要任何复杂事件循环:
 *   * 内联完成型 (快同步, <~1ms): 在 start 内直接算完并 done(OK) 上报, 返回 NULL;
 *   * 锚定协程型 (推荐): 创建 Task 帧并 resume 到首挂起点, 返回 job 句柄;
 *   * 阻塞委托型: 经 scheduler.offload 委托宿主阻塞池;
 *   * 自管线程型: 登记工作到专用线程, 完成时任意线程调用 notify.done
 * - 反向调用 (插件调用宿主工具 call_tool_async / 其他插件能力 invoke_capability_async)
 *   提供完成回调形接口与 AgentxxOpHandle 句柄 (仅用于 cancel, 宿主托管生命周期)
 *
 * - 每插件一个 AgentxxHost (opaque 指向宿主侧插件实例): 注册/订阅自动关联
 *   到调用它的插件, 插件卸载时宿主自动清理其全部注册残留
 * - 接口表是进程级静态只读数据: entry 时查询一次缓存指针即可长期使用;
 *   同一进程内任意 host 句柄查到的同一 IID 表指针相同
 * - 线程约定:
 *   - query_interface 与 alloc/free/strdup 任意线程可调用;
 *   - 注册类 (tools/hooks/events/capabilities/resources) 与 session/plugins/
 *     config/prompt/scheduler 的 io 线程约束操作在非 io 线程调用时由宿主
 *     内部投递同步等待 (插件无感); 各接口表函数注释标注线程属性
 *   - 异步两件套 start/cancel 由宿主在【io 线程】驱动 (非阻塞快速
 *     返回约定: 单次调用不得超过 ~1ms);
 *   - AgentxxOpNotify.done 可从被调方任意线程回调 (线程安全)
 *   - 宿主派发给插件的完成回调 (AgentxxOpCb / sleep cb / offload done)
 *     保证在【宿主 io 线程】派发, 且一律经 asio::post 入队, 禁止同步重入
 * - 回调快速返回约定: 事件订阅回调在 io 线程同步调用, 必须快速返回
 * - 异常不外泄: 宿主接口表所有函数内部捕获全部异常 (C ABI 边界无异常);
 *   插件侧 start/cancel/event 回调同样不得让异常逃逸
 * - 字符串约定:
 *   - 所有跨边界"字符串参数/字段"类型为 AgentxxPluginStringView (data+size,
 *     不要求 NUL 结尾, 生命周期仅覆盖本次调用); 便捷构造见
 *     agentxx_plugin_sv / agentxx_plugin_sv_cstr / AGENTXX_SV
 *   - 所有"宿主分配"的字符串返回值 (工具结果 / error_out / payload /
 *     strdup/list_plugins/get_plugin/... ) 仍为 char* (NUL 结尾, host->alloc),
 *     调用方用完必须 host->free
 */
#ifndef AGENTXX_PLUGIN_API_H
#define AGENTXX_PLUGIN_API_H

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 插件导出符号控制 ==================== */

#if defined(AGENTXX_PLUGIN_BUILTIN)
#define AGENTXX_PLUGIN_EXPORT
#elif defined(_WIN32)
#define AGENTXX_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define AGENTXX_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define AGENTXX_PLUGIN_EXPORT
#endif

/// 全局 API 版本: 1 (锚定协程模型重构)
#define AGENTXX_PLUGIN_API_VERSION 1

/* ==================== 字符串视图 (跨边界字符串参数统一形态) ==================== */

/// 只读字符串视图: 指向调用方内存 (UTF-8), 不要求 NUL 结尾
typedef struct AgentxxPluginStringView {
    const char* data; ///< 指向 UTF-8 字节序列 (可含任意字节, 不必 NUL 结尾)
    size_t      size; ///< 字节数
} AgentxxPluginStringView;

static inline AgentxxPluginStringView agentxx_plugin_sv(const char* data, size_t size) {
    AgentxxPluginStringView sv;
    sv.data = data;
    sv.size = size;
    return sv;
}

static inline AgentxxPluginStringView agentxx_plugin_sv_cstr(const char* s) {
    AgentxxPluginStringView sv;
    sv.data = s;
    sv.size = s ? strlen(s) : 0;
    return sv;
}

static inline int agentxx_plugin_sv_empty(AgentxxPluginStringView sv) {
    return sv.data == NULL || sv.size == 0;
}

#define AGENTXX_SV(s) agentxx_plugin_sv_cstr((s))

/* ==================== 插件元信息 ==================== */

typedef struct AgentxxPluginInfo {
    int                     api_version; ///< 必须 == AGENTXX_PLUGIN_API_VERSION
    AgentxxPluginStringView name;        ///< 唯一标识 (只读借用)
    AgentxxPluginStringView version;
    AgentxxPluginStringView description;
} AgentxxPluginInfo;

/* ==================== 统一异步操作原语 (两件套) ==================== */

/// 操作终结状态 (AgentxxOpNotify.done 的 status 参数)
#define AGENTXX_OP_OK        0 ///< 成功 (payload = 结果数据, host->alloc)
#define AGENTXX_OP_CANCELLED 1 ///< 已取消 (payload 可为 NULL)
#define AGENTXX_OP_FAILED    2 ///< 失败 (payload = 错误信息, host->alloc)

/// 完成通知器 (宿主实现并随 start 下发; 操作终结时被调方须【恰好回调一次】)
/// - payload: host->alloc 分配的字符串, 所有权移交宿主 (可为 NULL)
/// - 线程安全: 可从被调方的任意线程回调, 宿主内部投递回 io 线程唤醒等待协程
typedef struct AgentxxOpNotify {
    void (*done)(void* host_ud, int status, char* payload);
    void* host_ud;
} AgentxxOpNotify;

/// 完成回调 (统一形态; 宿主保证在宿主 io 线程派发)
/// payload host->alloc 分配, 所有权归回调接收方 (须 host->free)
typedef void (*AgentxxOpCb)(void* ud, int status, char* payload);

/// 异步调用句柄 (仅用于取消; 不可轮询/收尸; 宿主托管生命周期)
typedef struct AgentxxOpHandle AgentxxOpHandle;

/// 协作式取消请求函数 (【宿主 io 线程调用】, 非阻塞):
/// - 被调方应尽快收尾并经 notifier 上报 CANCELLED; 也允许继续完成并上报 OK/FAILED
/// - 不可取消的操作可留 NULL
typedef void (*AgentxxOpCancelFn)(void* user_data, void* op);

/* ==================== 工具定义 ==================== */

#define AGENTXX_TOOL_FLAG_NONE         0
#define AGENTXX_TOOL_FLAG_AUTO_SUMMARY (1 << 0) ///< 输出超限时自动压缩 (经 share_store 卸载)

typedef struct AgentxxToolSpec {
    AgentxxPluginStringView name; ///< 须全局唯一 (与内置工具/MCP 工具同名将注册失败)
    AgentxxPluginStringView description;
    AgentxxPluginStringView parameters_json; ///< JSON Schema 字符串 (json object)

    /// 启动执行 (【宿主 io 线程调用】, 非阻塞; 两件套契约):
    /// - args_json/thread_id/tool_call_id: 只读借用, 仅本次调用有效
    /// - 快同步工具: 算完 → notify->done(AGENTXX_OP_OK, 结果 json) → 返回 NULL
    /// - 锚定协程/自管异步: 创建/挂起任务 → 返回 op 句柄
    /// - 失败: 返回 NULL 且 *error_out 输出错误 (host->alloc 分配)
    void* (*execute_start)(
        void*                   user_data,
        AgentxxPluginStringView args_json,
        AgentxxPluginStringView thread_id,
        AgentxxPluginStringView tool_call_id,
        const AgentxxOpNotify*  notify,
        char**                  error_out
    );
    /// 协作式取消请求 (io 线程, 非阻塞; 不可取消可留 NULL)
    void (*execute_cancel)(void* user_data, void* op);

    void* user_data;
    long  default_timeout_ms; ///< 0 = 不限制 (宿主按调用方取消语义执行)
    int   flags;              ///< AGENTXX_TOOL_FLAG_*
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

/// 钩子规格: 两件套形态
typedef struct AgentxxHookSpec {
    AgentxxHookPoint point;
    void* (*hook_start)(
        void*                   user_data,
        AgentxxHookPoint        point,
        AgentxxPluginStringView node_input_json,
        const AgentxxOpNotify*  notify,
        char**                  error_out
    );
    void (*hook_cancel)(void* user_data, void* op); ///< 可为 NULL
    void* user_data;
} AgentxxHookSpec;

/* ==================== 事件订阅句柄 / 前向声明 ==================== */

typedef struct AgentxxSubscription AgentxxSubscription;
struct AgentxxHost;

/// 能力方法处理器启动函数 (两件套契约):
typedef void* (*AgentxxCapStartFn)(
    void*                   ctx,
    const AgentxxHost*      caller_host,
    AgentxxPluginStringView method,
    AgentxxPluginStringView args_json,
    const AgentxxOpNotify*  notify,
    char**                  error_out
);

/* ==================== 核心宿主函数表 (契约冻结) ==================== */

typedef struct AgentxxHost AgentxxHost;

/// 核心 vtable: 仅内存三件套 + COM 风格接口表查询。【契约冻结】
typedef struct AgentxxHostVtable {
    /* ---- 内存 (跨 CRT 堆边界的唯一分配通道; 任意线程可调用) ---- */
    void* (*alloc)(size_t size);
    void (*free)(void* ptr);
    char* (*strdup)(const char* s);

    /* ---- COM 风格接口表查询 (QueryInterface; 任意线程可调用) ---- */
    const void* (*query_interface)(const AgentxxHost* host, AgentxxPluginStringView iid);
} AgentxxHostVtable;

struct AgentxxHost {
    const AgentxxHostVtable* vtable; ///< 核心函数表 (宿主静态)
    void* opaque;                    ///< 宿主内部 (指向插件实例状态, 插件不得使用)
};

#define AGENTXX_QUERY_IFACE(host, IfaceType, iid_name)                                        \
    ((const IfaceType*)(host)->vtable->query_interface((host), AGENTXX_SV(iid_name)))

/* ==================== 接口表: 工具 (agentxx.agent.tools) ==================== */

#define AGENTXX_IFACE_AGENT_TOOLS         "agentxx.agent.tools"
#define AGENTXX_IFACE_AGENT_TOOLS_VERSION 1

typedef struct AgentxxToolsIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_TOOLS_VERSION

    /// 注册工具 (io 线程约束, 非 io 线程由宿主投递同步等待); 名称冲突返回非 0
    int (*register_tool)(const AgentxxHost* host, const AgentxxToolSpec* spec);
    /// 注销工具 (按名称); 不存在返回非 0
    int (*unregister_tool)(const AgentxxHost* host, AgentxxPluginStringView name);

    /* ---- 插件互调: 完成回调形 (推荐) ---- */
    /// 异步调用插件工具 (宿主保证 cb 在宿主 io 线程派发, 一律 post 入队):
    /// - 返回的 AgentxxOpHandle 仅用于 op_cancel; 宿主自动管理句柄生命周期
    /// - 查表/装配失败返回 NULL 并 error_out
    AgentxxOpHandle* (*call_tool_async)(
        const AgentxxHost*      host,
        AgentxxPluginStringView name,
        AgentxxPluginStringView args_json,
        AgentxxPluginStringView thread_id,
        AgentxxOpCb             cb,
        void*                   ud,
        char**                  error_out
    );
    /// 取消异步操作 (幂等, 任意线程可调; 完成/退休后调用无害)
    void (*op_cancel)(AgentxxOpHandle* op);
} AgentxxToolsIface;

/* ==================== 接口表: 中间件钩子 (agentxx.agent.hooks) ==================== */

#define AGENTXX_IFACE_AGENT_HOOKS         "agentxx.agent.hooks"
#define AGENTXX_IFACE_AGENT_HOOKS_VERSION 1

typedef struct AgentxxHooksIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_HOOKS_VERSION

    /// 注册钩子 (两件套规格; 热插拔, 轮次边界生效; io 线程约束)
    int (*register_hook)(const AgentxxHost* host, const AgentxxHookSpec* spec);
    /// 注销钩子 (按 point 匹配); 不存在返回非 0
    int (*unregister_hook)(const AgentxxHost* host, AgentxxHookPoint point);
} AgentxxHooksIface;

/* ==================== 接口表: 事件 (agentxx.agent.events) ==================== */

#define AGENTXX_IFACE_AGENT_EVENTS         "agentxx.agent.events"
#define AGENTXX_IFACE_AGENT_EVENTS_VERSION 1

typedef struct AgentxxEventsIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_EVENTS_VERSION

    /// 订阅 (topic 自动加 "plugin." 前缀, 载荷为 JSON 字符串); 返回句柄
    AgentxxSubscription* (*subscribe)(
        const AgentxxHost* host,
        AgentxxPluginStringView topic,
        void (*handler)(AgentxxPluginStringView event_json, void* ud),
        void*                   ud
    );
    void (*unsubscribe)(AgentxxSubscription* sub);
    /// 发布 (异步投递, 立即返回; 禁用状态的插件被拒绝)
    int (*publish)(
        const AgentxxHost*      host,
        AgentxxPluginStringView topic,
        AgentxxPluginStringView event_json
    );
} AgentxxEventsIface;

/* ==================== 接口表: 能力 (agentxx.agent.capabilities) ==================== */

#define AGENTXX_IFACE_AGENT_CAPABILITIES         "agentxx.agent.capabilities"
#define AGENTXX_IFACE_AGENT_CAPABILITIES_VERSION 1

typedef struct AgentxxCapabilitiesIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_CAPABILITIES_VERSION

    /// 声明能力 (无方法处理器; 仅标记/互查; io 线程约束)
    int (*register_capability)(const AgentxxHost* host, AgentxxPluginStringView capability);
    /// 注册能力并附带两件套方法处理器 (通用插件间通信通道):
    int (*register_capability_ex)(
        const AgentxxHost*      host,
        AgentxxPluginStringView capability,
        AgentxxCapStartFn       start,
        AgentxxOpCancelFn       cancel,
        void*                   ctx
    );
    int (*unregister_capability)(const AgentxxHost* host, AgentxxPluginStringView capability);
    /// 是否存在指定能力 (io 线程查表)
    int (*has_capability)(const AgentxxHost* host, AgentxxPluginStringView capability);

    /* ---- 能力调用: 完成回调形 ---- */
    /// 异步调用能力提供者的方法: 宿主保证 cb 在 io 线程派发
    AgentxxOpHandle* (*invoke_capability_async)(
        const AgentxxHost*      host,
        AgentxxPluginStringView capability,
        AgentxxPluginStringView method,
        AgentxxPluginStringView args_json,
        AgentxxOpCb             cb,
        void*                   ud,
        char**                  error_out
    );
    /// 取消异步操作 (幂等, 任意线程可调)
    void (*op_cancel)(AgentxxOpHandle* op);
} AgentxxCapabilitiesIface;

/* ==================== 接口表: 任务调度 (agentxx.agent.scheduler) ==================== */

#define AGENTXX_IFACE_AGENT_SCHEDULER         "agentxx.agent.scheduler"
#define AGENTXX_IFACE_AGENT_SCHEDULER_VERSION 1

typedef struct AgentxxSchedulerIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_SCHEDULER_VERSION

    /// 当前线程是否为宿主 io 线程 (任意线程可调用)
    int (*is_io_thread)(const AgentxxHost* host);
    /// 投递任务到宿主 io 线程异步执行 (不等待, 线程安全)
    void (*post_to_io)(const AgentxxHost* host, void (*fn)(void* ud), void* ud);
    /// 推进并执行当前待处理的 io 任务 (仅 io 线程调用; 阻塞等待场景避免死锁)
    void (*pump_io)(const AgentxxHost* host);
    /// 一次性定时器 (宿主保证在 io 线程派发, 经 asio::post 入队)
    /// 返回 timer 句柄 (宿主所有); 插件卸载时自动取消
    void* (*sleep)(const AgentxxHost* host, long ms, void (*cb)(void* ud), void* ud);
    /// 取消定时器 (触发提前唤醒回调或取消)
    void (*cancel_sleep)(const AgentxxHost* host, void* timer);

    /* ---- 阻塞池委托 ---- */
    /// 在宿主阻塞线程池执行同步回调 (阻塞操作专用: 文件遍历/HTTP/子进程等)
    /// - cancel_flag: 调用方持有的取消标志 (volatile int; 0=未取消 1=已取消)
    /// - work: 在阻塞池线程执行; 返回结果与 error_out 须 host->alloc 分配
    /// - done: 宿主保证在 io 线程派发 (经 asio::post 入队)
    void (*offload)(
        const AgentxxHost* host,
        volatile int*      cancel_flag,
        void* (*work)(void* ud, volatile int* cancel_flag, char** error_out),
        void (*done)(void* ud, void* result, char* error),
        void*                   ud
    );
} AgentxxSchedulerIface;

/* ==================== 接口表: 会话访问 (agentxx.agent.session) ==================== */

#define AGENTXX_IFACE_AGENT_SESSION         "agentxx.agent.session"
#define AGENTXX_IFACE_AGENT_SESSION_VERSION 1

typedef struct AgentxxSessionIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_SESSION_VERSION

    /// 读取会话级 share_store 条目 (仅 io 线程); 不存在返回 NULL (host->alloc)
    char* (
        *get_share_store)(const AgentxxHost* host, AgentxxPluginStringView thread_id, long long id);
    /// 向会话 UI 推送提示消息 (仅 io 线程); level: 0=info 1=warning 2=error
    void (*emit_message_tip)(
        const AgentxxHost*      host,
        AgentxxPluginStringView thread_id,
        AgentxxPluginStringView text,
        int                     level
    );
    /// 写入会话级 share_store 条目并返回 ID (仅 io 线程); 失败返回 -1
    /// - content 为待存储的完整文本 (host 侧按行切片等处理与工具侧一致)
    long long (*add_share_store)(
        const AgentxxHost*      host,
        AgentxxPluginStringView thread_id,
        AgentxxPluginStringView content
    );
} AgentxxSessionIface;

/* ==================== 接口表: 插件互查 (agentxx.agent.plugins) ==================== */

#define AGENTXX_IFACE_AGENT_PLUGINS         "agentxx.agent.plugins"
#define AGENTXX_IFACE_AGENT_PLUGINS_VERSION 1

typedef struct AgentxxPluginsIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_PLUGINS_VERSION

    /// 全部已安装插件信息 JSON 数组 (host->alloc 分配)
    char* (*list_plugins)(const AgentxxHost* host);
    /// 单个插件信息 JSON (未安装返回 NULL, host->alloc)
    char* (*get_plugin)(const AgentxxHost* host, AgentxxPluginStringView name);
    /// 调用方插件自身信息 JSON {"name","version","description","path",...} (host->alloc)
    char* (*get_own_info)(const AgentxxHost* host);
} AgentxxPluginsIface;

/* ==================== 接口表: 宿主配置 (agentxx.agent.config) ==================== */

#define AGENTXX_IFACE_AGENT_CONFIG         "agentxx.agent.config"
#define AGENTXX_IFACE_AGENT_CONFIG_VERSION 1

typedef struct AgentxxConfigIface {
    int version; ///< 必须 >= AGENTXX_IFACE_AGENT_CONFIG_VERSION

    /// 宿主 AgentConfig 关键字段 JSON (io 线程; host->alloc):
    /// {"dataDir": "...", "projectRoot": "..."(可为空), "platform": "windows"|"linux"|"macos"}
    char* (*get_config)(const AgentxxHost* host);
    /// 本插件配置参数 JSON (yaml `plugins` 条目 args; io 线程; host->alloc):
    char* (*get_plugin_args)(const AgentxxHost* host);
    /// 宿主 toolPrompt 配置 (io 线程; host->alloc):
    /// {"depict": "...", "args": {"参数名": "参数说明", ...}}
    char* (*get_tool_prompt)(const AgentxxHost* host, AgentxxPluginStringView tool_name);
    /// 解析后的会话工作目录 (io 线程; host->alloc; 失败/未装配返回 NULL)
    char* (*get_work_dir)(const AgentxxHost* host);
    /// 指定会话生效的工作目录 (worktree 绑定优先, 回退 get_work_dir; io 线程; host->alloc)
    char* (*get_session_work_dir)(const AgentxxHost* host, AgentxxPluginStringView thread_id);
} AgentxxConfigIface;

/* ==================== 接口表: 主模型配置 (agentxx.agent.model) ==================== */

#define AGENTXX_IFACE_AGENT_MODEL         "agentxx.agent.model"
#define AGENTXX_IFACE_AGENT_MODEL_VERSION 1

typedef struct AgentxxModelIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_MODEL_VERSION

    /// 宿主主模型及关联配置 JSON (io 线程; host->alloc; 未装配返回 NULL):
    char* (*get_config)(const AgentxxHost* host);
} AgentxxModelIface;

/* ==================== 接口表: 会话取消状态 (agentxx.agent.cancel) ==================== */

#define AGENTXX_IFACE_AGENT_CANCEL         "agentxx.agent.cancel"
#define AGENTXX_IFACE_AGENT_CANCEL_VERSION 1

typedef struct AgentxxCancelIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_CANCEL_VERSION

    /// 查询会话当前轮次是否已取消 (advisory 定位; 权威通知始终是 cancel 回调)
    int (*is_cancelled)(const AgentxxHost* host, AgentxxPluginStringView thread_id);
} AgentxxCancelIface;

/* ==================== 接口表: 任务规划 (agentxx.agent.planning) ==================== */

#define AGENTXX_IFACE_AGENT_PLANNING         "agentxx.agent.planning"
#define AGENTXX_IFACE_AGENT_PLANNING_VERSION 1

typedef struct AgentxxPlanningIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_PLANNING_VERSION

    int (*set_planning)(
        const AgentxxHost*      host,
        AgentxxPluginStringView thread_id,
        AgentxxPluginStringView roadmap,
        AgentxxPluginStringView todos_json,
        AgentxxPluginStringView notes
    );
} AgentxxPlanningIface;

/* ==================== 接口表: 宿主提示词读写 (agentxx.agent.prompt) ==================== */

#define AGENTXX_IFACE_AGENT_PROMPT         "agentxx.agent.prompt"
#define AGENTXX_IFACE_AGENT_PROMPT_VERSION 1

typedef struct AgentxxPromptIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_PROMPT_VERSION

    char* (*get_prompt)(const AgentxxHost* host);
    int (*set_prompt)(const AgentxxHost* host, AgentxxPluginStringView prompt_json);
} AgentxxPromptIface;

/* ==================== 接口表: JSON 辅助 (agentxx.agent.json) ==================== */

#define AGENTXX_IFACE_AGENT_JSON         "agentxx.agent.json"
#define AGENTXX_IFACE_AGENT_JSON_VERSION 1

typedef struct AgentxxJsonIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_JSON_VERSION

    char* (*json_get_string)(
        const AgentxxHost*      host,
        AgentxxPluginStringView json,
        AgentxxPluginStringView key
    );
    char* (*json_escape)(const AgentxxHost* host, AgentxxPluginStringView s);
} AgentxxJsonIface;

/* ==================== 接口表: 日志 (agentxx.agent.log) ==================== */

#define AGENTXX_IFACE_AGENT_LOG         "agentxx.agent.log"
#define AGENTXX_IFACE_AGENT_LOG_VERSION 1

typedef struct AgentxxLogIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_LOG_VERSION

    void (*log)(const AgentxxHost* host, int level, AgentxxPluginStringView msg);
} AgentxxLogIface;

/* ==================== 接口表: 会话资源贡献 (agentxx.agent.resources) ==================== */

#define AGENTXX_IFACE_AGENT_RESOURCES         "agentxx.agent.resources"
#define AGENTXX_IFACE_AGENT_RESOURCES_VERSION 1

typedef struct AgentxxResourcesIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_RESOURCES_VERSION

    int (*register_skill_dir)(const AgentxxHost* host, AgentxxPluginStringView path);
    int (*unregister_skill_dir)(const AgentxxHost* host, AgentxxPluginStringView path);
    int (*register_memory_file)(const AgentxxHost* host, AgentxxPluginStringView path);
    int (*unregister_memory_file)(const AgentxxHost* host, AgentxxPluginStringView path);
    int (*register_mcp_server)(const AgentxxHost* host, AgentxxPluginStringView spec_json);
    int (*unregister_mcp_server)(const AgentxxHost* host, AgentxxPluginStringView name_space);
    char* (*get_own_resources)(const AgentxxHost* host);
} AgentxxResourcesIface;

/* ==================== 插件入口符号 (dlsym) ==================== */

typedef const AgentxxPluginInfo* (*AgentxxPluginGetInfoFn)(void);
typedef int (*AgentxxPluginCreateFn)(const AgentxxHost* host, void** plugin_ctx);
typedef void (*AgentxxPluginDestroyFn)(void* plugin_ctx);

#define AGENTXX_PLUGIN_SYMBOL_GET_INFO  "agentxx_plugin_get_info"
#define AGENTXX_PLUGIN_SYMBOL_CREATE    "agentxx_plugin_create"
#define AGENTXX_PLUGIN_SYMBOL_DESTROY   "agentxx_plugin_destroy"

/* ==================== 便捷宏 (插件侧使用) ==================== */

#define AGENTXX_STRDUP(host, s) ((host)->vtable->strdup((s)))

#ifdef __cplusplus
}
#endif

#endif /* AGENTXX_PLUGIN_API_H */
