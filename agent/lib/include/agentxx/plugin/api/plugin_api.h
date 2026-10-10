/// agentxx 插件系统纯 C ABI 契约 (agent 侧领域表 + 通用基座)
///
/// ════════════════════════════════════════════════════════════════════
/// 架构: COM 风格接口表查询
/// ════════════════════════════════════════════════════════════════════
/// - 明确字节对齐: 全部跨边界 ABI 结构体严格遵循 8 字节对齐 (#pragma pack(push, 8))
/// - 明确基本类型: 统一使用定长基本类型 (int32_t, int64_t, uint64_t)
/// - 明确函数调用约定: 接口表函数指针、入口符号与回调全部显式标注 PLUGINXX_CALL
/// - 结构体传递与返回值规范: 入参用指针, 返回值用出参 + int32_t 状态码
/// - 版本策略: 全局 PLUGINXX_API_VERSION + 各表 version/struct_size 自校验
///
/// ════════════════════════════════════════════════════════════════════
/// 归属分层 (框架内核已拆分为 cxx_pluginxx 独立工程)
/// ════════════════════════════════════════════════════════════════════
/// - 与宿主领域无关的基座与通用表: `pluginxx/api/abi.h` / `pluginxx/api/tables.h`
///   (本头已包含, 插件源码只需包含本头即可拿到全部声明)
/// - **本头只声明 agent 领域表**: 工具 (agentxx.agent.tools)、工具权限声明
///   (agentxx.agent.permission)、中间件钩子 (agentxx.agent.hooks) 与钩子有序登记
///   (agentxx.agent.hooks_ex)、会话访问
///   (agentxx.agent.session)、主模型配置 (agentxx.agent.model)、宿主提示词读写
///   (agentxx.agent.prompt)、会话资源贡献 (agentxx.agent.resources)、执行图
///   (agentxx.agent.graph)
/// - client 侧领域表见 `agentxx/plugin/api/client_plugin_api.h`
#ifndef AGENTXX_PLUGIN_API_H
#define AGENTXX_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pluginxx/api/abi.h"
#include "pluginxx/api/tables.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 插件入口符号名 (宿主自定义, 内核不再提供) ==================== */
/* 内核只定义"宿主怎么交出符号名"的接缝 (pluginxx/api/entry.h); 符号名属于宿主命名空间。 */
#define AGENTXX_PLUGIN_AGENT_SYMBOL_GET_INFO "agentxx_plugin_agent_get_info"
#define AGENTXX_PLUGIN_AGENT_SYMBOL_CREATE   "agentxx_plugin_agent_create"
#define AGENTXX_PLUGIN_AGENT_SYMBOL_START    "agentxx_plugin_agent_start"
#define AGENTXX_PLUGIN_AGENT_SYMBOL_STOP     "agentxx_plugin_agent_stop"
#define AGENTXX_PLUGIN_AGENT_SYMBOL_DESTROY  "agentxx_plugin_agent_destroy"

#pragma pack(push, 8)

/* ==================== 工具定义 ==================== */

#define AGENTXX_PLUGIN_TOOL_FLAG_NONE         0
#define AGENTXX_PLUGIN_TOOL_FLAG_AUTO_SUMMARY (1 << 0) ///< 输出超限时自动压缩 (经 share_store 卸载)
/// 该工具的执行体可与其他声明了本标志的工具**并发执行**
/// - 只应给"只读、不写会话、不改插件内部共享状态"的工具置位; 未置位=独占
///   (写文件、命令执行、交互询问类工具必须保持独占)
/// - 并发只发生在同一条 assistant 消息声明的工具调用之间, 结果仍按声明顺序写回
/// - 宿主侧并发上限见 [AgentConfig::toolParallelMaxConcurrency]
#define AGENTXX_PLUGIN_TOOL_FLAG_PARALLEL_SAFE (1 << 1)
/// 连续相同调用重复检查 (与内置工具 [agentxx::tools::XXToolBase::repeatCallCheck] 同语义)
/// - 置位: 同一 llm <-> tool 交替链内 (无用户消息打断) 连续多次 (阈值
///   [AgentConfig::toolcallRepeatCheckThreshold], 默认 5 次) 相同工具 + 相同参数
///   调用时, 宿主经用户确认通道询问, 确认后才继续执行, 拒绝则中止本次调用
/// - 未置位 = 关闭 (默认): 只给"模型可能以同一参数反复调用"的工具置位
#define AGENTXX_PLUGIN_TOOL_FLAG_REPEAT_CALL_CHECK (1 << 2)

