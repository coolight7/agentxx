/// agentxx 插件系统纯 C ABI 契约 (agent 侧领域表 + 通用基座)
///
/// ════════════════════════════════════════════════════════════════════
/// 架构: COM 风格接口表查询
/// ════════════════════════════════════════════════════════════════════
/// - 明确字节对齐: 全部跨边界 ABI 结构体严格遵循 8 字节对齐 (#pragma pack(push, 8))
/// - 明确基本类型: 统一使用定长基本类型 (int32_t, int64_t, uint64_t)
/// - 明确函数调用约定: 接口表函数指针、入口符号与回调全部显式标注 AGENTXX_PLUGIN_CALL
/// - 结构体传递与返回值规范: 入参用指针, 返回值用出参 + int32_t 状态码
/// - 版本策略: 全局 AGENTXX_PLUGIN_API_VERSION + 各表 version/struct_size 自校验
///
/// ════════════════════════════════════════════════════════════════════
/// 归属分层 (框架内核已拆分为 cxx_pluginxx 独立工程)
/// ════════════════════════════════════════════════════════════════════
/// - 与宿主领域无关的基座与通用表: `pluginxx/api/abi.h` / `pluginxx/api/tables.h`
///   (本头已包含, 插件源码只需包含本头即可拿到全部声明)
/// - **本头只声明 agent 领域表**: 工具 (agentxx.agent.tools)、工具权限声明
///   (agentxx.agent.permission)、中间件钩子 (agentxx.agent.hooks)、会话访问
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

#pragma pack(push, 8)


/* ==================== 工具定义 ==================== */

#define AGENTXX_PLUGIN_TOOL_FLAG_NONE         0
#define AGENTXX_PLUGIN_TOOL_FLAG_AUTO_SUMMARY (1 << 0) ///< 输出超限时自动压缩 (经 share_store 卸载)

typedef struct AgentxxPluginToolSpec {
    AgentxxPluginStringView name; ///< 须全局唯一 (与内置工具/MCP 工具同名将注册失败)
    AgentxxPluginStringView description;
    AgentxxPluginStringView parameters_json; ///< JSON Schema 字符串 (json object)

    /// 启动执行 (【宿主 io 线程调用】, 非阻塞; 操作契约):
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

/* ==================== 接口表: 工具 (agentxx.agent.tools) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_TOOLS         "agentxx.agent.tools"
#define AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION 1

typedef struct AgentxxPluginToolsIface {
    int32_t  version;     ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION
    uint32_t struct_size; ///< sizeof(AgentxxPluginToolsIface) or a larger known table

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
    AgentxxPluginStringView tool_name;
    /// 权限作用域 (AGENTXX_PLUGIN_PERMISSION_SCOPE_*)
    int32_t scope;
    /// 权限目标来源 (AGENTXX_PLUGIN_PERMISSION_TARGET_*)
    int32_t target_kind;
    /// 目标参数名 (工具 args JSON 中的字段名; TARGET_NONE 时可留空)
    /// - 目标值按**参数实际 JSON 类型**处理: 字符串视为单个目标, 数组则逐项判定
    ///   (无需声明形态)
    AgentxxPluginStringView target_arg;
    /// 本结构体字节数 (sizeof(AgentxxPluginToolPermissionSpec)); 传 0 时按当前布局解析
    uint32_t struct_size;
    uint32_t _reserved; ///< 8 字节补齐
    /// 权限分类文本 (权限询问卡片上显示; 留空则按作用域生成)
    AgentxxPluginStringView category;
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
    AgentxxPluginStringView session_id;
    /// 待查路径数组 (只读借用, 仅本次调用有效): 绝对路径优先; 相对路径按会话工作目录解析
    const AgentxxPluginStringView* paths;
} AgentxxPluginPermissionPathQuery;

typedef struct AgentxxPluginPermissionIface {
    int32_t  version;     ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION_VERSION
    uint32_t struct_size; ///< sizeof(AgentxxPluginPermissionIface) or a larger known table

    /// 声明工具权限限制 (工具注册后调用; 同工具重复声明为覆盖)
    /// - 未声明的工具不参与权限判定 (直接放行); 声明后由宿主按权限规则
    ///   (白/黑名单、permission.mode、记住的选择、工作区隔离) 统一判定
    /// - 权限声明属于附加能力: 宿主未装配权限中间件时返回非 0, 插件可忽略
    /// `return`: 0 成功, 非 0 不支持或失败
    int32_t(AGENTXX_PLUGIN_CALL* register_tool_permission)(
        const AgentxxPluginHost*               host,
        const AgentxxPluginToolPermissionSpec* spec
    );
    /// 撤销工具权限声明 (工具注销/插件禁用/卸载时由宿主自动撤销, 一般无需手动调用)
    /// `return`: 0 成功, 非 0 不存在
    int32_t(AGENTXX_PLUGIN_CALL* unregister_tool_permission)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* tool_name
    );

    /// 批量查询路径权限判定 (advisory; 只读已生效规则)
    /// - **不发起权限询问、不产生中断、不弹任何界面、不做阻塞等待**: 纯只读判定,
    ///   用于支持模式/前缀参数的插件工具 (如 glob/grep) 在枚举出实际路径后逐项过滤
    /// - 判定口径与工具调用权限检查一致 (工作区隔离 → 配置拒绝 → 完全授权 →
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
    int32_t(AGENTXX_PLUGIN_CALL* check_paths)(
        const AgentxxPluginHost*                host,
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

    int32_t(AGENTXX_PLUGIN_CALL* register_hook)(
        const AgentxxPluginHost*     host,
        const AgentxxPluginHookSpec* spec
    );
    int32_t(AGENTXX_PLUGIN_CALL* unregister_hook)(const AgentxxPluginHost* host, int32_t point);
} AgentxxPluginHooksIface;

/* ==================== 接口表: 会话访问 (agentxx.agent.session) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_SESSION         "agentxx.agent.session"
#define AGENTXX_PLUGIN_IFACE_AGENT_SESSION_VERSION 1

typedef struct AgentxxPluginSessionIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_SESSION_VERSION
    uint32_t struct_size;

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

/* ==================== 接口表: 主模型配置 (agentxx.agent.model) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_MODEL         "agentxx.agent.model"
#define AGENTXX_PLUGIN_IFACE_AGENT_MODEL_VERSION 1

typedef struct AgentxxPluginModelIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_MODEL_VERSION
    uint32_t struct_size;
    /// 宿主主模型及关联配置 JSON (io 线程; host->alloc; 未装配返回空串):
    int32_t(AGENTXX_PLUGIN_CALL* get_config)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
} AgentxxPluginModelIface;

/* ==================== 接口表: 宿主提示词读写 (agentxx.agent.prompt) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_PROMPT         "agentxx.agent.prompt"
#define AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_VERSION 1

typedef struct AgentxxPluginPromptIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_VERSION
    uint32_t struct_size;

    int32_t(AGENTXX_PLUGIN_CALL* get_prompt)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
    int32_t(AGENTXX_PLUGIN_CALL* set_prompt)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* prompt_json
    );
} AgentxxPluginPromptIface;

/* ==================== 接口表: 会话资源贡献 (agentxx.agent.resources) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES         "agentxx.agent.resources"
#define AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_VERSION 1

typedef struct AgentxxPluginResourcesIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_VERSION
    uint32_t struct_size;

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
    AgentxxPluginGraphNodeRunStartFn  run_start;  ///< 节点执行 (操作契约)
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
    uint32_t struct_size;

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

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* AGENTXX_PLUGIN_API_H */
