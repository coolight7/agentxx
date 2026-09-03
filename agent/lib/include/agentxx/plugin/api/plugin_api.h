///
/// agentxx/plugin/api/plugin_api.h —— 插件系统纯 C ABI 契约 (agent 侧; 跨语言跨编译器稳定接口)
///
/// ════════════════════════════════════════════════════════════════════
/// 架构: COM 风格接口表查询 (API v1 重构版)
/// ════════════════════════════════════════════════════════════════════
/// - 明确字节对齐: 全部跨边界 ABI 结构体严格遵循 8 字节对齐 (#pragma pack(push, 8))
/// - 明确基本类型: 统一使用定长基本类型 (int32_t, int64_t, uint64_t)，杜绝 int/long
///   跨平台字节宽度差异 (LLP64 vs LP64)
/// - 明确函数调用约定: 接口表函数指针、入口符号与回调全部显式标注 AGENTXX_PLUGIN_CALL (__stdcall)
/// - 结构体传递与返回值规范:
///   * 结构体入参统一采用指针传递 (const Struct*)，杜绝结构体按值传参
///   * 结构体返回值统一改为函数出参 (Struct* out) 并返回 int32_t 状态码 (0=成功)
/// - 核心 vtable 极简与最小正交基:
///   * 跨堆内存两件套: alloc(uint64_t) / free(void*)
///   * COM 风格能力查询: query_interface (strdup 移出 vtable 改由内联函数基于 alloc 实现)
/// - 版本策略:
///   * 全局 AGENTXX_PLUGIN_API_VERSION 严格匹配门禁 (当前为 1)
///   * 接口表首字段为 int32_t version (全部重置为 1)
///
#ifndef AGENTXX_PLUGIN_API_H
#define AGENTXX_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 插件导出符号控制与调用约定 ==================== */

#if defined(AGENTXX_PLUGIN_BUILTIN)
#define AGENTXX_PLUGIN_EXPORT
#elif defined(_WIN32)
#define AGENTXX_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define AGENTXX_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define AGENTXX_PLUGIN_EXPORT
#endif

#if defined(_WIN32)
#define AGENTXX_PLUGIN_CALL __stdcall
#elif defined(__GNUC__) || defined(__clang__)
#if defined(__i386__)
#define AGENTXX_PLUGIN_CALL __attribute__((stdcall))
#else
#define AGENTXX_PLUGIN_CALL
#endif
#else
#define AGENTXX_PLUGIN_CALL
#endif

/// 全局 API 版本 (agent 侧)
#define AGENTXX_PLUGIN_API_VERSION 1

#pragma pack(push, 8)

/* ==================== 字符串视图 (跨边界只读参数统一形态) ==================== */

/// 只读字符串视图: 指向调用方内存 (UTF-8), 不要求 NUL 结尾
/// - C ABI: data(8) + size(8) 纯 POD, 恒按指针/出参传递 (不按值跨边界)
/// - C++ 便捷: 提供值→指针转换成员与 empty() 查询 (布局不变, 便于值语义调用)
typedef struct AgentxxPluginStringView {
    const char* data; ///< 指向 UTF-8 字节序列 (可含任意字节, 不必 NUL 结尾)
    uint64_t    size; ///< 字节数 (明确定长 64 位)

#ifdef __cplusplus
    AgentxxPluginStringView() :
        data(nullptr),
        size(0) {}

    AgentxxPluginStringView(const char* d, uint64_t n) :
        data(d),
        size(n) {}

    operator const AgentxxPluginStringView*() const {
        return this;
    }

    bool empty() const {
        return data == nullptr || size == 0;
    }
#endif
} AgentxxPluginStringView;

typedef struct AgentxxPluginHost AgentxxPluginHost;

/* ==================== 跨边界堆分配字符串 (具有显式所有权) ==================== */

/// 跨 CRT 堆分配的 UTF-8 字符串 (显式所有权: 由宿主分配, 调用方接管并负责释放)
typedef struct AgentxxPluginString {
    char* data; ///< 指向宿主堆分配的 UTF-8 字节序列 (以 \0 结尾; 空串或 NULL 时可为 NULL)
    uint64_t size; ///< 字节数 (不含结尾 \0; O(1) 访问)

#ifdef __cplusplus
    AgentxxPluginString() :
        data(nullptr),
        size(0) {}

    AgentxxPluginString(char* d, uint64_t n) :
        data(d),
        size(n) {}

    operator const AgentxxPluginString*() const {
        return this;
    }
#endif
} AgentxxPluginString;