typedef struct AgentxxPluginToolSpec {
    PluginxxStringView name; ///< 须全局唯一 (与内置工具/MCP 工具同名将注册失败)
    PluginxxStringView description;
    PluginxxStringView parameters_json; ///< JSON Schema 字符串 (json object)

    /// 启动执行 (【宿主 io 线程调用】, 非阻塞; 操作契约):
    /// - 入参均为指针传递 (只读借用, 仅本次调用有效)
    /// - 快同步工具: 算完 → notify->done(PLUGINXX_OPERATOR_OK, &res_sv) → 返回 NULL
    /// - 锚定协程/自管异步: 创建/挂起任务 → 返回 op 句柄
    /// - 失败: 返回 NULL 且 *error_out 输出错误 (跨边界堆分配字符串, host->alloc 分配)
    void*(PLUGINXX_CALL* execute_start)(
        void*                         user_data,
        const PluginxxStringView*     args_json,
        const PluginxxStringView*     session_id,
        const PluginxxStringView*     tool_call_id,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               error_out
    );
    /// 协作式取消请求 (io 线程, 非阻塞; 不可取消可留 NULL)
    void(PLUGINXX_CALL* execute_cancel)(void* user_data, void* op);

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

typedef struct AgentxxPluginHookSpec {
    int32_t  point;     ///< AgentxxPluginHookPoint (明确 32 位整型)
    uint32_t _reserved; ///< 8 字节补齐
    void*(PLUGINXX_CALL* hook_start)(
        void*                         user_data,
        int32_t                       point,
        const PluginxxStringView*     node_input_json,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               error_out
    );
    void(PLUGINXX_CALL* hook_cancel)(void* user_data, void* op); ///< 可为 NULL
    void* user_data;
} AgentxxPluginHookSpec;

/// 钩子处理器登记选项 (有序登记: 同一实例同一个点可以登记多个处理器)
///
/// 与 [AgentxxPluginHookSpec] 的区别: 多了层内顺序 ([priority]) 与展示信息
/// ([owner_tag] / [depict]), 且**不覆盖**同点已有处理器 —— 每次登记都得到一个
/// 独立句柄, 经 [AgentxxPluginHooksExIface::unregister_hook_ex] 精确撤销。
typedef struct AgentxxPluginHookSpecEx {
    uint32_t struct_size; ///< sizeof(AgentxxPluginHookSpecEx); 0 = 按当前布局
    int32_t  point;       ///< AgentxxPluginHookPoint
    /// 层内顺序, 小者先 (默认 0 = 按登记顺序 = 插件装载顺序)
    /// - 越界裁剪到宿主允许范围并记一条警告, 不拒绝登记
    int32_t priority;
    int32_t flags; ///< 预留 (本轮无定义标志, 传 0)
    /// 展示归属标签 (可空; 清单里显示, 便于看出"这是谁的处理器")
    PluginxxStringView owner_tag;
    /// 一句话说明 (可空; 清单里显示)
    PluginxxStringView depict;

    /// 执行体 (语义与 [AgentxxPluginHookSpec::hook_start] 完全一致):
    /// - 宿主 io 线程调用; 结果被丢弃 (钩子只用于通知)
    /// - 快同步: 算完 → notify->done(PLUGINXX_OPERATOR_OK, NULL) → 返回 NULL
    /// - 锚定协程/自管异步: 创建或挂起任务 → 返回 op 句柄
    void*(PLUGINXX_CALL* hook_start)(
        void*                         user_data,
        int32_t                       point,
        const PluginxxStringView*     node_input_json,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               error_out
    );
    void(PLUGINXX_CALL* hook_cancel)(void* user_data, void* op); ///< 可为 NULL
    void* user_data;
} AgentxxPluginHookSpecEx;

/* ==================== 接口表: 工具 (agentxx.agent.tools) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_TOOLS         "agentxx.agent.tools"
#define AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION 1

typedef struct AgentxxPluginToolsIface {
    int32_t  version;     ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION
    uint32_t struct_size; ///< sizeof(AgentxxPluginToolsIface) or a larger known table

    /// 注册工具 (io 线程约束, 非 io 线程由宿主投递同步等待)
    /// `return`: 0 成功, 非 0 冲突或失败
    int32_t(PLUGINXX_CALL* register_tool)(
        const PluginxxHost*          host,
        const AgentxxPluginToolSpec* spec
    );
    /// 注销工具 (按名称)
    /// `return`: 0 成功, 非 0 不存在
    int32_t(PLUGINXX_CALL* unregister_tool)(
        const PluginxxHost*       host,
        const PluginxxStringView* name
    );

    /* ---- 插件互调: 完成回调形 ---- */
    PluginxxOperatorHandle*(PLUGINXX_CALL* call_tool_async)(
        const PluginxxHost*       host,
        const PluginxxStringView* name,
        const PluginxxStringView* args_json,
        const PluginxxStringView* session_id,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
    void(PLUGINXX_CALL* op_cancel)(PluginxxOperatorHandle* op);
} AgentxxPluginToolsIface;

/* ==================== 接口表: 工具权限声明 (agentxx.agent.permission) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION         "agentxx.agent.permission"
#define AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION_VERSION 1

/// 权限作用域 (读/写各自一套规则表, 互不影响)
#define AGENTXX_PLUGIN_PERMISSION_SCOPE_READ  1
#define AGENTXX_PLUGIN_PERMISSION_SCOPE_WRITE 2

/// 权限目标来源
#define AGENTXX_PLUGIN_PERMISSION_TARGET_NONE 0 ///< 无目标 (仅工具级判定)
#define AGENTXX_PLUGIN_PERMISSION_TARGET_PATH 1 ///< 参数值为路径 (按会话工作目录规范化后匹配规则)
#define AGENTXX_PLUGIN_PERMISSION_TARGET_TEXT 2 ///< 参数值为普通文本 (如命令/网址; 原样匹配规则)

/// 路径权限判定结果 (check_paths 出参; 三态)
#define AGENTXX_PLUGIN_PERMISSION_DECISION_DENY  0 ///< 已明确拒绝 (黑名单/记住的拒绝/写边界)
#define AGENTXX_PLUGIN_PERMISSION_DECISION_ALLOW 1 ///< 已明确允许 (白名单/工作目录/完全授权等)
#define AGENTXX_PLUGIN_PERMISSION_DECISION_ASK   2 ///< 未获批准 (本查询不发起询问)

/// 工具权限声明 (插件在注册工具后为自身工具声明权限限制; 由宿主统一判定)
typedef struct AgentxxPluginToolPermissionSpec {
    /// 目标工具名 (须为本实例已注册的工具)
    PluginxxStringView tool_name;
    /// 权限作用域 (AGENTXX_PLUGIN_PERMISSION_SCOPE_*)
    int32_t scope;
    /// 权限目标来源 (AGENTXX_PLUGIN_PERMISSION_TARGET_*)
    int32_t target_kind;
    /// 目标参数名 (工具 args JSON 中的字段名; TARGET_NONE 时可留空)
    /// - 目标值按**参数实际 JSON 类型**处理: 字符串视为单个目标, 数组则逐项判定
    ///   (无需声明形态)
    PluginxxStringView target_arg;
    /// 本结构体字节数 (sizeof(AgentxxPluginToolPermissionSpec)); 传 0 时按当前布局解析
    uint32_t struct_size;
    uint32_t _reserved; ///< 8 字节补齐
    /// 权限分类文本 (权限询问卡片上显示; 留空则按作用域生成)
    PluginxxStringView category;
} AgentxxPluginToolPermissionSpec;

/// 路径权限批量查询入参 (check_paths; 三态判定, 不发起询问)
typedef struct AgentxxPluginPermissionPathQuery {
    /// 本结构体字节数 (sizeof(AgentxxPluginPermissionPathQuery)); 传 0 时按当前布局解析
    uint32_t struct_size;
    /// 权限作用域 (AGENTXX_PLUGIN_PERMISSION_SCOPE_READ / _WRITE)
    int32_t scope;
    /// paths 元素个数 (须 > 0 且不超过宿主上限)
    int32_t  path_count;
    uint32_t _reserved; ///< 8 字节补齐
    /// 会话 (解析会话工作目录与工作区隔离边界; 可为空, 为空时按进程工作目录解析)
    PluginxxStringView session_id;
    /// 待查路径数组 (只读借用, 仅本次调用有效): 绝对路径优先; 相对路径按会话工作目录解析
    const PluginxxStringView* paths;
} AgentxxPluginPermissionPathQuery;

typedef struct AgentxxPluginPermissionIface {
    int32_t  version;     ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION_VERSION
    uint32_t struct_size; ///< sizeof(AgentxxPluginPermissionIface) or a larger known table

    /// 声明工具权限限制 (工具注册后调用; 同工具重复声明为覆盖)
    /// - 未声明的工具不参与权限判定 (直接放行); 声明后由宿主按权限规则
    ///   (白/黑名单、permission.mode、记住的选择、工作区隔离) 统一判定
    /// - 权限声明属于附加能力: 宿主未装配权限中间件时返回非 0, 插件可忽略
    /// `return`: 0 成功, 非 0 不支持或失败
    int32_t(PLUGINXX_CALL* register_tool_permission)(
        const PluginxxHost*                    host,
        const AgentxxPluginToolPermissionSpec* spec
    );
    /// 撤销工具权限声明 (工具注销/插件禁用/卸载时由宿主自动撤销, 一般无需手动调用)
    /// `return`: 0 成功, 非 0 不存在
    int32_t(PLUGINXX_CALL* unregister_tool_permission)(
        const PluginxxHost*       host,
        const PluginxxStringView* tool_name
    );

    /// 批量查询路径权限判定 (advisory; 只读已生效规则)
    /// - **不发起权限询问、不产生中断、不弹任何界面、不做阻塞等待**: 纯只读判定,
    ///   用于支持模式/前缀参数的插件工具 (如 glob/grep) 在枚举出实际路径后逐项过滤
    /// - 判定规则与工具调用权限检查一致 (工作区隔离 → 配置拒绝 → 完全授权 →
    ///   规则表 → noRuleOperator); 区别仅在于"应询问(INTERRUPT)"以 ASK 返回而不询问
    /// - 工具侧建议: DENY 丢弃; ASK 表示"未获批准"同样不应访问 (按未批准处理)
    /// - 线程: 任意线程可调用 (非 io 线程由宿主投递到 io 线程同步等待);
    ///   单次批量不宜过大 (宿主在 io 线程执行), 建议按需分批 (如 512 项)
    ///
    /// - `args`:
    ///     - [query] 查询入参 (路径数组/作用域/会话)
    ///     - [out_decisions] 调用方提供的等长出参数组 (path_count 项)
    ///
    /// `return`: 0 成功; 非 0 不支持或失败 (失败时调用方应跳过过滤, 按原行为处理)
    int32_t(PLUGINXX_CALL* check_paths)(
        const PluginxxHost*                     host,
        const AgentxxPluginPermissionPathQuery* query,
        int32_t*                                out_decisions
    );
} AgentxxPluginPermissionIface;

/* ==================== 接口表: 中间件钩子 (agentxx.agent.hooks) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_HOOKS         "agentxx.agent.hooks"
#define AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_VERSION 1

typedef struct AgentxxPluginHooksIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_VERSION
    uint32_t struct_size;

    int32_t(PLUGINXX_CALL* register_hook)(
        const PluginxxHost*          host,
        const AgentxxPluginHookSpec* spec
    );
    int32_t(PLUGINXX_CALL* unregister_hook)(const PluginxxHost* host, int32_t point);
} AgentxxPluginHooksIface;

/* ==================== 接口表: 钩子有序登记与清单 (agentxx.agent.hooks_ex) ==================== */

/// 钩子注册表的扩展项 (基础两项见 [AgentxxPluginHooksIface])
///
/// 钩子 (7 个固定点) 是"核心在某一步通知所有关心的人"的机制: 载荷仍是
/// `{sessionId, point}`、结果仍被丢弃、仍是 7 个点。本表只补两件"能读、能排"的事:
/// 1. **有序登记**: 同一实例在同一个点可以登记任意多个处理器 (基础表一个点只能一个),
///    每个处理器给一个句柄, 顺序 = `plugin` 层 (插件 / FFI 宿主) 按 (priority 升序,
///    登记序号), 其后是 `core` 层;
/// 2. **清单**: 读出"这个点上有哪些处理器、谁的、什么顺序"。
///
/// 登记语义:
/// - 不声明 `priority` (= 0) 时顺序就是登记顺序 (插件装载顺序) —— 与基础登记一致;
/// - 处理器数量不设上限 (一个点可以登记任意多个);
/// - 处理器跟着实例走: 插件禁用 → 不生效 (派发时跳过), 卸载 → 全部摘除,
///   重新启用时由插件 `start` 重新登记。
///
/// 为什么单列一张表而不是在 [AgentxxPluginHooksIface] 表尾追加成员: 表尾追加会让
/// 新插件在老宿主上**整表校验失败** (SDK 校验为 `struct_size >= sizeof(表结构)`),
/// 连基础的 `register_hook` 一起丢掉。新能力一律走新接口表 (见
/// docs/zh-cn/design/plugins.md §9), 插件应把本表当可选能力: 查询不到时降级用基础两项。
#define AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_EX         "agentxx.agent.hooks_ex"
#define AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_EX_VERSION 1

typedef struct AgentxxPluginHooksExIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_EX_VERSION
    uint32_t struct_size;

    /// 登记一个钩子处理器 (同一实例同一个点可登记任意多个, 各自句柄)
    /// - `priority` 越界裁剪到上下限并记一条警告 (不拒绝登记);
    /// - 拒绝的情形只有: `point` 越界 / `hook_start` 为空 / 实例正在关闭或已禁用
    ///
    /// `return`: 0 成功 (`*out_handle` 收到句柄, 之后凭它精确撤销); 非 0 失败
    int32_t(PLUGINXX_CALL* register_hook_ex)(
        const PluginxxHost*            host,
        const AgentxxPluginHookSpecEx* spec,
        int64_t*                       out_handle
    );
    /// 撤销本实例登记的一个处理器 (按句柄; 别人的句柄会被拒绝)
    ///
    /// `return`: 0 成功; 非 0 失败 (句柄不存在 / 不属于本实例)
    int32_t(PLUGINXX_CALL* unregister_hook_ex)(const PluginxxHost* host, int64_t handle);
    /// 处理器清单 JSON (host->alloc; 形状见 docs/zh-cn/design/plugins.md §8)
    int32_t(PLUGINXX_CALL* list_hooks)(const PluginxxHost* host, PluginxxString* out_json);
} AgentxxPluginHooksExIface;

