///
/// agentxx/plugin/api/plugin_api.h —— 插件系统纯 C ABI 契约 (agent 侧; 唯一跨版本稳定接口)
///
/// ════════════════════════════════════════════════════════════════════
/// 架构: COM 风格接口表查询
/// ════════════════════════════════════════════════════════════════════
/// - 核心 vtable [冻结]: 仅 内存三件套 (alloc/free/strdup) +
///   query_interface —— 一切宿主能力都按稳定 IID 字符串查询独立接口表获取
///   (COM QueryInterface 风格), 核心永不再增删成员
/// - 每个接口表为纯 C 结构体, 首字段恒为 int version (该接口自身版本,
///   独立演进, 与全局 api_version 解耦); 表内函数指针可能为 NULL
///   (宿主未实现该子能力), 调用前必须判空; 查询未知名称返回 NULL (安全失败)
/// - 版本策略 (双层):
///   1) 全局 AGENTXX_PLUGIN_API_VERSION 只覆盖核心契约 (核心 vtable 形状 +
///      Info 结构 + 入口符号 + 本头共享类型); 宿主精确匹配门禁: api_version
///      不匹配直接拒绝加载
///   2) 接口表各自携带 version 独立演进: 新增能力 = 定义新接口表或表内
///      追加成员并递增该表版本, 全局版本号不动、其他插件不受影响
///
/// 设计要点:
/// - 纯 C 头: 插件可用任意编译器/任意语言 (C/C++/Rust...) 实现, 与宿主
///   STL/异常/RTTI ABI 完全解耦; 插件编译无需链接 libagentxx
/// - 跨 CRT 堆边界: 所有"宿主分配"的跨边界内存统一由宿主 alloc/free 管理
///   (核心 vtable); 而"字符串参数/字段/回调载荷"一律以 AgentxxPluginStringView (data + size)
///   传入, 是只读借用 (不要求 NUL 结尾, 不要求宿主分配)
///
/// ════════════════════════════════════════════════════════════════════
/// 统一异步操作模型 (两件套 start/cancel + 锚定协程)
/// ════════════════════════════════════════════════════════════════════
/// - 工具执行 / 中间件钩子 / 能力方法 等"可能耗时的被调方操作"一律为
///   两件套 start/cancel —— 宿主在 io 线程启动操作, 终结由 AgentxxPluginOperatorNotify.done
///   恰好一次上报; 配合 SDK (plugin_kit.h) 的 Task 协程, 挂起与恢复均经
///   宿主在 io 线程派发的回调完成, 协程段物理执行于宿主 io 线程, 原生交错
/// - 被调方不需要任何复杂事件循环:
///   * 内联完成型 (快同步, <~1ms): 在 start 内直接算完并 done(OK) 上报, 返回 NULL;
///   * 锚定协程型 (推荐): 创建 Task 帧并 resume 到首挂起点, 返回 job 句柄;
///   * 阻塞委托型: 经 scheduler.offload 委托宿主阻塞池;
///   * 自管线程型: 登记工作到专用线程, 完成时任意线程调用 notify.done
/// - 反向调用 (插件调用宿主工具 call_tool_async / 其他插件能力 invoke_capability_async)
///   提供完成回调形接口与 AgentxxPluginOperatorHandle 句柄 (仅用于 cancel, 宿主托管生命周期)
///
/// - 每插件一个 AgentxxPluginHost (opaque 指向宿主侧插件实例): 注册/订阅自动关联
///   到调用它的插件, 插件卸载时宿主自动清理其全部注册残留
/// - 接口表是进程级静态只读数据: entry 时查询一次缓存指针即可长期使用;
///   同一进程内任意 host 句柄查到的同一 IID 表指针相同
/// - 线程约定:
///   - query_interface 与 alloc/free/strdup 任意线程可调用;
///   - 注册类 (tools/hooks/events/capabilities/resources) 与 session/plugins/
///     config/prompt/scheduler 的 io 线程约束操作在非 io 线程调用时由宿主
///     内部投递同步等待 (插件无感); 各接口表函数注释标注线程属性
///   - 异步两件套 start/cancel 由宿主在【io 线程】驱动 (非阻塞快速
///     返回约定: 单次调用不得超过 ~1ms);
///   - AgentxxPluginOperatorNotify.done 可从被调方任意线程回调 (线程安全)
///   - 宿主派发给插件的完成回调 (AgentxxPluginOperatorCallback / sleep cb / offload done)
///     保证在【宿主 io 线程】派发, 且一律经 asio::post 入队, 禁止同步重入
/// - 回调快速返回约定: 事件订阅回调在 io 线程同步调用, 必须快速返回
/// - 异常不外泄: 宿主接口表所有函数内部捕获全部异常 (C ABI 边界无异常);
///   插件侧 start/cancel/event 回调同样不得让异常逃逸
/// - 字符串约定:
///   - 所有跨边界"字符串参数/字段/回调载荷"类型为 AgentxxPluginStringView (data+size,
///     不要求 NUL 结尾, 生命周期仅覆盖本次调用); 便捷构造见
///     agentxx_plugin_sv / agentxx_plugin_sv_cstr
///   - 所有"宿主分配"的字符串返回值 (strdup/list_plugins/get_plugin/... ) 仍为 char*
///     (NUL 结尾, host->alloc), 调用方用完必须 host->free
///
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