/// ==================== 插件元信息 ====================

typedef struct AgentxxPluginInfo {
    int32_t                 api_version; ///< 必须 == AGENTXX_PLUGIN_API_VERSION
    uint32_t                _reserved;   ///< 8 字节补齐
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
/// - payload: 只读借用字符串视图指针 (可为 NULL/空)
/// - 线程安全: 可从被调方的任意线程回调, 宿主内部投递回 io 线程唤醒等待协程
typedef struct AgentxxPluginOperatorNotify {
    void(AGENTXX_PLUGIN_CALL*
             done)(void* host_ud, int32_t status, const AgentxxPluginStringView* payload);
    void* host_ud;
} AgentxxPluginOperatorNotify;

/// 完成回调 (统一形态; 宿主保证在宿主 io 线程派发)
/// payload 只读借用指针, 生命周期仅覆盖本次回调
typedef void(AGENTXX_PLUGIN_CALL* AgentxxPluginOperatorCallback)(
    void*                          ud,
    int32_t                        status,
    const AgentxxPluginStringView* payload
);

/// 异步调用句柄 (仅用于取消; 不可轮询/收尸; 宿主托管生命周期)
typedef struct AgentxxPluginOperatorHandle AgentxxPluginOperatorHandle;

/// 协作式取消请求函数 (【宿主 io 线程调用】, 非阻塞):
typedef void(AGENTXX_PLUGIN_CALL* AgentxxPluginOperatorCancelFunction)(void* user_data, void* op);

/* ==================== 工具定义 ==================== */

#define AGENTXX_PLUGIN_TOOL_FLAG_NONE         0
#define AGENTXX_PLUGIN_TOOL_FLAG_AUTO_SUMMARY (1 << 0) ///< 输出超限时自动压缩 (经 share_store 卸载)

typedef struct AgentxxPluginToolSpec {
    AgentxxPluginStringView name; ///< 须全局唯一 (与内置工具/MCP 工具同名将注册失败)
    AgentxxPluginStringView description;
    AgentxxPluginStringView parameters_json; ///< JSON Schema 字符串 (json object)

    /// 启动执行 (【宿主 io 线程调用】, 非阻塞; 两件套契约):
    /// - 入参均为指针传递 (只读借用, 仅本次调用有效)
    /// - 快同步工具: 算完 → notify->done(AGENTXX_PLUGIN_OPERATOR_OK, &res_sv) → 返回 NULL
    /// - 锚定协程/自管异步: 创建/挂起任务 → 返回 op 句柄
    /// - 失败: 返回 NULL 且 *error_out 输出错误 (跨边界堆分配字符串, host->alloc 分配)
    void*(AGENTXX_PLUGIN_CALL* execute_start)(
        void*                              user_data,
        const AgentxxPluginStringView*     args_json,
        const AgentxxPluginStringView*     session_id,
        const AgentxxPluginStringView*     tool_call_id,
        const AgentxxPluginOperatorNotify* notify,
        AgentxxPluginString*               error_out
    );
    /// 协作式取消请求 (io 线程, 非阻塞; 不可取消可留 NULL)
    void(AGENTXX_PLUGIN_CALL* execute_cancel)(void* user_data, void* op);

    void*    user_data;
    int64_t  default_timeout_ms; ///< 0 = 不限制 (定长 64 位整型)
    int32_t  flags;              ///< AGENTXX_PLUGIN_TOOL_FLAG_*
    uint32_t _reserved;          ///< 8 字节补齐
} AgentxxPluginToolSpec;

/* ==================== 中间件钩子 ==================== */

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
    int32_t  point;     ///< AgentxxPluginHookPoint (明确 32 位整型)
    uint32_t _reserved; ///< 8 字节补齐
    void*(AGENTXX_PLUGIN_CALL* hook_start)(
        void*                              user_data,
        int32_t                            point,
        const AgentxxPluginStringView*     node_input_json,
        const AgentxxPluginOperatorNotify* notify,
        AgentxxPluginString*               error_out
    );
    void(AGENTXX_PLUGIN_CALL* hook_cancel)(void* user_data, void* op); ///< 可为 NULL
    void* user_data;
} AgentxxPluginHookSpec;