/* ==================== 接口表: 会话访问 (agentxx.agent.session) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_SESSION         "agentxx.agent.session"
#define AGENTXX_PLUGIN_IFACE_AGENT_SESSION_VERSION 1

typedef struct AgentxxPluginSessionIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_SESSION_VERSION
    uint32_t struct_size;

    /// 读取会话级 share_store 条目 (仅 io 线程); 返回 0 成功, out 接收数据 (host->alloc)
    /// - 条目不存在或 id 未分配 (大于自增 id, 见 add_share_store) 时返回 -1
    ///   (out 不分配内存, 宿主不抛异常)
    int32_t(PLUGINXX_CALL* get_share_store)(
        const PluginxxHost*       host,
        const PluginxxStringView* session_id,
        int64_t                   id,
        PluginxxString*           out
    );
    void(PLUGINXX_CALL* emit_message_tip)(
        const PluginxxHost*       host,
        const PluginxxStringView* session_id,
        const PluginxxStringView* text,
        int32_t                   level
    );
    int64_t(PLUGINXX_CALL* add_share_store)(
        const PluginxxHost*       host,
        const PluginxxStringView* session_id,
        const PluginxxStringView* content
    );
} AgentxxPluginSessionIface;

/* ==================== 接口表: 会话上下文查询 (agentxx.agent.context) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT         "agentxx.agent.context"
#define AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT_VERSION 1

/// 会话 LLM 上下文查询
///
/// 背景: LLM 上下文由会话持有 (唯一权威), 图状态不再包含 `messages` 通道;
/// 插件需要上下文内容时经本表查询, 图状态里的 `xx_messagesMeta` 通道只给
/// 条数/版本/末尾消息摘要等轻量信息 (与上下文大小无关)。
typedef struct AgentxxPluginContextIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT_VERSION
    uint32_t struct_size;

    /// 读取会话 LLM 上下文 (JSON 数组文本; 宿主 alloc, 需 host->vtable->free 释放)
    /// - 返回 0 成功 (out 为 ChatMessage JSON 数组文本), -1 失败/会话不存在
    int32_t(PLUGINXX_CALL* get_messages)(
        const PluginxxHost*       host,
        const PluginxxStringView* session_id,
        PluginxxString*           out
    );

    /// 会话 LLM 上下文条数 (会话不存在返回 -1)
    /// - 仅取数量时用它, 避免为读一个数字拉取整段上下文
    int64_t(PLUGINXX_CALL* messages_count)(
        const PluginxxHost*       host,
        const PluginxxStringView* session_id
    );
} AgentxxPluginContextIface;

/* ==================== 接口表: 主模型配置 (agentxx.agent.model) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_MODEL         "agentxx.agent.model"
#define AGENTXX_PLUGIN_IFACE_AGENT_MODEL_VERSION 1

typedef struct AgentxxPluginModelIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_MODEL_VERSION
    uint32_t struct_size;
    /// 宿主主模型及关联配置 JSON (io 线程; host->alloc; 未装配返回空串):
    int32_t(PLUGINXX_CALL* get_config)(const PluginxxHost* host, PluginxxString* out);
} AgentxxPluginModelIface;

/* ==================== 接口表: 宿主提示词读写 (agentxx.agent.prompt) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_PROMPT         "agentxx.agent.prompt"
#define AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_VERSION 1

typedef struct AgentxxPluginPromptIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_VERSION
    uint32_t struct_size;

    int32_t(PLUGINXX_CALL* get_prompt)(const PluginxxHost* host, PluginxxString* out);
    int32_t(PLUGINXX_CALL* set_prompt)(
        const PluginxxHost*       host,
        const PluginxxStringView* prompt_json
    );
} AgentxxPluginPromptIface;

/* ==================== 接口表: 会话资源贡献 (agentxx.agent.resources) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES         "agentxx.agent.resources"
#define AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_VERSION 1

typedef struct AgentxxPluginResourcesIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_VERSION
    uint32_t struct_size;

    int32_t(PLUGINXX_CALL* register_skill_dir)(
        const PluginxxHost*       host,
        const PluginxxStringView* path
    );
    int32_t(PLUGINXX_CALL* unregister_skill_dir)(
        const PluginxxHost*       host,
        const PluginxxStringView* path
    );
    int32_t(PLUGINXX_CALL* register_memory_file)(
        const PluginxxHost*       host,
        const PluginxxStringView* path
    );
    int32_t(PLUGINXX_CALL* unregister_memory_file)(
        const PluginxxHost*       host,
        const PluginxxStringView* path
    );
    int32_t(PLUGINXX_CALL* register_mcp_server)(
        const PluginxxHost*       host,
        const PluginxxStringView* spec_json
    );
    int32_t(PLUGINXX_CALL* unregister_mcp_server)(
        const PluginxxHost*       host,
        const PluginxxStringView* name_space
    );
    int32_t(PLUGINXX_CALL* get_own_resources)(const PluginxxHost* host, PluginxxString* out);
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
typedef void*(PLUGINXX_CALL* AgentxxPluginGraphNodeRunStartFn)(
    void*                         user_data,
    const PluginxxStringView*     node_name,
    const PluginxxStringView*     config_json,
    const PluginxxStringView*     state_json,
    const PluginxxStringView*     thread_id,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out
);

/// 协作式取消请求 (io 线程, 非阻塞; 不可取消可留 NULL)
typedef void(PLUGINXX_CALL* AgentxxPluginGraphNodeRunCancelFn)(void* user_data, void* op);

/// 插件节点类型注册规格
typedef struct AgentxxPluginGraphNodeTypeSpec {
    PluginxxStringView                type;       ///< 节点类型名 (须全局唯一)
    AgentxxPluginGraphNodeRunStartFn  run_start;  ///< 节点执行 (操作契约)
    AgentxxPluginGraphNodeRunCancelFn run_cancel; ///< 可空
    void*                             user_data;  ///< 透传给 run_start/run_cancel
    /// 可选节点 config JSON Schema (Draft 2020-12 片段; 仅供导出/文档, 引擎不校验)
    PluginxxStringView config_schema_json;
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
    uint32_t struct_size;

    /// 注册节点类型 (io 线程约束, 非 io 线程由宿主投递同步等待)
    /// `return`: 类型名冲突返回非 0
    int32_t(PLUGINXX_CALL* register_node_type)(
        const PluginxxHost*                   host,
        const AgentxxPluginGraphNodeTypeSpec* spec
    );
    /// 注销节点类型 (按类型名; 卸载时宿主自动清理)
    /// `return`: 不存在返回非 0
    int32_t(PLUGINXX_CALL* unregister_node_type)(
        const PluginxxHost*       host,
        const PluginxxStringView* type
    );
    /// 获取当前执行图 JSON 定义 (host->alloc; 插件可基于此判断后 set 修改)
    int32_t(PLUGINXX_CALL* get_graph_json)(const PluginxxHost* host, PluginxxString* out);
    /// 获取当前执行图名称 (host->alloc; 默认 "agentxx.default")
    int32_t(PLUGINXX_CALL* get_graph_name)(const PluginxxHost* host, PluginxxString* out);
    /// 设置执行图 JSON 定义 (覆盖; 宿主构建 engine 前生效)
    /// `return`: JSON 非法返回非 0 (host 侧解析失败)
    int32_t(PLUGINXX_CALL* set_graph_json)(
        const PluginxxHost*       host,
        const PluginxxStringView* graph_json
    );
} AgentxxPluginGraphIface;

/* ==================== 接口表: 功能点 (agentxx.agent.feature) ==================== */