/// 全局 API 版本
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

/// ==================== 插件元信息 ====================

typedef struct AgentxxPluginInfo {
    int                     api_version; ///< 必须 == AGENTXX_PLUGIN_API_VERSION
    AgentxxPluginStringView name;        ///< 唯一标识 (只读借用)
    AgentxxPluginStringView version;
    AgentxxPluginStringView description;
} AgentxxPluginInfo;

/// ==================== 统一异步操作原语 ====================

/// 操作终结状态 (AgentxxPluginOperatorNotify.done 的 status 参数)
#define AGENTXX_PLUGIN_OPERATOR_OK        0 ///< 成功 (payload = 结果数据)
#define AGENTXX_PLUGIN_OPERATOR_CANCELLED 1 ///< 已取消 (payload 可为 NULL/空)
#define AGENTXX_PLUGIN_OPERATOR_FAILED    2 ///< 失败 (payload = 错误信息)

/// 完成通知器 (宿主实现并随 start 下发; 操作终结时被调方须【恰好回调一次】)
/// - payload: 只读借用字符串视图 (可为 NULL/空)
/// - 线程安全: 可从被调方的任意线程回调, 宿主内部投递回 io 线程唤醒等待协程
typedef struct AgentxxPluginOperatorNotify {
    void (*done)(void* host_ud, int status, AgentxxPluginStringView payload);
    void* host_ud;
} AgentxxPluginOperatorNotify;

/// 完成回调 (统一形态; 宿主保证在宿主 io 线程派发)
/// payload 只读借用, 生命周期仅覆盖本次回调
typedef void (*AgentxxPluginOperatorCallback)(
    void*                   ud,
    int                     status,
    AgentxxPluginStringView payload
);

/// 异步调用句柄 (仅用于取消; 不可轮询/收尸; 宿主托管生命周期)
typedef struct AgentxxPluginOperatorHandle AgentxxPluginOperatorHandle;

/// 协作式取消请求函数 (【宿主 io 线程调用】, 非阻塞):
/// - 被调方应尽快收尾并经 notifier 上报 CANCELLED; 也允许继续完成并上报 OK/FAILED
/// - 不可取消的操作可留 NULL
typedef void (*AgentxxPluginOperatorCancelFunction)(void* user_data, void* op);

/* ==================== 工具定义 ==================== */

#define AGENTXX_PLUGIN_TOOL_FLAG_NONE         0
#define AGENTXX_PLUGIN_TOOL_FLAG_AUTO_SUMMARY (1 << 0) ///< 输出超限时自动压缩 (经 share_store 卸载)

typedef struct AgentxxPluginToolSpec {
    AgentxxPluginStringView name; ///< 须全局唯一 (与内置工具/MCP 工具同名将注册失败)
    AgentxxPluginStringView description;
    AgentxxPluginStringView parameters_json; ///< JSON Schema 字符串 (json object)

    /// 启动执行 (【宿主 io 线程调用】, 非阻塞; 两件套契约):
    /// - args_json/session_id/tool_call_id: 只读借用, 仅本次调用有效
    /// - 快同步工具: 算完 → notify->done(AGENTXX_PLUGIN_OPERATOR_OK, 结果 json) → 返回 NULL
    /// - 锚定协程/自管异步: 创建/挂起任务 → 返回 op 句柄
    /// - 失败: 返回 NULL 且 *error_out 输出错误 (host->alloc 分配)
    void* (*execute_start)(
        void*                              user_data,
        AgentxxPluginStringView            args_json,
        AgentxxPluginStringView            session_id,
        AgentxxPluginStringView            tool_call_id,
        const AgentxxPluginOperatorNotify* notify,
        char**                             error_out
    );
    /// 协作式取消请求 (io 线程, 非阻塞; 不可取消可留 NULL)
    void (*execute_cancel)(void* user_data, void* op);

    void* user_data;
    long  default_timeout_ms; ///< 0 = 不限制 (宿主按调用方取消语义执行)
    int   flags;              ///< AGENTXX_TOOL_FLAG_*
} AgentxxPluginToolSpec;

/* ==================== 中间件钩子 ==================== */