/* ==================== 事件订阅句柄 / 前向声明 ==================== */

typedef struct AgentxxPluginSubscription AgentxxPluginSubscription;

/// 能力方法处理器启动函数 (两件套契约):
typedef void*(AGENTXX_PLUGIN_CALL* AgentxxPluginCapabilityStartFunction)(
    void*                              ctx,
    const AgentxxPluginHost*           caller_host,
    const AgentxxPluginStringView*     method,
    const AgentxxPluginStringView*     args_json,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error_out
);

/* ==================== 核心宿主函数表 ==================== */

/// 核心 vtable: 极简正交基 (内存两件套 + COM 风格接口表查询)
typedef struct AgentxxHostVtable {
    /* ---- 内存 (跨 CRT 堆边界的唯一分配通道; 任意线程可调用) ---- */
    void*(AGENTXX_PLUGIN_CALL* alloc)(uint64_t size);
    void(AGENTXX_PLUGIN_CALL* free)(void* ptr);

    /* ---- COM 风格接口表查询 (QueryInterface; 任意线程可调用) ---- */
    const void*(AGENTXX_PLUGIN_CALL* query_interface)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* iid
    );
} AgentxxHostVtable;

struct AgentxxPluginHost {
    const AgentxxHostVtable* vtable; ///< 核心函数表 (宿主静态)
    void* opaque; ///< 宿主内部 (指向插件实例状态, 插件不得使用)
};

/* ==================== 接口表: 工具 (agentxx.agent.tools) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_TOOLS         "agentxx.agent.tools"
#define AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION 1

typedef struct AgentxxPluginToolsIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION
    uint32_t _reserved;

    /// 注册工具 (io 线程约束, 非 io 线程由宿主投递同步等待)
    /// `return`: 0 成功, 非 0 冲突或失败
    int32_t(AGENTXX_PLUGIN_CALL* register_tool)(
        const AgentxxPluginHost*     host,
        const AgentxxPluginToolSpec* spec
    );
    /// 注销工具 (按名称)
    /// `return`: 0 成功, 非 0 不存在
    int32_t(AGENTXX_PLUGIN_CALL* unregister_tool)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* name
    );

    /* ---- 插件互调: 完成回调形 ---- */
    AgentxxPluginOperatorHandle*(AGENTXX_PLUGIN_CALL* call_tool_async)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* name,
        const AgentxxPluginStringView* args_json,
        const AgentxxPluginStringView* session_id,
        AgentxxPluginOperatorCallback  cb,
        void*                          ud,
        AgentxxPluginString*           error_out
    );
    void(AGENTXX_PLUGIN_CALL* op_cancel)(AgentxxPluginOperatorHandle* op);
} AgentxxPluginToolsIface;