/// 功能点 (feature point) —— 核心在关键位置主动调用的扩展点
///
/// 三层: 点 (Point) / 实现 (Impl) / 调用 (Call)。插件可以:
/// 1. 为**任何已声明的点**登记实现 ([AgentxxPluginFeatureImplSpec]): 应用点由宿主
///    装配期声明, 插件点由插件自己经 define_point 声明;
/// 2. **声明自己的点** ([AgentxxPluginFeaturePointSpec]): id 必须落在
///    `plugin.<本实例插件名>.*`, 声明期间恒可被任何一方调用 (本轮只允许 provide 类型);
/// 3. **调用点** (call_point_async): 只拿数据 —— 不写值缓存、不记置空、不改调用方
///    会话与上下文、不发界面提示、不落盘。
///
/// 实现链顺序: `plugin` 层 (插件 / FFI 宿主) → `core` 层 (libagentxx 自身兜底);
/// 层内按 `(priority 升序, 登记顺序)`, 默认优先级带为插件 `0` / FFI 宿主 `1000`。
/// 每个实现返回 `{"value": ...}` 给出值、`{"disable": true}` 显式置空 (不再问后面的
/// 实现)、`{}` 表示没意见 (继续问下一个)。
///
/// 超时: 点的声明方可以给自己的点设 `impl_timeout_ms` (等单个实现的上限); 实现方
/// 可以自报 `default_timeout_ms`; 宿主取两者中非 0 的较小值, 都为 0 (默认) 表示不限。
#define AGENTXX_PLUGIN_IFACE_AGENT_FEATURE         "agentxx.agent.feature"
#define AGENTXX_PLUGIN_IFACE_AGENT_FEATURE_VERSION 1