/// 钩子点 (与宿主 7 个中间件钩子一一对应)
typedef enum AgentxxPluginHookPoint {
    AGENTXX_PLUGIN_HOOK_AGENT_START = 0, ///< 会话轮次开始
    AGENTXX_PLUGIN_HOOK_AGENT_END,       ///< 会话轮次结束
    AGENTXX_PLUGIN_HOOK_MODEL_START,     ///< LLM 调用开始
    AGENTXX_PLUGIN_HOOK_MODEL_RUN,       ///< LLM 调用执行 (重试时多次触发)
    AGENTXX_PLUGIN_HOOK_MODEL_END,       ///< LLM 调用结束
    AGENTXX_PLUGIN_HOOK_TOOL_START,      ///< 工具分发开始
    AGENTXX_PLUGIN_HOOK_TOOL_END,        ///< 工具分发结束
    AGENTXX_PLUGIN_HOOK_COUNT
} AgentxxPluginHookPoint;

/// 钩子规格: 两件套形态
typedef struct AgentxxPluginHookSpec {
    AgentxxPluginHookPoint point;
    void* (*hook_start)(
        void*                              user_data,
        AgentxxPluginHookPoint             point,
        AgentxxPluginStringView            node_input_json,
        const AgentxxPluginOperatorNotify* notify,
        char**                             error_out
    );
    void (*hook_cancel)(void* user_data, void* op); ///< 可为 NULL
    void* user_data;
} AgentxxPluginHookSpec;

/* ==================== 事件订阅句柄 / 前向声明 ==================== */

typedef struct AgentxxPluginSubscription AgentxxPluginSubscription;
struct AgentxxPluginHost;

/// 能力方法处理器启动函数 (两件套契约):
typedef void* (*AgentxxPluginCapabilityStartFunction)(
    void*                              ctx,
    const AgentxxPluginHost*           caller_host,
    AgentxxPluginStringView            method,
    AgentxxPluginStringView            args_json,
    const AgentxxPluginOperatorNotify* notify,
    char**                             error_out
);

/* ==================== 核心宿主函数表 ==================== */

typedef struct AgentxxPluginHost AgentxxPluginHost;

/// 核心 vtable: 仅内存三件套 + COM 风格接口表查询
/// - 不建议更改该结构体，应当冻结
/// - 后续若实在需要破坏性更改，可通过声明不同版本的 [AgentxxHostVtable]，然后运行时框架根据插件版本
/// 自动兼容创建不同版本的 [AgentxxHostVtable] 适配
typedef struct AgentxxHostVtable {
    /* ---- 内存 (跨 CRT 堆边界的唯一分配通道; 任意线程可调用) ---- */
    void* (*alloc)(size_t size);
    void (*free)(void* ptr);
    char* (*strdup)(AgentxxPluginStringView s);

    /* ---- COM 风格接口表查询 (QueryInterface; 任意线程可调用) ---- */
    const void* (*query_interface)(const AgentxxPluginHost* host, AgentxxPluginStringView iid);
} AgentxxHostVtable;

struct AgentxxPluginHost {
    const AgentxxHostVtable* vtable; ///< 核心函数表 (宿主静态)
    void* opaque; ///< 宿主内部 (指向插件实例状态, 插件不得使用)
};

#define AGENTXX_PLUGIN_QUERY_IFACE(host, IfaceType, iid_name) \
    ((const IfaceType*)(host)->vtable->query_interface((host), agentxx_plugin_sv_cstr(iid_name)))

/* ==================== 接口表: 工具 (agentxx.agent.tools) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_TOOLS         "agentxx.agent.tools"
#define AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION 1

typedef struct AgentxxPluginToolsIface {
    int version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION

    /// 注册工具 (io 线程约束, 非 io 线程由宿主投递同步等待)
    /// `return`: 名称冲突返回非 0
    int (*register_tool)(const AgentxxPluginHost* host, const AgentxxPluginToolSpec* spec);
    /// 注销工具 (按名称)
    /// `return`: 不存在返回非 0
    int (*unregister_tool)(const AgentxxPluginHost* host, AgentxxPluginStringView name);

    /* ---- 插件互调: 完成回调形 (推荐) ---- */
    /// 异步调用插件工具 (宿主保证 cb 在宿主 io 线程派发, 一律 post 入队):
    /// - 返回的 AgentxxPluginOperatorHandle 仅用于 op_cancel; 宿主自动管理句柄生命周期
    /// - 查表/装配失败返回 NULL 并 error_out
    AgentxxPluginOperatorHandle* (*call_tool_async)(
        const AgentxxPluginHost*      host,
        AgentxxPluginStringView       name,
        AgentxxPluginStringView       args_json,
        AgentxxPluginStringView       session_id,
        AgentxxPluginOperatorCallback cb,
        void*                         ud,
        char**                        error_out
    );
    /// 取消异步操作 (幂等, 任意线程可调; 完成/退休后调用无害)
    void (*op_cancel)(AgentxxPluginOperatorHandle* op);
} AgentxxPluginToolsIface;