/* ==================== 接口表: 中间件钩子 (agentxx.agent.hooks) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_HOOKS         "agentxx.agent.hooks"
#define AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_VERSION 1

typedef struct AgentxxPluginHooksIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_VERSION
    uint32_t _reserved;

    int32_t(AGENTXX_PLUGIN_CALL* register_hook)(
        const AgentxxPluginHost*     host,
        const AgentxxPluginHookSpec* spec
    );
    int32_t(AGENTXX_PLUGIN_CALL* unregister_hook)(const AgentxxPluginHost* host, int32_t point);
} AgentxxPluginHooksIface;

/* ==================== 接口表: 事件 (agentxx.agent.events) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_EVENTS         "agentxx.agent.events"
#define AGENTXX_PLUGIN_IFACE_AGENT_EVENTS_VERSION 1

typedef struct AgentxxPluginEventsIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_EVENTS_VERSION
    uint32_t _reserved;

    AgentxxPluginSubscription*(AGENTXX_PLUGIN_CALL* subscribe)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* topic,
        void(AGENTXX_PLUGIN_CALL* handler)(const AgentxxPluginStringView* event_json, void* ud),
        void* ud
    );
    void(AGENTXX_PLUGIN_CALL* unsubscribe)(AgentxxPluginSubscription* sub);
    int32_t(AGENTXX_PLUGIN_CALL* publish)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* topic,
        const AgentxxPluginStringView* event_json
    );
} AgentxxPluginEventsIface;

/* ==================== 接口表: 能力 (agentxx.agent.capabilities) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES         "agentxx.agent.capabilities"
#define AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES_VERSION 1

typedef struct AgentxxPluginCapabilitiesIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES_VERSION
    uint32_t _reserved;

    int32_t(AGENTXX_PLUGIN_CALL* register_capability)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* capability
    );
    int32_t(AGENTXX_PLUGIN_CALL* register_capability_ex)(
        const AgentxxPluginHost*             host,
        const AgentxxPluginStringView*       capability,
        AgentxxPluginCapabilityStartFunction start,
        AgentxxPluginOperatorCancelFunction  cancel,
        void*                                ctx
    );
    int32_t(AGENTXX_PLUGIN_CALL* unregister_capability)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* capability
    );
    int32_t(AGENTXX_PLUGIN_CALL* has_capability)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* capability
    );

    AgentxxPluginOperatorHandle*(AGENTXX_PLUGIN_CALL* invoke_capability_async)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* capability,
        const AgentxxPluginStringView* method,
        const AgentxxPluginStringView* args_json,
        AgentxxPluginOperatorCallback  cb,
        void*                          ud,
        AgentxxPluginString*           error_out
    );
    void(AGENTXX_PLUGIN_CALL* op_cancel)(AgentxxPluginOperatorHandle* op);
} AgentxxPluginCapabilitiesIface;

/* ==================== 接口表: 任务调度 (agentxx.agent.scheduler) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER         "agentxx.agent.scheduler"
#define AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER_VERSION 1

typedef struct AgentxxPluginSchedulerIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER_VERSION
    uint32_t _reserved;

    int32_t(AGENTXX_PLUGIN_CALL* is_io_thread)(const AgentxxPluginHost* host);
    void(AGENTXX_PLUGIN_CALL* post_to_io)(
        const AgentxxPluginHost* host,
        void(AGENTXX_PLUGIN_CALL* fn)(void* ud),
        void* ud
    );
    void(AGENTXX_PLUGIN_CALL* pump_io)(const AgentxxPluginHost* host);
    void*(AGENTXX_PLUGIN_CALL* sleep)(
        const AgentxxPluginHost* host,
        int64_t                  ms,
        void(AGENTXX_PLUGIN_CALL* cb)(void* ud),
        void* ud
    );
    void(AGENTXX_PLUGIN_CALL* cancel_sleep)(const AgentxxPluginHost* host, void* timer);

    void(AGENTXX_PLUGIN_CALL* offload)(
        const AgentxxPluginHost* host,
        volatile int32_t*        cancel_flag,
        void*(AGENTXX_PLUGIN_CALL*
                  work)(void* ud, volatile int32_t* cancel_flag, AgentxxPluginString* error_out),
        void(AGENTXX_PLUGIN_CALL*
                 done)(void* ud, void* result, const AgentxxPluginStringView* error),
        void* ud
    );
} AgentxxPluginSchedulerIface;

/* ==================== 接口表: 会话访问 (agentxx.agent.session) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_SESSION         "agentxx.agent.session"
#define AGENTXX_PLUGIN_IFACE_AGENT_SESSION_VERSION 1

typedef struct AgentxxPluginSessionIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_SESSION_VERSION
    uint32_t _reserved;

    /// 读取会话级 share_store 条目 (仅 io 线程); 返回 0 成功, out 接收数据 (host->alloc)
    int32_t(AGENTXX_PLUGIN_CALL* get_share_store)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* session_id,
        int64_t                        id,
        AgentxxPluginString*           out
    );
    void(AGENTXX_PLUGIN_CALL* emit_message_tip)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* session_id,
        const AgentxxPluginStringView* text,
        int32_t                        level
    );
    int64_t(AGENTXX_PLUGIN_CALL* add_share_store)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* session_id,
        const AgentxxPluginStringView* content
    );
} AgentxxPluginSessionIface;

/* ==================== 接口表: 插件互查 (agentxx.agent.plugins) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS         "agentxx.agent.plugins"
#define AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS_VERSION 1

typedef struct AgentxxPluginsIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS_VERSION
    uint32_t _reserved;

    int32_t(AGENTXX_PLUGIN_CALL* list_plugins)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
    int32_t(AGENTXX_PLUGIN_CALL* get_plugin)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* name,
        AgentxxPluginString*           out
    );
    int32_t(AGENTXX_PLUGIN_CALL* get_own_info)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
} AgentxxPluginsIface;

/* ==================== 接口表: 宿主配置 (agentxx.agent.config) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_CONFIG         "agentxx.agent.config"
#define AGENTXX_PLUGIN_IFACE_AGENT_CONFIG_VERSION 1

typedef struct AgentxxPluginConfigIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_CONFIG_VERSION
    uint32_t _reserved;

    /// 宿主 AgentConfig 关键字段 JSON (io 线程; host->alloc):
    /// {"dataDir": "...", "projectRoot": "..."(可为空), "platform":
    /// "windows"|"linux"|"macos"|"android"|"ios"}
    int32_t(AGENTXX_PLUGIN_CALL* get_config)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
    /// 本插件配置参数 JSON (yaml `plugins` 条目 args; io 线程; host->alloc):
    int32_t(AGENTXX_PLUGIN_CALL* get_plugin_args)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
    /// 宿主 toolPrompt 配置 (io 线程; host->alloc):
    /// {"depict": "...", "args": {"参数名": "参数说明", ...}}
    int32_t(AGENTXX_PLUGIN_CALL* get_tool_prompt)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* tool_name,
        AgentxxPluginString*           out
    );
    /// 指定会话生效的工作目录 (io 线程; host->alloc; 失败/未装配返回空串):
    /// - session_id 非空: worktree 绑定优先, 依次回退会话覆写 / AgentConfig
    /// - session_id 为空: 返回解析后的默认会话工作目录
    int32_t(AGENTXX_PLUGIN_CALL* get_session_work_dir)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* session_id,
        AgentxxPluginString*           out
    );
    /// 本插件配置文件所在目录或文件路径 (yaml `plugins` 条目 config; io 线程;
    /// host->alloc; 未指定返回空串, 空串表示未配置)
    /// - 可指向文件或目录 (由插件自行判断类型并加载)
    /// - 宿主已归一化为绝对路径 (正斜杠, lexically_normal)
    int32_t(AGENTXX_PLUGIN_CALL* get_plugin_config_path)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
} AgentxxPluginConfigIface;

/* ==================== 接口表: 主模型配置 (agentxx.agent.model) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_MODEL         "agentxx.agent.model"
#define AGENTXX_PLUGIN_IFACE_AGENT_MODEL_VERSION 1

typedef struct AgentxxPluginModelIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_MODEL_VERSION
    uint32_t _reserved;
    /// 宿主主模型及关联配置 JSON (io 线程; host->alloc; 未装配返回空串):
    int32_t(AGENTXX_PLUGIN_CALL* get_config)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
} AgentxxPluginModelIface;

/* ==================== 接口表: 会话取消状态 (agentxx.agent.cancel) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_CANCEL         "agentxx.agent.cancel"
#define AGENTXX_PLUGIN_IFACE_AGENT_CANCEL_VERSION 1

typedef struct AgentxxPluginCancelIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_CANCEL_VERSION
    uint32_t _reserved;
    /// 查询会话当前轮次是否已取消 (advisory 定位; 权威通知始终是 cancel 回调)

    int32_t(AGENTXX_PLUGIN_CALL* is_cancelled)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* session_id
    );
} AgentxxPluginCancelIface;

/* ==================== 接口表: 宿主提示词读写 (agentxx.agent.prompt) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_PROMPT         "agentxx.agent.prompt"
#define AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_VERSION 1

typedef struct AgentxxPluginPromptIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_VERSION
    uint32_t _reserved;

    int32_t(AGENTXX_PLUGIN_CALL* get_prompt)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
    int32_t(AGENTXX_PLUGIN_CALL* set_prompt)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* prompt_json
    );
} AgentxxPluginPromptIface;

/* ==================== 接口表: JSON 辅助 (agentxx.agent.json) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_JSON         "agentxx.agent.json"
#define AGENTXX_PLUGIN_IFACE_AGENT_JSON_VERSION 1

typedef struct AgentxxPluginJsonIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_JSON_VERSION
    uint32_t _reserved;

    int32_t(AGENTXX_PLUGIN_CALL* json_get_string)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* json,
        const AgentxxPluginStringView* key,
        AgentxxPluginString*           out
    );
    int32_t(AGENTXX_PLUGIN_CALL* json_escape)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* s,
        AgentxxPluginString*           out
    );
} AgentxxPluginJsonIface;

/* ==================== 接口表: 日志 (agentxx.agent.log) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_LOG         "agentxx.agent.log"
#define AGENTXX_PLUGIN_IFACE_AGENT_LOG_VERSION 1

typedef struct AgentxxPluginLogIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_LOG_VERSION
    uint32_t _reserved;

    void(AGENTXX_PLUGIN_CALL*
             log)(const AgentxxPluginHost* host, int32_t level, const AgentxxPluginStringView* msg);
} AgentxxPluginLogIface;

/* ==================== 接口表: 会话资源贡献 (agentxx.agent.resources) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES         "agentxx.agent.resources"
#define AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_VERSION 1

typedef struct AgentxxPluginResourcesIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_VERSION
    uint32_t _reserved;

    int32_t(AGENTXX_PLUGIN_CALL* register_skill_dir)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* path
    );
    int32_t(AGENTXX_PLUGIN_CALL* unregister_skill_dir)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* path
    );
    int32_t(AGENTXX_PLUGIN_CALL* register_memory_file)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* path
    );
    int32_t(AGENTXX_PLUGIN_CALL* unregister_memory_file)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* path
    );
    int32_t(AGENTXX_PLUGIN_CALL* register_mcp_server)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* spec_json
    );
    int32_t(AGENTXX_PLUGIN_CALL* unregister_mcp_server)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* name_space
    );
    int32_t(AGENTXX_PLUGIN_CALL* get_own_resources)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
} AgentxxPluginResourcesIface;

/* ==================== 接口表: 执行图 (agentxx.agent.graph) ==================== */