/// 功能点类型 (本轮只开放 `PROVIDE`; `DECIDE` 只有宿主留出的点使用)
#define AGENTXX_PLUGIN_FEATURE_TYPE_PROVIDE 0
#define AGENTXX_PLUGIN_FEATURE_TYPE_DECIDE  1

/// 功能点实现自报的耗时上限 (毫秒; 0 = 不限); 与点的 `impl_timeout_ms` 取较小非 0 值
/// (字段语义与 [AgentxxPluginToolSpec::default_timeout_ms] 一致)

/// 插件声明自己的功能点
typedef struct AgentxxPluginFeaturePointSpec {
    /// 本结构体字节数 (sizeof(AgentxxPluginFeaturePointSpec)); 传 0 时按当前布局解析
    uint32_t struct_size;
    /// 类型 (AGENTXX_PLUGIN_FEATURE_TYPE_*)
    int32_t type;
    /// 等实现方的超时 (毫秒; 0 = 不限, 默认): 到点取消当次实现并按"没意见"继续链
    int32_t impl_timeout_ms;
    int32_t _reserved; ///< 8 字节补齐
    /// 点 id, 必须为 `plugin.<本实例插件名>.<名字>` (不含空白字符)
    PluginxxStringView id;
    /// 展示名 (可空, 缺省用 id)
    PluginxxStringView title;
    /// 一句话说明
    PluginxxStringView depict;
    /// 参数说明 (文本; 进清单与作者文档)
    PluginxxStringView args_doc;
    /// 结果说明 (文本)
    PluginxxStringView result_doc;
} AgentxxPluginFeaturePointSpec;