/* ==================== 接口表: 中间件钩子 (agentxx.agent.hooks) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_HOOKS         "agentxx.agent.hooks"
#define AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_VERSION 1

typedef struct AgentxxPluginHooksIface {
    int version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_VERSION

    /// 注册钩子 (两件套规格; 热插拔, 轮次边界生效; io 线程约束)
    int (*register_hook)(const AgentxxPluginHost* host, const AgentxxPluginHookSpec* spec);
    /// 注销钩子 (按 point 匹配); 不存在返回非 0
    int (*unregister_hook)(const AgentxxPluginHost* host, AgentxxPluginHookPoint point);
} AgentxxPluginHooksIface;

/* ==================== 接口表: 事件 (agentxx.agent.events) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_EVENTS         "agentxx.agent.events"
#define AGENTXX_PLUGIN_IFACE_AGENT_EVENTS_VERSION 1

typedef struct AgentxxPluginEventsIface {
    int version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_EVENTS_VERSION

    /// 订阅 (topic 自动加 "plugin." 前缀, 载荷为 JSON 字符串); 返回句柄
    AgentxxPluginSubscription* (*subscribe)(
        const AgentxxPluginHost* host,
        AgentxxPluginStringView  topic,
        void (*handler)(AgentxxPluginStringView event_json, void* ud),
        void* ud
    );
    void (*unsubscribe)(AgentxxPluginSubscription* sub);
    /// 发布 (异步投递, 立即返回; 禁用状态的插件被拒绝)
    int (*publish)(
        const AgentxxPluginHost* host,
        AgentxxPluginStringView  topic,
        AgentxxPluginStringView  event_json
    );
} AgentxxPluginEventsIface;

/* ==================== 接口表: 能力 (agentxx.agent.capabilities) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES         "agentxx.agent.capabilities"
#define AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES_VERSION 1

typedef struct AgentxxPluginCapabilitiesIface {
    int version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES_VERSION

    /// 声明能力 (无方法处理器; 仅标记/互查; io 线程约束)
    int (*register_capability)(const AgentxxPluginHost* host, AgentxxPluginStringView capability);
    /// 注册能力并附带两件套方法处理器 (通用插件间通信通道):
    int (*register_capability_ex)(
        const AgentxxPluginHost*             host,
        AgentxxPluginStringView              capability,
        AgentxxPluginCapabilityStartFunction start,
        AgentxxPluginOperatorCancelFunction  cancel,
        void*                                ctx
    );
    int (*unregister_capability)(const AgentxxPluginHost* host, AgentxxPluginStringView capability);
    /// 是否存在指定能力 (io 线程查表)
    int (*has_capability)(const AgentxxPluginHost* host, AgentxxPluginStringView capability);

    /* ---- 能力调用: 完成回调形 ---- */
    /// 异步调用能力提供者的方法: 宿主保证 cb 在 io 线程派发
    AgentxxPluginOperatorHandle* (*invoke_capability_async)(
        const AgentxxPluginHost*      host,
        AgentxxPluginStringView       capability,
        AgentxxPluginStringView       method,
        AgentxxPluginStringView       args_json,
        AgentxxPluginOperatorCallback cb,
        void*                         ud,
        char**                        error_out
    );
    /// 取消异步操作 (幂等, 任意线程可调)
    void (*op_cancel)(AgentxxPluginOperatorHandle* op);
} AgentxxPluginCapabilitiesIface;

/* ==================== 接口表: 任务调度 (agentxx.agent.scheduler) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER         "agentxx.agent.scheduler"
#define AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER_VERSION 1

typedef struct AgentxxPluginSchedulerIface {
    int version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER_VERSION

    /// 当前线程是否为宿主 io 线程 (任意线程可调用)
    int (*is_io_thread)(const AgentxxPluginHost* host);
    /// 投递任务到宿主 io 线程异步执行 (不等待, 线程安全)
    void (*post_to_io)(const AgentxxPluginHost* host, void (*fn)(void* ud), void* ud);
    /// 推进并执行当前待处理的 io 任务 (仅 io 线程调用; 阻塞等待场景避免死锁)
    void (*pump_io)(const AgentxxPluginHost* host);
    /// 一次性定时器 (宿主保证在 io 线程派发, 经 asio::post 入队)
    /// 返回 timer 句柄 (宿主所有); 插件卸载时自动取消
    void* (*sleep)(const AgentxxPluginHost* host, long ms, void (*cb)(void* ud), void* ud);
    /// 取消定时器 (触发提前唤醒回调或取消)
    void (*cancel_sleep)(const AgentxxPluginHost* host, void* timer);

    /* ---- 阻塞池委托 ---- */
    /// 在宿主阻塞线程池执行同步回调 (阻塞操作专用: 文件遍历/HTTP/子进程等)
    /// - cancel_flag: 调用方持有的取消标志 (volatile int; 0=未取消 1=已取消)
    /// - work: 在阻塞池线程执行; 返回结果与 error_out 须 host->alloc 分配
    /// - done: 宿主保证在 io 线程派发 (经 asio::post 入队)
    void (*offload)(
        const AgentxxPluginHost* host,
        volatile int*            cancel_flag,
        void* (*work)(void* ud, volatile int* cancel_flag, char** error_out),
        void (*done)(void* ud, void* result, AgentxxPluginStringView error),
        void* ud
    );
} AgentxxPluginSchedulerIface;