/// 插件节点执行函数 (【宿主 io 线程调用】, 非阻塞):
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
typedef void*(AGENTXX_PLUGIN_CALL* AgentxxPluginGraphNodeRunStartFn)(
    void*                              user_data,
    const AgentxxPluginStringView*     node_name,
    const AgentxxPluginStringView*     config_json,
    const AgentxxPluginStringView*     state_json,
    const AgentxxPluginStringView*     thread_id,
    const AgentxxPluginOperatorNotify* notify,
    AgentxxPluginString*               error_out
);

/// 协作式取消请求 (io 线程, 非阻塞; 不可取消可留 NULL)
typedef void(AGENTXX_PLUGIN_CALL* AgentxxPluginGraphNodeRunCancelFn)(void* user_data, void* op);

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
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_GRAPH_VERSION
    uint32_t _reserved;

    /// 注册节点类型 (io 线程约束, 非 io 线程由宿主投递同步等待)
    /// `return`: 类型名冲突返回非 0
    int32_t(AGENTXX_PLUGIN_CALL* register_node_type)(
        const AgentxxPluginHost*              host,
        const AgentxxPluginGraphNodeTypeSpec* spec
    );
    /// 注销节点类型 (按类型名; 卸载时宿主自动清理)
    /// `return`: 不存在返回非 0
    int32_t(AGENTXX_PLUGIN_CALL* unregister_node_type)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* type
    );
    /// 获取当前执行图 JSON 定义 (host->alloc; 插件可基于此判断后 set 修改)
    int32_t(AGENTXX_PLUGIN_CALL* get_graph_json)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
    /// 获取当前执行图名称 (host->alloc; 默认 "agentxx.default")
    int32_t(AGENTXX_PLUGIN_CALL* get_graph_name)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
    /// 设置执行图 JSON 定义 (覆盖; 宿主构建 engine 前生效)
    /// `return`: JSON 非法返回非 0 (host 侧解析失败)
    int32_t(AGENTXX_PLUGIN_CALL* set_graph_json)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* graph_json
    );
} AgentxxPluginGraphIface;