/// 功能点实现登记 (与工具 `execute_start` 同一套操作协议)
typedef struct AgentxxPluginFeatureImplSpec {
    /// 本结构体字节数; 传 0 时按当前布局解析
    uint32_t struct_size;
    /// 层内顺序, 小者先 (插件默认 0; 越界裁剪到上下限并记警告, 不拒绝登记)
    int32_t priority;
    /// 自报的最大耗时 (毫秒; 0 = 不限, 默认)
    int32_t default_timeout_ms;
    /// 目标点 id (该点必须已声明)
    PluginxxStringView point_id;
    void*              user_data;
    /// 启动实现 (【宿主 io 线程调用】, 非阻塞; 操作契约):
    /// - `point_id`: 目标点 id (只读借用, 仅本次调用有效)
    /// - `call_json`: 调用上下文 JSON (只读借用), 形状见下:
    ///   `{ "point": "...", "args": {...}, "request": {...}, "caller": "插件名或空",
    ///      "viaCall": true/false, "identity": "..." }`
    /// - `notify`: 完成/失败/取消都经它回到宿主 IO 线程 (必须恰好回调一次)
    /// - 快同步实现: 算完 → notify->done(OK, &answer) → 返回 NULL
    /// - 锚定协程/自管异步: 返回 op 句柄 (宿主用 impl_cancel 请求取消)
    /// - 回答文本: `{"value": ...}` 给出值 / `{"disable": true}` 显式置空 /
    ///   `{}` (或空串) 没意见
    /// - 失败: 返回 NULL 且 *error_out 输出错误 (host->alloc 分配)
    void*(PLUGINXX_CALL* impl_start)(
        void*                         user_data,
        const PluginxxStringView*     point_id,
        const PluginxxStringView*     call_json,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               error_out
    );
    /// 协作式取消请求 (io 线程, 非阻塞; 不可取消可留 NULL)
    void(PLUGINXX_CALL* impl_cancel)(void* user_data, void* op);
} AgentxxPluginFeatureImplSpec;

typedef struct AgentxxPluginFeatureIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_FEATURE_VERSION
    uint32_t struct_size;

    /// 功能点清单 (JSON 文本; 出参经 host->alloc 分配, 失败返回非 0)
    int32_t(PLUGINXX_CALL* list_points)(const PluginxxHost* host, PluginxxString* out_json);

    /// 声明插件自己的功能点
    /// `return`: 0 成功; 非 0 失败 (id 非法 / 类型不支持 / 与宿主点冲突)
    int32_t(PLUGINXX_CALL* define_point)(
        const PluginxxHost*                    host,
        const AgentxxPluginFeaturePointSpec*   spec
    );
    /// 撤销插件自己的点 (连带撤掉这些点上的全部实现; 禁用/卸载时宿主自动处理)
    /// `return`: 0 成功; 非 0 失败 (点不存在 / 不是本实例声明的点)
    int32_t(PLUGINXX_CALL* undefine_point)(
        const PluginxxHost*       host,
        const PluginxxStringView* point_id
    );

    /// 登记实现 (同一 `(点, 实例)` 重复登记 = 覆盖; 数量不设上限)
    /// `return`: 0 成功; 非 0 失败 (点未声明 / 回调为空)
    int32_t(PLUGINXX_CALL* register_impl)(
        const PluginxxHost*                   host,
        const AgentxxPluginFeatureImplSpec*   spec
    );
    /// 撤销实现 (按点 id)
    /// `return`: 0 成功; 非 0 不存在
    int32_t(PLUGINXX_CALL* unregister_impl)(
        const PluginxxHost*       host,
        const PluginxxStringView* point_id
    );

    /// 调用一个功能点 (异步; 完成回调经 notify 在宿主 IO 线程发布)
    /// - `args_json`: 调用方原样给的参数 (JSON; 空串按空对象处理)
    /// - 结果 JSON: `{"ok":true,"id":...,"identity":...,"value":...,"by":...,"fromCache":...,
    ///   "ms":N}` 或 `{"ok":false,"id":...,"error":"not_callable|bad_args|no_impl|
    ///   disabled|busy|failed","message":"..."}`
    /// - 受理失败 (参数非法/宿主不支持) 返回 NULL 并写 error_out; 调用本身的失败经回调回
    PluginxxOperatorHandle*(PLUGINXX_CALL* call_point_async)(
        const PluginxxHost*       host,
        const PluginxxStringView* point_id,
        const PluginxxStringView* args_json,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
    /// 取消一次功能点调用 (协作式; 上一次调用没结束时同一 (点, 调用方) 的重入会被拒绝)
    void(PLUGINXX_CALL* op_cancel)(PluginxxOperatorHandle* op);
} AgentxxPluginFeatureIface;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* AGENTXX_PLUGIN_API_H */