/* ==================== 接口表: 会话访问 (agentxx.agent.session) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_SESSION         "agentxx.agent.session"
#define AGENTXX_PLUGIN_IFACE_AGENT_SESSION_VERSION 1

typedef struct AgentxxPluginSessionIface {
    int version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_SESSION_VERSION

    /// 读取会话级 share_store 条目 (仅 io 线程); 不存在返回 NULL (host->alloc)
    char* (*get_share_store)(
        const AgentxxPluginHost* host,
        AgentxxPluginStringView  session_id,
        long long                id
    );
    /// 向会话 UI 推送提示消息 (仅 io 线程); level: 0=info 1=warning 2=error
    void (*emit_message_tip)(
        const AgentxxPluginHost* host,
        AgentxxPluginStringView  session_id,
        AgentxxPluginStringView  text,
        int                      level
    );
    /// 写入会话级 share_store 条目并返回 ID (仅 io 线程); 失败返回 -1
    /// - content 为待存储的完整文本 (host 侧按行切片等处理与工具侧一致)
    long long (*add_share_store)(
        const AgentxxPluginHost* host,
        AgentxxPluginStringView  session_id,
        AgentxxPluginStringView  content
    );
} AgentxxPluginSessionIface;

/* ==================== 接口表: 插件互查 (agentxx.agent.plugins) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS         "agentxx.agent.plugins"
#define AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS_VERSION 1

typedef struct AgentxxPluginsIface {
    int version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS_VERSION

    /// 全部已安装插件信息 JSON 数组 (host->alloc 分配)
    char* (*list_plugins)(const AgentxxPluginHost* host);
    /// 单个插件信息 JSON (未安装返回 NULL, host->alloc)
    char* (*get_plugin)(const AgentxxPluginHost* host, AgentxxPluginStringView name);
    /// 调用方插件自身信息 JSON {"name","version","description","path",...} (host->alloc)
    char* (*get_own_info)(const AgentxxPluginHost* host);
} AgentxxPluginsIface;

/* ==================== 接口表: 宿主配置 (agentxx.agent.config) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_CONFIG         "agentxx.agent.config"
#define AGENTXX_PLUGIN_IFACE_AGENT_CONFIG_VERSION 1

typedef struct AgentxxPluginConfigIface {
    int version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_CONFIG_VERSION

    /// 宿主 AgentConfig 关键字段 JSON (io 线程; host->alloc):
    /// {"dataDir": "...", "projectRoot": "..."(可为空), "platform": "windows"|"linux"|"macos"}
    char* (*get_config)(const AgentxxPluginHost* host);
    /// 本插件配置参数 JSON (yaml `plugins` 条目 args; io 线程; host->alloc):
    char* (*get_plugin_args)(const AgentxxPluginHost* host);
    /// 宿主 toolPrompt 配置 (io 线程; host->alloc):
    /// {"depict": "...", "args": {"参数名": "参数说明", ...}}
    char* (*get_tool_prompt)(const AgentxxPluginHost* host, AgentxxPluginStringView tool_name);
    /// 指定会话生效的工作目录 (io 线程; host->alloc; 失败/未装配返回 NULL):
    /// - session_id 非空: worktree 绑定优先, 依次回退会话覆写 / AgentConfig
    /// - session_id 为空: 返回解析后的默认会话工作目录
    char* (*get_session_work_dir)(
        const AgentxxPluginHost* host,
        AgentxxPluginStringView  session_id
    );
    /// 本插件配置文件所在目录或文件路径 (yaml `plugins` 条目 config; io 线程;
    /// host->alloc; 未指定返回 NULL, 空串表示未配置)
    /// - 可指向文件或目录 (由插件自行判断类型并加载)
    /// - 宿主已归一化为绝对路径 (正斜杠, lexically_normal)
    char* (*get_plugin_config_path)(const AgentxxPluginHost* host);
} AgentxxPluginConfigIface;

/* ==================== 接口表: 主模型配置 (agentxx.agent.model) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_MODEL         "agentxx.agent.model"
#define AGENTXX_PLUGIN_IFACE_AGENT_MODEL_VERSION 1

typedef struct AgentxxPluginModelIface {
    int version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_MODEL_VERSION

    /// 宿主主模型及关联配置 JSON (io 线程; host->alloc; 未装配返回 NULL):
    char* (*get_config)(const AgentxxPluginHost* host);
} AgentxxPluginModelIface;

/* ==================== 接口表: 会话取消状态 (agentxx.agent.cancel) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_CANCEL         "agentxx.agent.cancel"
#define AGENTXX_PLUGIN_IFACE_AGENT_CANCEL_VERSION 1

typedef struct AgentxxPluginCancelIface {
    int version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_CANCEL_VERSION

    /// 查询会话当前轮次是否已取消 (advisory 定位; 权威通知始终是 cancel 回调)
    int (*is_cancelled)(const AgentxxPluginHost* host, AgentxxPluginStringView session_id);
} AgentxxPluginCancelIface;

/* ==================== 接口表: 宿主提示词读写 (agentxx.agent.prompt) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_PROMPT         "agentxx.agent.prompt"
#define AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_VERSION 1

typedef struct AgentxxPluginPromptIface {
    int version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_VERSION

    char* (*get_prompt)(const AgentxxPluginHost* host);
    int (*set_prompt)(const AgentxxPluginHost* host, AgentxxPluginStringView prompt_json);
} AgentxxPluginPromptIface;

/* ==================== 接口表: JSON 辅助 (agentxx.agent.json) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_JSON         "agentxx.agent.json"
#define AGENTXX_PLUGIN_IFACE_AGENT_JSON_VERSION 1

typedef struct AgentxxPluginJsonIface {
    int version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_JSON_VERSION

    char* (*json_get_string)(
        const AgentxxPluginHost* host,
        AgentxxPluginStringView  json,
        AgentxxPluginStringView  key
    );
    char* (*json_escape)(const AgentxxPluginHost* host, AgentxxPluginStringView s);
} AgentxxPluginJsonIface;

/* ==================== 接口表: 日志 (agentxx.agent.log) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_LOG         "agentxx.agent.log"
#define AGENTXX_PLUGIN_IFACE_AGENT_LOG_VERSION 1

typedef struct AgentxxPluginLogIface {
    int version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_LOG_VERSION

    void (*log)(const AgentxxPluginHost* host, int level, AgentxxPluginStringView msg);
} AgentxxPluginLogIface;

/* ==================== 接口表: 会话资源贡献 (agentxx.agent.resources) ==================== */
/* 注: 仅支持两类来源 —— 1) plugin.yaml 声明式段 (skill/memory/mcp, 见
 * PluginManifestResources) 2) 插件初始化时 (agentxx_plugin_agent_create 内)
 * 追加注册。初始化完成后资源冻结，后续固定不可变以防上下文变化；
 * 运行时 register/unregister 在冻结后返回非 0 失败 (宿主日志 WARN)。
 */