/* ==================== 接口表: 后台任务 (agentxx.agent.tasks) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_TASKS         "agentxx.agent.tasks"
#define AGENTXX_PLUGIN_IFACE_AGENT_TASKS_VERSION 1

typedef struct AgentxxPluginTasksIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_TASKS_VERSION
    uint32_t _reserved;

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
    AgentxxPluginOperatorHandle*(AGENTXX_PLUGIN_CALL* register_task)(
        const AgentxxPluginHost*            host,
        AgentxxPluginOperatorCancelFunction cancel_fn,
        void*                               cancel_ud,
        AgentxxPluginOperatorNotify*        notify,
        AgentxxPluginString*                error_out
    );
    /// 取消任务 (幂等; 仅限 io 线程调用, 或宿主内部经 ioCallSync 投递后调用)
    /// - 与宿主 detachAll 内部路径一致; 句柄由宿主托管, 跨线程主动取消需经
    ///   scheduler.post_to_io / ioCallSync 回到 io 线程 (与注册类接口线程
    ///   约束一致), 避免 handle->caller 裸指针跨线程反查实例
    void(AGENTXX_PLUGIN_CALL* cancel_task)(AgentxxPluginOperatorHandle* h);
} AgentxxPluginTasksIface;

/* ==================== 插件入口符号 (dlsym) ==================== */