#define AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES         "agentxx.agent.resources"
#define AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_VERSION 1

typedef struct AgentxxPluginResourcesIface {
    int version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_VERSION

    int (*register_skill_dir)(const AgentxxPluginHost* host, AgentxxPluginStringView path);
    int (*unregister_skill_dir)(const AgentxxPluginHost* host, AgentxxPluginStringView path);
    int (*register_memory_file)(const AgentxxPluginHost* host, AgentxxPluginStringView path);
    int (*unregister_memory_file)(const AgentxxPluginHost* host, AgentxxPluginStringView path);
    int (*register_mcp_server)(const AgentxxPluginHost* host, AgentxxPluginStringView spec_json);
    int (*unregister_mcp_server)(const AgentxxPluginHost* host, AgentxxPluginStringView name_space);
    char* (*get_own_resources)(const AgentxxPluginHost* host);
} AgentxxPluginResourcesIface;

/* ==================== 接口表: 执行图 (agentxx.agent.graph) ==================== */

/// 插件节点执行函数 (两件套契约; 【宿主 io 线程调用】, 非阻塞):
/// - node_name/config_json/state_json/thread_id: 只读借用, 仅本次调用有效
/// - state_json 为 GraphState::serialize() 的结果: {"channels": {<ch名>: {"value": ..., "version":
/// N}}, "global_version": N}
///   (插件只读; 修改须经返回的 writes)
/// - 完成时 notify->done(OK, payload): payload 为节点输出 JSON (host->alloc):
///   {"writes": [{"channel": "...", "value": ..., "mode": "reduce"|"overwrite"}],
///    "command": {"goto_node": "...", "updates": [...]} | null,
///    "sends": [{"target_node": "...", "input": {...}}]}
/// - 快同步节点: 算完 → done → 返回 NULL; 锚定协程/自管异步: 返回 op 句柄
/// - 失败: 返回 NULL 且 *error_out 输出错误 (host->alloc 分配)
typedef void* (*AgentxxPluginGraphNodeRunStartFn)(
    void*                              user_data,
    AgentxxPluginStringView            node_name,
    AgentxxPluginStringView            config_json,
    AgentxxPluginStringView            state_json,
    AgentxxPluginStringView            thread_id,
    const AgentxxPluginOperatorNotify* notify,
    char**                             error_out
);
/// 协作式取消请求 (io 线程, 非阻塞; 不可取消可留 NULL)
typedef void (*AgentxxPluginGraphNodeRunCancelFn)(void* user_data, void* op);

/// 插件节点类型注册规格
typedef struct AgentxxPluginGraphNodeTypeSpec {
    AgentxxPluginStringView           type;       ///< 节点类型名 (须全局唯一)
    AgentxxPluginGraphNodeRunStartFn  run_start;  ///< 节点执行 (两件套契约)
    AgentxxPluginGraphNodeRunCancelFn run_cancel; ///< 可空
    void*                             user_data;  ///< 透传给 run_start/run_cancel
    /// 可选节点 config JSON Schema (Draft 2020-12 片段; 仅供导出/文档, 引擎不校验)
    AgentxxPluginStringView config_schema_json;
} AgentxxPluginGraphNodeTypeSpec;

/// 执行图接口表: 插件注册自定义节点类型 + 读写宿主执行图 JSON 定义
/// - 节点类型注册进 per-agent GraphRegistry (多实例隔离), 图编译时按类型名
///   实例化; 卸载插件后不再编译新图 (engine 已构建), 注册残留无害
/// - get/set graph JSON 用于插件查看/修改宿主执行图 (默认名 "agentxx.default");
///   修改后的 JSON 在宿主构建 engine 前生效 (插件须保证图合法性, 非法时宿主
///   回退默认图并记日志)
#define AGENTXX_PLUGIN_IFACE_AGENT_GRAPH         "agentxx.agent.graph"
#define AGENTXX_PLUGIN_IFACE_AGENT_GRAPH_VERSION 1

typedef struct AgentxxPluginGraphIface {
    int version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_GRAPH_VERSION

    /// 注册节点类型 (io 线程约束, 非 io 线程由宿主投递同步等待)
    /// `return`: 类型名冲突返回非 0
    int (*register_node_type)(
        const AgentxxPluginHost*              host,
        const AgentxxPluginGraphNodeTypeSpec* spec
    );
    /// 注销节点类型 (按类型名; 卸载时宿主自动清理)
    /// `return`: 不存在返回非 0
    int (*unregister_node_type)(const AgentxxPluginHost* host, AgentxxPluginStringView type);
    /// 获取当前执行图 JSON 定义 (host->alloc; 插件可基于此判断后 set 修改)
    char* (*get_graph_json)(const AgentxxPluginHost* host);
    /// 获取当前执行图名称 (host->alloc; 默认 "agentxx.default")
    char* (*get_graph_name)(const AgentxxPluginHost* host);
    /// 设置执行图 JSON 定义 (覆盖; 宿主构建 engine 前生效)
    /// `return`: JSON 非法返回非 0 (host 侧解析失败)
    int (*set_graph_json)(const AgentxxPluginHost* host, AgentxxPluginStringView graph_json);
} AgentxxPluginGraphIface;

/* ==================== 接口表: 后台任务 (agentxx.agent.tasks) ==================== */
/* 背景: kit (plugin_kit.h) 的 spawn 让插件启动后台协作任务 (如周期采集:
 * while(!cancelled()) { offload; sleep; })。本表把 spawn 纳入宿主统一任务
 * 管理 —— 与工具/能力 op 同构: 宿主登记句柄 (outstandingOps, detachAll 统一
 * 取消) + 持 inflight (waitInflightZero 精确等待) + notify.done 完成通知
 * (宿主回收句柄)。插件卸载时 detachAll cancel → 协程退出 → notify → inflight
 * 归零 → dlclose 安全, 无协程帧悬挂/UAF。
 */

#define AGENTXX_PLUGIN_IFACE_AGENT_TASKS         "agentxx.agent.tasks"
#define AGENTXX_PLUGIN_IFACE_AGENT_TASKS_VERSION 1