typedef const AgentxxPluginInfo*(AGENTXX_PLUGIN_CALL* AgentxxPluginGetInfoFn)(void);
typedef int32_t(AGENTXX_PLUGIN_CALL* AgentxxPluginCreateFn)(
    const AgentxxPluginHost* host,
    void**                   plugin_ctx
);
typedef void(AGENTXX_PLUGIN_CALL* AgentxxPluginDestroyFn)(void* plugin_ctx);

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

// 与 BuiltinPluginInfo 同步生成于 plugins/builtin_plugins.cpp
typedef struct AgentxxPluginBuiltinManifest {
    AgentxxPluginStringView name; ///< 插件名
    AgentxxPluginStringView yaml; ///< plugin.yaml 原文 (UTF-8, 静态只读)
} AgentxxPluginBuiltinManifest;

#pragma pack(pop)

/* ==================== 辅助内联函数 ==================== */

static inline AgentxxPluginStringView agentxx_plugin_sv(const char* data, uint64_t size) {
    AgentxxPluginStringView sv;
    sv.data = data;
    sv.size = size;
    return sv;
}

static inline AgentxxPluginStringView agentxx_plugin_sv_cstr(const char* s) {
    AgentxxPluginStringView sv;
    sv.data = s;
    sv.size = s ? (uint64_t)strlen(s) : 0;
    return sv;
}

static inline int32_t agentxx_plugin_sv_empty(const AgentxxPluginStringView* sv) {
    return sv == NULL || sv->data == NULL || sv->size == 0;
}

static inline AgentxxPluginString agentxx_plugin_str_empty(void) {
    AgentxxPluginString s;
    s.data = NULL;
    s.size = 0;
    return s;
}

static inline int32_t agentxx_plugin_string_empty(const AgentxxPluginString* str) {
    return str == NULL || str->data == NULL || str->size == 0;
}

static inline AgentxxPluginStringView agentxx_plugin_string_to_sv(const AgentxxPluginString* str) {
    AgentxxPluginStringView sv;
    if (str) {
        sv.data = str->data;
        sv.size = str->size;
    } else {
        sv.data = NULL;
        sv.size = 0;
    }
    return sv;
}

static inline void
    agentxx_plugin_string_free(const AgentxxPluginHost* host, AgentxxPluginString* str) {
    if (str && str->data) {
        if (host && host->vtable && host->vtable->free) {
            host->vtable->free(str->data);
        }
        str->data = NULL;
        str->size = 0;
    }
}