typedef struct AgentxxPluginTasksIface {
    int version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_TASKS_VERSION

    /// 注册后台任务 (io 线程约束, 非 io 线程由宿主投递同步等待)。宿主记录
    /// 句柄 (可取消/跟踪完成/持 inflight), 插件协程最终结束时经 *notify
    /// 上报 (恰好一次) → 宿主回收句柄。
    /// - cancel_fn/cancel_ud: 宿主卸载取消时回调 (宿主 io 线程, 协作式):
    ///   置 cancelFlag + 唤醒挂起的 sleep/offload; 不可取消可传 NULL
    /// - notify: 【出参】宿主填写的完成通知器 (AgentxxPluginOperatorNotify 值
    ///   拷贝); 插件协程结束 (帧销毁后) 经 notify.done 恰好一次上报 → 宿主
    ///   guard.reset + 回收句柄。以 const 指针形式入参无法回填 —— 宿主只能
    ///   自建一个无法告知插件的 notify, 与本表"插件上报完成"语义矛盾, 必须
    ///   为出参
    /// - notify.done 线程属性与既有 ABI 契约一致: 可从【任意线程】回调
    ///   (宿主 OpCore::onDone 内部原子 CAS + 投递回 io, 线程安全) —— spawn
    ///   协程内若直接调用宿主回调形接口 (invoke_capability_async 等) 或经
    ///   自管线程收尾, 上报可能非 io 线程, 宿主必须按任意线程实现
    /// - 返回宿主托管句柄 (失败返回 NULL 并 *error_out 输出错误, host->alloc)
    AgentxxPluginOperatorHandle* (*register_task)(
        const AgentxxPluginHost*            host,
        AgentxxPluginOperatorCancelFunction cancel_fn,
        void*                               cancel_ud,
        AgentxxPluginOperatorNotify*        notify, ///< [out] 见上
        char**                              error_out
    );
    /// 取消任务 (幂等; 仅限 io 线程调用, 或宿主内部经 ioCallSync 投递后调用)
    /// - 与宿主 detachAll 内部路径一致; 句柄由宿主托管, 跨线程主动取消需经
    ///   scheduler.post_to_io / ioCallSync 回到 io 线程 (与注册类接口线程
    ///   约束一致), 避免 handle->caller 裸指针跨线程反查实例
    void (*cancel_task)(AgentxxPluginOperatorHandle* h);
} AgentxxPluginTasksIface;

/* ==================== 插件入口符号 (dlsym) ==================== */

typedef const AgentxxPluginInfo* (*AgentxxPluginGetInfoFn)(void);
typedef int (*AgentxxPluginCreateFn)(const AgentxxPluginHost* host, void** plugin_ctx);
typedef void (*AgentxxPluginDestroyFn)(void* plugin_ctx);

#define AGENTXX_PLUGIN_AGENT_SYMBOL_GET_INFO "agentxx_plugin_agent_get_info"
#define AGENTXX_PLUGIN_AGENT_SYMBOL_CREATE   "agentxx_plugin_agent_create"
#define AGENTXX_PLUGIN_AGENT_SYMBOL_DESTROY  "agentxx_plugin_agent_destroy"

/// 内置插件描述 (编译进 libagentxx 的插件; 静态数组, 进程生命周期有效)
typedef struct AgentxxPluginBuiltinInfo {
    AgentxxPluginStringView name; ///< 插件唯一名 (如 "example_plugin"); NULL = 空表占位
    AgentxxPluginGetInfoFn get_info; ///< 可空 (加载前元信息校验, 与 dlsym 可选符号同语义)
    AgentxxPluginCreateFn create; ///< 必需 (实例创建, 与 agentxx_plugin_agent_create 同契约)
    AgentxxPluginDestroyFn destroy; ///< 可空 (实例销毁, 与 agentxx_plugin_agent_destroy 同契约)
} AgentxxPluginBuiltinInfo;

// ==================== 内嵌编译清单 ====================
// 与 BuiltinPluginInfo 同步生成于 plugins/builtin_plugins.cpp
typedef struct AgentxxPluginBuiltinManifest {
    AgentxxPluginStringView name; ///< 插件名 (与 BuiltinPluginInfo.name 一致)
    AgentxxPluginStringView yaml; ///< plugin.yaml 原文 (UTF-8, 静态只读)
} AgentxxPluginBuiltinManifest;

/// 查询全部内置插件 (libagentxx 实现; 返回静态数组, count 输出条目数)
/// - 调用方须跳过 name == NULL 的占位条目 (空表时 count 为 1)
/// - 任意线程可调用 (静态只读数据)
const AgentxxPluginBuiltinInfo* agentxx_plugin_get_builtin_plugins(size_t* count);

const AgentxxPluginBuiltinManifest* agentxx_plugin_get_builtin_manifests(size_t* count);

#define AGENTXX_PLUGIN_STRDUP(host, s)     ((host)->vtable->strdup(agentxx_plugin_sv_cstr(s)))
#define AGENTXX_PLUGIN_STRDUP_SV(host, sv) ((host)->vtable->strdup((sv)))

#ifdef __cplusplus
}
#endif

#endif /* AGENTXX_PLUGIN_API_H */