static inline AgentxxPluginString agentxx_plugin_string_from_sv(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView* sv
) {
    AgentxxPluginString res;
    res.data = NULL;
    res.size = 0;
    if (!host || !host->vtable || !host->vtable->alloc || !sv) {
        return res;
    }
    if (sv->data == NULL && sv->size == 0) {
        return res;
    }
    char* p = (char*)host->vtable->alloc(sv->size + 1);
    if (p) {
        if (sv->size > 0 && sv->data) {
            memcpy(p, sv->data, (size_t)sv->size);
        }
        p[sv->size] = '\0';
        res.data    = p;
        res.size    = sv->size;
    }
    return res;
}

static inline AgentxxPluginString
    agentxx_plugin_string_from_cstr(const AgentxxPluginHost* host, const char* s) {
    AgentxxPluginStringView sv = agentxx_plugin_sv_cstr(s);
    return agentxx_plugin_string_from_sv(host, &sv);
}

static inline char*
    agentxx_plugin_strdup(const AgentxxPluginHost* host, const AgentxxPluginStringView* sv) {
    if (!host || !host->vtable || !host->vtable->alloc || !sv) {
        return NULL;
    }
    char* p = (char*)host->vtable->alloc(sv->size + 1);
    if (p) {
        if (sv->size > 0 && sv->data) {
            memcpy(p, sv->data, (size_t)sv->size);
        }
        p[sv->size] = '\0';
    }
    return p;
}

static inline const void*
    agentxx_plugin_query_interface(const AgentxxPluginHost* host, const char* iid) {
    if (!host || !host->vtable || !host->vtable->query_interface || !iid) {
        return NULL;
    }
    AgentxxPluginStringView sv = agentxx_plugin_sv_cstr(iid);
    return host->vtable->query_interface(host, &sv);
}

#define AGENTXX_PLUGIN_QUERY_IFACE(host, IfaceType, iid_name) \
    ((const IfaceType*)agentxx_plugin_query_interface((host), (iid_name)))

const AgentxxPluginBuiltinInfo* agentxx_plugin_get_builtin_plugins(uint64_t* count);

const AgentxxPluginBuiltinManifest* agentxx_plugin_get_builtin_manifests(uint64_t* count);

#define AGENTXX_PLUGIN_STRDUP(host, s)     (agentxx_plugin_strdup((host), &agentxx_plugin_sv_cstr(s)))
#define AGENTXX_PLUGIN_STRDUP_SV(host, sv) (agentxx_plugin_strdup((host), (sv)))

#ifdef __cplusplus
}
#endif

/* ==================== C++ 便捷重载 (非 ABI; 仅 C++ 侧免 & 取址样板) ==================== */

#ifdef __cplusplus
static inline int32_t agentxx_plugin_sv_empty(const AgentxxPluginStringView& sv) {
    return agentxx_plugin_sv_empty(&sv);
}

static inline int32_t agentxx_plugin_string_empty(const AgentxxPluginString& str) {
    return agentxx_plugin_string_empty(&str);
}

static inline AgentxxPluginStringView agentxx_plugin_string_to_sv(const AgentxxPluginString& str) {
    return agentxx_plugin_string_to_sv(&str);
}

static inline AgentxxPluginString agentxx_plugin_string_from_sv(
    const AgentxxPluginHost*       host,
    const AgentxxPluginStringView& sv
) {
    return agentxx_plugin_string_from_sv(host, &sv);
}

static inline AgentxxPluginString
    agentxx_plugin_string_from_sv(const AgentxxPluginHost* host, const char* s) {
    return agentxx_plugin_string_from_cstr(host, s);
}

static inline char*
    agentxx_plugin_strdup(const AgentxxPluginHost* host, const AgentxxPluginStringView& sv) {
    return agentxx_plugin_strdup(host, &sv);
}

static inline char* agentxx_plugin_strdup(const AgentxxPluginHost* host, const char* s) {
    auto sv = agentxx_plugin_sv_cstr(s);
    return agentxx_plugin_strdup(host, &sv);
}

static inline void
    agentxx_plugin_string_free(const AgentxxPluginHost* host, AgentxxPluginString& str) {
    agentxx_plugin_string_free(host, &str);
}
#endif

#endif /* AGENTXX_PLUGIN_API_H */
