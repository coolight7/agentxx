/// agentxx 插件系统纯 C ABI 契约 (agent 侧业务表 + 插件框架通用部分)
///
/// ════════════════════════════════════════════════════════════════════
/// 架构: COM 风格接口表查询
/// ════════════════════════════════════════════════════════════════════
/// - 明确字节对齐: 全部跨边界 ABI 结构体严格遵循 8 字节对齐 (#pragma pack(push, 8))
/// - 明确基本类型: 统一使用定长基本类型 (int32_t, int64_t, uint64_t)
/// - 明确函数调用约定: 接口表函数指针、入口符号与回调全部显式标注 PLUGINXX_CALL
/// - 结构体传递与返回值规范: 入参用指针, 返回值用出参 + int32_t 状态码
/// - 版本策略: 全局 PLUGINXX_API_VERSION + 每张表四项齐全 (IID / `struct_size` /
///   `version` - `_VERSION` / `_MIN_VERSION` / `_BASELINE_SIZE`), 判定规则与追加策略
///   见 `pluginxx/api/tables.h` 的文件头说明与 docs/zh-cn/design/plugins.md §9
/// - 跨边界结构体 (spec): 填充方填 `struct_size`, 读取方按它 gate (见
///   `agentxx/plugin/api/plugin_spec.h`); 新增可选字段只能加在结构体尾部
///
/// ════════════════════════════════════════════════════════════════════
/// 归属分层 (插件框架已拆分为 cxx_pluginxx 独立工程)
/// ════════════════════════════════════════════════════════════════════
/// - 插件框架的通用部分 (与主程序业务无关): `pluginxx/api/abi.h` / `pluginxx/api/tables.h`
///   (本头已包含, 插件源码只需包含本头即可拿到全部声明)
/// - **本头只声明 agent 业务表**: 工具 (agentxx.agent.tools)、工具权限声明
///   (agentxx.agent.permission)、会话访问
///   (agentxx.agent.session)、主模型配置 (agentxx.agent.model)、主程序提示词读写
///   (agentxx.agent.prompt)、会话资源贡献 (agentxx.agent.resources)、执行图
///   (agentxx.agent.graph)、挂载查询 (agentxx.agent.mount)、
///   会话运行时 (agentxx.agent.runtime)
/// - client 侧业务表见 `agentxx/plugin/api/client_plugin_api.h`
///
/// ════════════════════════════════════════════════════════════════════
/// 调用模型: 表成员一律是**异步操作契约**
/// ════════════════════════════════════════════════════════════════════
/// 绝大多数成员形如
/// ```c
/// PluginxxOperatorHandle*(PLUGINXX_CALL* member)(
///     const PluginxxHost*      host,
///     ... 业务参数 ...,
///     PluginxxOperatorCallback cb,        ///< 完成回调, 可传 NULL (不关心结果)
///     void*                    ud,        ///< 原样回传给 cb
///     PluginxxString*          error_out  ///< 未受理时写原因
/// );
/// ```
/// - 返回 `NULL` = **未受理** (参数错 / 作用域失效 / 实例正在关闭), 原因写进
///   `error_out`, 且 `cb` 不会被调用; 写错参数仍然立刻可见, 不用等一次往返;
/// - 返回非 NULL = 已受理, 结果经 `cb` **恰好一次**交回, 回调在主程序 io 线程发出;
///   `payload` 就是旧版出参 `out` 的内容 (JSON / 纯文本 / `1` 与 `0` 等标记), 语义不变;
/// - `status` 取 `PLUGINXX_OPERATOR_OK` / `PLUGINXX_OPERATOR_FAILED` /
///   `PLUGINXX_OPERATOR_CANCELED`; `OK` 时 `payload` 是结果, 其它值时可以是原因;
/// - `cb` 传 `NULL` 时主程序只受理、不回程 (fire-and-forget), 适用于 `show_toast` /
///   `publish_broadcast` / `update_*` / `unregister_*` 一类不关心结果的成员;
/// - 未完成的调用可经 `pluginxx.cancel.op_cancel` 取消 (协作式)。
///
/// 顺序: 同一实例的调用按**受理顺序**执行 (FIFO)。注册类调用必须先 `co_await`
/// (或等回调) 完成再继续,"登记后立刻可见"的语义不变。
///
/// 少数成员保持同步, 只因为它们能在调用方线程当场算完、根本不跨执行体:
/// `agentxx.agent.session.is_canceled` (本地原子标记) 与
/// `pluginxx.scheduler.is_io_thread` (线程本地判定)。
///
/// C++ 侧三种消费形态在 `agentxx/plugin/api/plugin_kit.h`:
/// `co_await tableOp(ctx, ...)` / `tableOpCallback(ctx, ..., done)` /
/// `tableOpSyncWait(...)` (只给必须同步回答外部同步 API 的桥用, 且仅限非 io 线程)。
#ifndef AGENTXX_PLUGIN_API_H
#define AGENTXX_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pluginxx/api/abi.h"
#include "pluginxx/api/tables.h"

#include "agentxx/plugin/api/agentxx_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 插件入口符号名 (主程序自定义, 插件框架不再提供) ==================== */
/* 插件框架只定义"主程序怎么交出符号名"的扩展点 (pluginxx/api/entry.h); 符号名属于主程序命名空间。
 */
#define AGENTXX_PLUGIN_AGENT_SYMBOL_GET_INFO "agentxx_plugin_agent_get_info"
#define AGENTXX_PLUGIN_AGENT_SYMBOL_CREATE   "agentxx_plugin_agent_create"
#define AGENTXX_PLUGIN_AGENT_SYMBOL_START    "agentxx_plugin_agent_start"
#define AGENTXX_PLUGIN_AGENT_SYMBOL_STOP     "agentxx_plugin_agent_stop"
#define AGENTXX_PLUGIN_AGENT_SYMBOL_DESTROY  "agentxx_plugin_agent_destroy"
/// 挂载 / 卸载入口 (每个 agent 一次; 缺失 = 本插件无 agent 侧能力)
#define AGENTXX_PLUGIN_AGENT_SYMBOL_MOUNT   "agentxx_plugin_agent_mount"
#define AGENTXX_PLUGIN_AGENT_SYMBOL_UNMOUNT "agentxx_plugin_agent_unmount"

/// 需要 agent 引用的接口上, 引用缺失/无效/指向已卸载的挂载/序号不匹配
///
/// - 注册类接口返回 `AGENTXX_RESULT_ERR_NO_AGENT`, 且**不产生任何副作用**;
/// - 典型成因: 在 `start` 里注册 (那时还没有任何挂载), 或保存了上一轮挂载的旧引用。
/// - 返回码表见 `agentxx_result.h` (`AGENTXX_RESULT_OK` / `AGENTXX_RESULT_ERR_*`)。

#pragma pack(push, 8)

/* ==================== 挂载参数 (每个 agent 一次) ==================== */

/// 主程序侧 agent 挂载参数
///
/// - 字符串均为**借用视图**, 仅在本次调用期间有效; 需要就自己拷进 per-agent 状态;
/// - `config_json` 是本挂载的配置 (加载表 args 与 `plugin.mount[].args` 的合并结果);
/// - `mount_seq` 每次挂载都不同: 同一 `agent_id` 卸载后重新挂载会拿到新序号,
///   上一轮留在插件里的旧引用自动失效。
typedef struct AgentxxPluginAgentMountSpec {
    /// 本结构体字节数 (`sizeof(AgentxxPluginAgentMountSpec)`); `0` = 基线布局
    uint32_t struct_size;
    uint32_t flags; ///< 预留 (0)
    /// agent 标识 (如 "root" / "sub:3" / "ext:qq:1")
    PluginxxStringView agent_id;
    /// agent 展示名 (AgentConfig::agentName)
    PluginxxStringView agent_name;
    /// 工作目录 (绝对路径, UTF-8)
    PluginxxStringView work_dir;
    /// 语言代码 ("zh-cn" / "en")
    PluginxxStringView language;
    /// 当前模型配置 JSON
    PluginxxStringView model_json;
    /// 本挂载的插件配置 JSON (加载 args ⊕ plugin.mount[].args)
    PluginxxStringView config_json;
    /// 主程序内单调递增的挂载序号 (防 id 复用)
    uint64_t mount_seq;
} AgentxxPluginAgentMountSpec;

/* 基线: 重置时 = 完整布局; 以后追加字段保持不变 (改为 offsetof(第一个追加字段)) */
#define AGENTXX_PLUGIN_AGENT_MOUNT_SPEC_BASELINE_SIZE sizeof(AgentxxPluginAgentMountSpec)

/// 挂载入口 (与 start/stop 同一套操作契约): 同步完成时先 `notify->done(OK)`
/// 再返回 per-agent 状态 (即 mount_ud, 可为 NULL); 失败返回 NULL 并写 `out_error`。
typedef void*(PLUGINXX_CALL* AgentxxPluginAgentMountFn)(
    void*                              plugin_ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    out_error
);

/// 卸载入口: `mount_ud` 就是 mount 的返回值; 只释放该 agent 的状态,
/// 主程序会自动撤销注册 (工具/权限/提示词/能力行/图/资源/事件)。
typedef void*(PLUGINXX_CALL* AgentxxPluginAgentUnmountFn)(
    void*                         plugin_ctx,
    void*                         mount_ud,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               out_error
);

/* ==================== 工具定义 ==================== */

/// 工具结果的两段上报: 正文与元数据分开传, **不要**把两段拼成一段 JSON
///
/// 为什么要分开 (而不是一个 `{text, attachments}` 信封):
/// - 正文是工具的原始输出, 可能是本地代码页的字节 (命令行输出、本地日志),
///   而 JSON 只接受合法 UTF-8 —— 进一次 JSON 序列化, 非法字节就被替换成 U+FFFD,
///   主程序再也无法做编码探测与无损转码;
/// - 正文动辄几百 KB, JSON 转义/反转义是额外的整份内存与 CPU 开销。
///
/// 约定:
/// - `result` = 正文 (对应 `agentxx::util::ToolResult::text`), 原样字节, 允非 UTF-8;
/// - `payload` = 正文以外的结构化元数据 (JSON 对象), 目前是多模态附件
///   `{"attachments":[…]}`; 没有元数据就传 NULL 或空视图, 不产生任何解析开销。

/// 工具完成通知器 (主程序在 `execute_start` 时下发; 工具终结时被调方须【恰好回调一次】)
///
/// - 可从插件的任意线程回调; 主程序内部投递回 io 线程
/// - `status` 取 `PLUGINXX_OPERATOR_OK` / `FAILED` / `CANCELED`;
///   `FAILED` 时正文只写错误内容, 不加 `[Error: …]` 前缀 (主程序统一排版)
/// - 三个字符串视图都是只读借用, 仅在本次回调期间有效
/// - 三选一都能用: 普通工具用 `done`; 插件 SDK 在只有一段可用的位置用
///   `done_text` / `done_frame` (两个都在主程序侧实现, 插件直接挂函数指针即可)
typedef struct AgentxxPluginToolNotify {
    /// 两段完成 (正文 + 元数据) —— 常规用法
    void(PLUGINXX_CALL* done)(
        void*                     host_ud,
        int32_t                   status,
        const PluginxxStringView* result,
        const PluginxxStringView* payload
    );
    /// 只有正文的完成 (元数据为空)
    /// - 签名与 `PluginxxOperatorNotify.done` 完全一致, 因此可以整接插给
    ///   插件框架的协程根 (那里只传一段): 典型用法见 SDK 的 `tool` (Task 型工具)
    void(PLUGINXX_CALL* done_text)(
        void*                     host_ud,
        int32_t                   status,
        const PluginxxStringView* result
    );
    /// 整帧完成: `frame` 是工具 SDK 拼好的两段帧 (见 `agentxx/plugin/tool_result_frame.h`)
    /// - 同样与 `PluginxxOperatorNotify.done` 签名一致, 给受控轮询工具的根直接挂上
    ///   (那个根也只能传一段字符串, 由 SDK 在插件侧把两段拼成帧)
    /// - 普通插件不需要它
    void(PLUGINXX_CALL* done_frame)(
        void*                     host_ud,
        int32_t                   status,
        const PluginxxStringView* frame
    );
    void* host_ud;
} AgentxxPluginToolNotify;

/// 工具调用完成回调 (插件互调 `call_tool` 使用; 两段含义同上)
/// - 主程序保证在**调用方** io 线程派发, 且只派发一次
/// - 字符串视图只读借用, 仅本次回调内有效
typedef void(PLUGINXX_CALL* AgentxxPluginToolCallback)(
    void*                     ud,
    int32_t                   status,
    const PluginxxStringView* result,
    const PluginxxStringView* payload
);

#define AGENTXX_PLUGIN_TOOL_FLAG_NONE         0
#define AGENTXX_PLUGIN_TOOL_FLAG_AUTO_SUMMARY (1 << 0) ///< 输出超限时自动压缩 (经 share_store 卸载)
/// 该工具的执行体可与其他声明了本标志的工具**并发执行**
/// - 只应给"只读、不写会话、不改插件内部共享状态"的工具置位; 未置位=独占
///   (写文件、命令执行、交互询问类工具必须保持独占)
/// - 并发只发生在同一条 assistant 消息声明的工具调用之间, 结果仍按声明顺序写回
/// - 主程序侧并发上限见 [AgentConfig::toolParallelMaxConcurrency]
#define AGENTXX_PLUGIN_TOOL_FLAG_PARALLEL_SAFE (1 << 1)
/// 连续相同调用重复检查 (与内置工具 [agentxx::tools::XXToolBase::repeatCallCheck] 同语义)
/// - 置位: 同一 llm <-> tool 交替链内 (无用户消息打断) 连续多次 (阈值
///   [AgentConfig::toolcallRepeatCheckThreshold], 默认 5 次) 相同工具 + 相同参数
///   调用时, 主程序经用户确认通道询问, 确认后才继续执行, 拒绝则中止本次调用
/// - 未置位 = 关闭 (默认): 只给"模型可能以同一参数反复调用"的工具置位
#define AGENTXX_PLUGIN_TOOL_FLAG_REPEAT_CALL_CHECK (1 << 2)

typedef struct AgentxxPluginToolSpec {
    /// 本结构体字节数 (`sizeof(AgentxxPluginToolSpec)`); `0` = 基线布局
    /// - 以后往尾部追加可选字段时, 主程序读该字段前先用
    ///   `agentxx::plugin::specHas<&AgentxxPluginToolSpec::字段>(spec->struct_size)` 判定
    uint32_t           struct_size;
    uint32_t           _reserved; ///< 8 字节补齐
    PluginxxStringView name; ///< 须全局唯一 (与内置工具/MCP 工具同名将注册失败)
    PluginxxStringView description;
    PluginxxStringView parameters_json; ///< JSON Schema 字符串 (json object)

    /// 启动执行 (【主程序 io 线程调用】, 非阻塞; 操作契约):
    /// - 入参均为指针传递 (只读借用, 仅本次调用有效)
    /// - 结果分两段上报 (正文 + 元数据), 见 [AgentxxPluginToolNotify]:
    ///   快同步工具算完 → `notify->done(PLUGINXX_OPERATOR_OK, &result_sv, &payload_sv)`
    ///   → 返回 NULL
    /// - 锚定协程/自管异步: 创建/挂起任务 → 返回 op 句柄
    /// - 失败: 返回 NULL 且 *error_out 输出错误 (跨边界堆分配字符串, host->alloc 分配)
    void*(PLUGINXX_CALL* execute_start)(
        void*                          user_data,
        const PluginxxStringView*      args_json,
        const PluginxxStringView*      session_id,
        const PluginxxStringView*      tool_call_id,
        const AgentxxPluginToolNotify* notify,
        PluginxxString*                error_out
    );
    /// 协作式取消请求 (io 线程, 非阻塞; 不可取消可留 NULL)
    void(PLUGINXX_CALL* execute_cancel)(void* user_data, void* op);

    void*   user_data;
    int64_t default_timeout_ms; ///< 0 = 不限制 (定长 64 位整型)
    int32_t flags;              ///< AGENTXX_PLUGIN_TOOL_FLAG_*
} AgentxxPluginToolSpec;

/* 基线: 重置时 = 完整布局; 以后追加字段保持不变 (改为 offsetof(第一个追加字段)) */
#define AGENTXX_PLUGIN_TOOL_SPEC_BASELINE_SIZE sizeof(AgentxxPluginToolSpec)

/* ==================== 接口表: 工具 (agentxx.agent.tools) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_TOOLS             PLUGINXX_APP_ID("agent.tools")
/// v2: 全部成员增加 `agent` 参数; 删除 `op_cancel` (统一用 `pluginxx.cancel` 的取消)
/// v3: 工具结果的成败由最终通知的状态位表达 (`PLUGINXX_OPERATOR_OK`/`FAILED`/`CANCELED`),
///     结果文本只给模型读 —— 插件不要再把 `[Error: ...]` 之类前缀写进 payload,
///     失败时只给错误内容 (主程序统一排版); 布局未变 (基线不变)
/// v4: 工具结果改成**两段上报** (`result` 正文 + `payload` 元数据), 完成通知器由
///     `PluginxxOperatorNotify` 换成 [AgentxxPluginToolNotify], 插件互调回调换成
///     [AgentxxPluginToolCallback]; 成员顺序不变, 但两处签名变了 = 破坏性改动
///     (旧插件重建后才能加载; 全局 API 代次同步 +1)
/// v5: `register_tool` / `unregister_tool` 改成异步操作契约 (见 plugin_api.h 文件头);
///     `call_tool_async` 更名为 `call_tool` (异步已是默认, 名字不再带后缀);
///     基线与最低版本重置
#define AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION     5
#define AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_MIN_VERSION 5
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_BASELINE_SIZE sizeof(AgentxxPluginToolsIface)

typedef struct AgentxxPluginToolsIface {
    int32_t  version;     ///< 主程序填 AGENTXX_PLUGIN_IFACE_AGENT_TOOLS_VERSION
    uint32_t struct_size; ///< 主程序填 sizeof(AgentxxPluginToolsIface) 或更大的已知表长

    /// 注册工具 (**异步**)
    /// - `agent` 必须非空 (引用无效时不受理);
    /// - `spec` 里的字符串视图只在本次调用期间有效, 主程序先拷贝再执行;
    /// - 完成时工具已生效, 处于同一事件循环里的后续查询能查到它。
    ///
    /// `status` = `PLUGINXX_OPERATOR_OK` 表示成功; 失败时 `payload` 给原因
    /// (重名 `ERR_CONFLICT` / 参数非法 `ERR_ARG` / 没有挂载 `ERR_NO_AGENT` 的名字)
    PluginxxOperatorHandle*(PLUGINXX_CALL* register_tool)(
        const PluginxxHost*          host,
        const PluginxxScopeRef*      agent,
        const AgentxxPluginToolSpec* spec,
        PluginxxOperatorCallback     cb,
        void*                        ud,
        PluginxxString*              error_out
    );
    /// 注销工具 (**异步**; 按名称, 只允许注销本挂载注册的工具)
    /// - 完成时工具已从注册表移除
    PluginxxOperatorHandle*(PLUGINXX_CALL* unregister_tool)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* name,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );

    /* ---- 插件互调: 完成回调形 ---- */
    /// 调用该 agent 工具表里的一个工具 (异步; 取消用 `pluginxx.cancel.op_cancel`)
    /// - 结果按两段给 `cb` (正文 + 元数据), 与工具自己的上报形状一致
    PluginxxOperatorHandle*(PLUGINXX_CALL* call_tool)(
        const PluginxxHost*        host,
        const PluginxxScopeRef*    agent,
        const PluginxxStringView*  name,
        const PluginxxStringView*  args_json,
        const PluginxxStringView*  session_id,
        AgentxxPluginToolCallback  cb,
        void*                      ud,
        PluginxxString*            error_out
    );
} AgentxxPluginToolsIface;

/* ==================== 接口表: 工具权限声明 (agentxx.agent.permission) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION             PLUGINXX_APP_ID("agent.permission")
/// v2: 全部成员增加 `agent` 参数 (权限中间件与工作区都是 per-agent)
/// v3: 全部成员改成异步操作契约 (`check_paths` 的判定数组经 `payload` 以 JSON 数组交回);
///     基线与最低版本重置
#define AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION_VERSION     3
#define AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION_MIN_VERSION 3
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION_BASELINE_SIZE sizeof(AgentxxPluginPermissionIface)

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

/// 工具权限声明 (插件在注册工具后为自身工具声明权限限制; 由主程序统一判定)
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
    /// 本结构体字节数 (sizeof(AgentxxPluginToolPermissionSpec)); 0 = 基线布局
    uint32_t struct_size;
    uint32_t _reserved; ///< 8 字节补齐
    /// 权限分类文本 (权限询问卡片上显示; 留空则按作用域生成)
    PluginxxStringView category;
} AgentxxPluginToolPermissionSpec;

/* 基线: 重置时 = 完整布局; 以后追加字段保持不变 (改为 offsetof(第一个追加字段)) */
#define AGENTXX_PLUGIN_TOOL_PERMISSION_SPEC_BASELINE_SIZE sizeof(AgentxxPluginToolPermissionSpec)

/// 路径权限批量查询入参 (check_paths; 三态判定, 不发起询问)
typedef struct AgentxxPluginPermissionPathQuery {
    /// 本结构体字节数 (sizeof(AgentxxPluginPermissionPathQuery)); 0 = 基线布局
    uint32_t struct_size;
    /// 权限作用域 (AGENTXX_PLUGIN_PERMISSION_SCOPE_READ / _WRITE)
    int32_t scope;
    /// paths 元素个数 (须 > 0 且不超过主程序上限)
    int32_t  path_count;
    uint32_t _reserved; ///< 8 字节补齐
    /// 会话 (解析会话工作目录与工作区隔离边界; 可为空, 为空时按进程工作目录解析)
    PluginxxStringView session_id;
    /// 待查路径数组 (只读借用, 仅本次调用有效): 绝对路径优先; 相对路径按会话工作目录解析
    const PluginxxStringView* paths;
} AgentxxPluginPermissionPathQuery;

/* 基线: 重置时 = 完整布局; 以后追加字段保持不变 (改为 offsetof(第一个追加字段)) */
#define AGENTXX_PLUGIN_PERMISSION_PATH_QUERY_BASELINE_SIZE sizeof(AgentxxPluginPermissionPathQuery)

typedef struct AgentxxPluginPermissionIface {
    int32_t version;      ///< 主程序填 AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION_VERSION
    uint32_t struct_size; ///< 主程序填 sizeof(AgentxxPluginPermissionIface) 或更大的已知表长

    /// 声明工具权限限制 (**异步**; 工具注册后调用, 同工具重复声明为覆盖)
    /// - 未声明的工具不参与权限判定 (直接放行); 声明后由主程序按权限规则
    ///   (白/黑名单、permission.mode、记住的选择、工作区隔离) 统一判定
    /// - 权限声明属于附加能力: 主程序未加载权限中间件时完成回调给失败, 插件可忽略
    PluginxxOperatorHandle*(PLUGINXX_CALL* register_tool_permission)(
        const PluginxxHost*                    host,
        const PluginxxScopeRef*                agent,
        const AgentxxPluginToolPermissionSpec* spec,
        PluginxxOperatorCallback               cb,
        void*                                  ud,
        PluginxxString*                        error_out
    );
    /// 撤销工具权限声明 (**异步**; 工具注销/插件禁用/卸载时由主程序自动撤销, 一般无需手动调用)
    PluginxxOperatorHandle*(PLUGINXX_CALL* unregister_tool_permission)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* tool_name,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );

    /// 批量查询路径权限判定 (**异步**; advisory, 只读已生效规则)
    /// - **不发起权限询问、不产生中断、不弹任何界面、不做阻塞等待**: 纯只读判定,
    ///   用于支持模式/前缀参数的插件工具 (如 glob/grep) 在枚举出实际路径后逐项过滤
    /// - 判定规则与工具调用权限检查一致 (工作区隔离 → 配置拒绝 → 完全授权 →
    ///   规则表 → noRuleOperator); 区别仅在于"应询问(INTERRUPT)"以 ASK 返回而不询问
    /// - 工具侧建议: DENY 丢弃; ASK 表示"未获批准"同样不应访问 (按未批准处理)
    /// - 单次批量不宜过大, 建议按需分批 (如 512 项)
    ///
    /// 完成时 `payload` 是**判定数组的 JSON 文本** (与 `paths` 等长、顺序一致),
    /// 元素为 `AGENTXX_PLUGIN_PERMISSION_DECISION_*` 的十进制值, 如 `[1,0,2]`。
    /// `status` 非 OK 时调用方应跳过过滤, 按原行为处理
    PluginxxOperatorHandle*(PLUGINXX_CALL* check_paths)(
        const PluginxxHost*                     host,
        const PluginxxScopeRef*                 agent,
        const AgentxxPluginPermissionPathQuery* query,
        PluginxxOperatorCallback                cb,
        void*                                   ud,
        PluginxxString*                         error_out
    );
} AgentxxPluginPermissionIface;

/* ==================== 接口表: 会话访问 (agentxx.agent.session) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_SESSION             PLUGINXX_APP_ID("agent.session")
/// v2: 全部成员增加 `agent` 参数 (会话属于某个 agent)
/// v3: 表尾追加会话工作目录与"是否已取消"查询 (原在插件框架通用表里的业务成员);
///     基线随本代重置为完整布局
/// v4: 改成异步操作契约 (`get_share_store` / `emit_message_tip` /
///     `add_share_store` / `get_session_work_dir`); `is_canceled` 保持同步
///     (本地原子标记, 任意线程直接读); 基线与最低版本重置
#define AGENTXX_PLUGIN_IFACE_AGENT_SESSION_VERSION     4
#define AGENTXX_PLUGIN_IFACE_AGENT_SESSION_MIN_VERSION 4
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_PLUGIN_IFACE_AGENT_SESSION_BASELINE_SIZE sizeof(AgentxxPluginSessionIface)

typedef struct AgentxxPluginSessionIface {
    int32_t  version; ///< 主程序填 AGENTXX_PLUGIN_IFACE_AGENT_SESSION_VERSION
    uint32_t struct_size;

    /// 读取会话级 share_store 条目 (**异步**)
    /// - 成功时 `payload` 是条目正文;
    /// - 条目不存在或 id 未分配 (大于自增 id, 见 [add_share_store]) 时
    ///   `status = PLUGINXX_OPERATOR_FAILED` 并给出原因文本 (旧版是 `ERR_NOT_FOUND`)
    PluginxxOperatorHandle*(PLUGINXX_CALL* get_share_store)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* session_id,
        int64_t                   id,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
    /// 给会话插一条提示消息 (**异步**; 不关心受理结果时 `cb` 传 NULL)
    PluginxxOperatorHandle*(PLUGINXX_CALL* emit_message_tip)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* session_id,
        const PluginxxStringView* text,
        int32_t                   level,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
    /// 写一条会话级 share_store 条目 (**异步**; 成功时 `payload` 是十进制 id)
    ///
    /// 旧版直接返回 `int64_t` id (失败 `-1`); 现在经回调正文交回, 用
    /// `TableOpResult::integer(-1)` 读。
    PluginxxOperatorHandle*(PLUGINXX_CALL* add_share_store)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* session_id,
        const PluginxxStringView* content,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
    /// (v3) 指定会话生效的工作目录 (**异步**; 成功时 `payload` 是绝对路径):
    /// - `agent` 必须非空 (引用无效时入口返回 NULL 并记警告): 会话属于某个 agent
    /// - session_id 非空: 会话级覆写优先 (worktree 绑定), 未设置时回退主程序默认工作目录
    /// - session_id 为空: 返回解析后的默认会话工作目录
    PluginxxOperatorHandle*(PLUGINXX_CALL* get_session_work_dir)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* session_id,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
    /// (v3) 查询会话当前轮次是否已取消 (**同步**; advisory 定位, 权威通知始终是取消回调):
    /// - 保留同步: 取消状态是一个原子标记, 任意线程直接读, 不跨执行体、不产生等待
    /// - `agent` 必须非空 (引用无效返回 0 并记警告)
    /// - 返回 1 已取消 / 0 未取消
    int32_t(PLUGINXX_CALL* is_canceled)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* thread_id
    );
} AgentxxPluginSessionIface;

/* ==================== 接口表: 会话上下文查询 (agentxx.agent.context) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT             PLUGINXX_APP_ID("agent.context")
/// v2: 全部成员增加 `agent` 参数
/// v3: 全部成员改成异步操作契约 (结果经 `payload` 交回)
#define AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT_VERSION     3
#define AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT_MIN_VERSION 3
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT_BASELINE_SIZE sizeof(AgentxxPluginContextIface)

/// 会话 LLM 上下文查询
///
/// 背景: LLM 上下文由会话持有 (唯一权威), 图状态不再包含 `messages` 通道;
/// 插件需要上下文内容时经本表查询, 图状态里的 `xx_messagesMeta` 通道只给
/// 条数/版本/末尾消息摘要等轻量信息 (与上下文大小无关)。
typedef struct AgentxxPluginContextIface {
    int32_t  version; ///< 主程序填 AGENTXX_PLUGIN_IFACE_AGENT_CONTEXT_VERSION
    uint32_t struct_size;

    /// 读取会话 LLM 上下文 (**异步**)
    /// - 完成时 `payload` 是 ChatMessage JSON 数组文本; 会话不存在按失败交回
    PluginxxOperatorHandle*(PLUGINXX_CALL* get_messages)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* session_id,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );

    /// 会话 LLM 上下文条数 (**异步**; 只取数量时用它, 避免为读一个数字拉取整段上下文)
    /// - 完成时 `payload` 是十进制整数文本
    PluginxxOperatorHandle*(PLUGINXX_CALL* messages_count)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* session_id,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
} AgentxxPluginContextIface;

/* ==================== 接口表: 主模型配置 (agentxx.agent.model) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_MODEL             PLUGINXX_APP_ID("agent.model")
/// v2: `get_config` 增加 `agent` 参数
/// v3: `get_config` 改成异步操作契约
#define AGENTXX_PLUGIN_IFACE_AGENT_MODEL_VERSION     3
#define AGENTXX_PLUGIN_IFACE_AGENT_MODEL_MIN_VERSION 3
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_PLUGIN_IFACE_AGENT_MODEL_BASELINE_SIZE sizeof(AgentxxPluginModelIface)

typedef struct AgentxxPluginModelIface {
    int32_t  version; ///< 主程序填 AGENTXX_PLUGIN_IFACE_AGENT_MODEL_VERSION
    uint32_t struct_size;
    /// 该 agent 的主模型及关联配置 JSON (**异步**; 未加载时 `payload` 为空串)
    PluginxxOperatorHandle*(PLUGINXX_CALL* get_config)(
        const PluginxxHost*      host,
        const PluginxxScopeRef*  agent,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    );
} AgentxxPluginModelIface;

/* ==================== 接口表: 主程序提示词读写 (agentxx.agent.prompt) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_PROMPT             PLUGINXX_APP_ID("agent.prompt")
/// v2: 全部成员增加 `agent` 参数 (提示词按 agent 贡献/合成)
/// v3: 表尾追加工具提示词查询 (原在插件框架通用表里的业务成员);
///     基线随本代重置为完整布局
/// v4: 全部成员改成异步操作契约
#define AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_VERSION     4
#define AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_MIN_VERSION 4
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_BASELINE_SIZE sizeof(AgentxxPluginPromptIface)

typedef struct AgentxxPluginPromptIface {
    int32_t  version; ///< 主程序填 AGENTXX_PLUGIN_IFACE_AGENT_PROMPT_VERSION
    uint32_t struct_size;

    /// 读当前完整提示词 JSON (**异步**; `payload` = 提示词 JSON 文本)
    PluginxxOperatorHandle*(PLUGINXX_CALL* get_prompt)(
        const PluginxxHost*      host,
        const PluginxxScopeRef*  agent,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    );
    /// 写回本插件贡献的提示词段 (**异步**; 完成时已生效)
    PluginxxOperatorHandle*(PLUGINXX_CALL* set_prompt)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* prompt_json,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
    /// 指定工具的提示词配置 JSON (**异步**)
    /// - `payload`: {"depict": "...", "args": {"参数名": "参数说明", ...}}
    /// - 未配置该工具时按失败交回 (`payload` 给原因)
    PluginxxOperatorHandle*(PLUGINXX_CALL* get_tool_prompt)(
        const PluginxxHost*       host,
        const PluginxxStringView* tool_name,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
} AgentxxPluginPromptIface;

/* ==================== 接口表: 会话资源贡献 (agentxx.agent.resources) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES             PLUGINXX_APP_ID("agent.resources")
/// v2: 全部成员增加 `agent` 参数 (资源按挂载应用, 每个 agent 各一份)
/// v3: 全部成员改成异步操作契约 (登记类完成时已生效, `get_own_resources` 的清单经
///     `payload` 交回)
#define AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_VERSION     3
#define AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_MIN_VERSION 3
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_BASELINE_SIZE sizeof(AgentxxPluginResourcesIface)

typedef struct AgentxxPluginResourcesIface {
    int32_t  version; ///< 主程序填 AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES_VERSION
    uint32_t struct_size;

    /// 登记技能目录 (**异步**; 完成时已生效: 主程序已完成目录扫描与索引)
    PluginxxOperatorHandle*(PLUGINXX_CALL* register_skill_dir)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* path,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
    PluginxxOperatorHandle*(PLUGINXX_CALL* unregister_skill_dir)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* path,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
    /// 登记记忆文件 (**异步**; 完成时已读入并校验)
    PluginxxOperatorHandle*(PLUGINXX_CALL* register_memory_file)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* path,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
    PluginxxOperatorHandle*(PLUGINXX_CALL* unregister_memory_file)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* path,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
    /// 登记 MCP 服务器 (**异步**; 可能要建连接/握手, 完成时已可用)
    PluginxxOperatorHandle*(PLUGINXX_CALL* register_mcp_server)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* spec_json,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
    PluginxxOperatorHandle*(PLUGINXX_CALL* unregister_mcp_server)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* name_space,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
    /// 本插件向该 agent 贡献的资源清单 (**异步**; `payload` 为 JSON)
    PluginxxOperatorHandle*(PLUGINXX_CALL* get_own_resources)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
} AgentxxPluginResourcesIface;

/* ==================== 接口表: 执行图 (agentxx.agent.graph) ==================== */

/// 插件节点执行函数 (【主程序 io 线程调用】, 非阻塞):
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
    /// 本结构体字节数 (sizeof(AgentxxPluginGraphNodeTypeSpec)); 0 = 基线布局
    uint32_t                          struct_size;
    uint32_t                          _reserved;  ///< 8 字节补齐
    PluginxxStringView                type;       ///< 节点类型名 (须全局唯一)
    AgentxxPluginGraphNodeRunStartFn  run_start;  ///< 节点执行 (操作契约)
    AgentxxPluginGraphNodeRunCancelFn run_cancel; ///< 可空
    void*                             user_data;  ///< 原样转给 run_start/run_cancel
    /// 可选节点 config JSON Schema (Draft 2020-12 片段; 仅供导出/文档, 引擎不校验)
    PluginxxStringView config_schema_json;
} AgentxxPluginGraphNodeTypeSpec;

/* 基线: 重置时 = 完整布局; 以后追加字段保持不变 (改为 offsetof(第一个追加字段)) */
#define AGENTXX_PLUGIN_GRAPH_NODE_TYPE_SPEC_BASELINE_SIZE sizeof(AgentxxPluginGraphNodeTypeSpec)

/// 执行图接口表: 插件注册自定义节点类型 + 读写主程序执行图 JSON 定义
/// - 节点类型注册进 per-agent GraphRegistry (多实例隔离), 图编译时按类型名
///   实例化; 卸载插件后不再编译新图 (engine 已构建), 注册残留无害
/// - get/set graph JSON 用于插件查看/修改主程序执行图 (默认名 "agentxx.default");
///   修改后的 JSON 在主程序构建 engine 前生效 (插件须保证图合法性, 非法时主程序
///   回退默认图并记日志)
#define AGENTXX_PLUGIN_IFACE_AGENT_GRAPH             PLUGINXX_APP_ID("agent.graph")
/// v2: 全部成员增加 `agent` 参数 (图注册表与图定义都是 per-agent)
/// v3: 全部成员改成异步操作契约 (注册类完成时已生效, 读取类结果经 `payload` 交回)
#define AGENTXX_PLUGIN_IFACE_AGENT_GRAPH_VERSION     3
#define AGENTXX_PLUGIN_IFACE_AGENT_GRAPH_MIN_VERSION 3
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_PLUGIN_IFACE_AGENT_GRAPH_BASELINE_SIZE sizeof(AgentxxPluginGraphIface)

typedef struct AgentxxPluginGraphIface {
    int32_t  version; ///< 主程序填 AGENTXX_PLUGIN_IFACE_AGENT_GRAPH_VERSION
    uint32_t struct_size;

    /// 注册节点类型 (**异步**; 类型名冲突时完成回调给失败)
    /// - 完成时已生效, 之后的建图能看到该类型
    PluginxxOperatorHandle*(PLUGINXX_CALL* register_node_type)(
        const PluginxxHost*                   host,
        const PluginxxScopeRef*               agent,
        const AgentxxPluginGraphNodeTypeSpec* spec,
        PluginxxOperatorCallback              cb,
        void*                                 ud,
        PluginxxString*                       error_out
    );
    /// 注销节点类型 (**异步**; 按类型名, 挂载卸载时主程序自动清理)
    PluginxxOperatorHandle*(PLUGINXX_CALL* unregister_node_type)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* type,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
    /// 获取该 agent 当前执行图 JSON 定义 (**异步**; `payload` 为图定义 JSON)
    PluginxxOperatorHandle*(PLUGINXX_CALL* get_graph_json)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
    /// 获取当前执行图名称 (**异步**; 默认 "agentxx.default")
    PluginxxOperatorHandle*(PLUGINXX_CALL* get_graph_name)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
    /// 设置执行图 JSON 定义 (**异步**; 覆盖, 主程序构建 engine 前生效)
    /// - JSON 非法时完成回调给失败
    PluginxxOperatorHandle*(PLUGINXX_CALL* set_graph_json)(
        const PluginxxHost*       host,
        const PluginxxScopeRef*   agent,
        const PluginxxStringView* graph_json,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
} AgentxxPluginGraphIface;


/* ==================== 接口表: 挂载查询 (agentxx.agent.mount) ==================== */

/// 挂载查询: 让插件看清"我在这几个 agent 上挂好了"
///
/// 插件自己就拿着 agent 引用 (挂载时下发), 不需要问主程序"我在谁身上";
/// 本表的用途是诊断与自查 (例如启动日志、测试断言)。
#define AGENTXX_PLUGIN_IFACE_AGENT_MOUNT             PLUGINXX_APP_ID("agent.mount")
/// v2: 两个成员改成异步操作契约 (摘要在完成回调的 `payload` 里)
#define AGENTXX_PLUGIN_IFACE_AGENT_MOUNT_VERSION     2
#define AGENTXX_PLUGIN_IFACE_AGENT_MOUNT_MIN_VERSION 2
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_PLUGIN_IFACE_AGENT_MOUNT_BASELINE_SIZE sizeof(AgentxxPluginMountIface)

typedef struct AgentxxPluginMountIface {
    int32_t  version; ///< 主程序填 AGENTXX_PLUGIN_IFACE_AGENT_MOUNT_VERSION
    uint32_t struct_size;

    /// 全部挂载的摘要 (**异步**; `payload` 为 JSON 数组)
    /// 每项: {"agentId","agentName","workDir","language","mountSeq","plugins":[...]}
    PluginxxOperatorHandle*(PLUGINXX_CALL* list)(
        const PluginxxHost*      host,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    );
    /// 按 agentId 查一个挂载的摘要 (**异步**; 没有这个挂载时完成回调给失败)
    PluginxxOperatorHandle*(PLUGINXX_CALL* get)(
        const PluginxxHost*       host,
        const PluginxxStringView* agent_id,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
} AgentxxPluginMountIface;

/* ==================== 接口表: 会话运行时 (agentxx.agent.runtime) ==================== */

/// 会话运行时: 让插件挂载自己的根 agent, 并在上面开关会话通道
///
/// 为什么需要这张表: 插件要"换一整套人设/工具集/插件挂载"只能在 `AgentConfig`
/// 级说, 会话级表达不了; 而插件实例全程只有一份 (`create`/`start` 各一次),
/// 所以它不是"再加载一次插件", 而是**把自己的根挂到现有容器上**。
///
/// 概念的三个层次:
/// - **根 (root)**: 挂到容器的根 agent, 各自一份 `AgentConfig` (提示词/白名单/挂载);
/// - **通道 (channel)**: 一个 `sessionId` 的驱动通道 (输入/事件/中断/状态);
/// - **触发器 (trigger) / 定时器 (timer)**: 把事件或时间转成通道输入。
///
/// 边界: 本表用句柄寻址 (`root_id` / `channel_id`), 因此**不带** agent 引用参数 ——
/// 它是容器级接口, 不是"当前 agent"接口。`root_id` 与该根的 `agentId` 是同一个值,
/// 需要 agent 级操作 (查挂了哪些插件等) 时直接当 agentId 用。
#define AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME             PLUGINXX_APP_ID("agent.runtime")
/// v2: 表尾追加 4 个后台任务入口 (submit/cancel/get/list_async_work)
/// v3: 21 个成员全部改成异步操作契约 (受理即返回, 结果经 cb 交回; 基线重置为整表长度)
#define AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME_VERSION     3
#define AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME_MIN_VERSION 3
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME_BASELINE_SIZE sizeof(AgentxxPluginRuntimeIface)

/* ---- 返回码 ---- */
/*
 * 全部入口共用 `agentxx_result.h` 的码表 (`AGENTXX_RESULT_OK` / `AGENTXX_RESULT_ERR_*`):
 * - `ERR_ARG`: 配置派生失败 / 字段越界 / `struct_size` 低于基线
 * - `ERR_STATE`: 未就绪 / 关闭中 / 在 `mount` 里调只能 `start` 用的入口
 * - `ERR_NOT_FOUND`: root / channel / trigger / timer / 任务 id 不存在
 * - `ERR_LIMIT`: 超预算 (根数 / 通道数 / 触发器数 / 定时器数)
 * - `ERR_CONFLICT`: (存储根, sessionId) 已被占用
 * - `ERR_UNSUPPORTED`: 主程序未提供运行时能力
 * - `ERR_FAILED`: 执行失败 (init 失败等)
 * 句柄类入口 (`open_channel` / `register_trigger` / `register_timer`) 返回 id, 失败给 `-1`。
 */

/// 通道事件类型 (回调的 `kind` 取值)
///
/// 顺序是有意义的: 新增类型只能追加在 `AGENTXX_PLUGIN_CHANNEL_EVT_COUNT` 之前。
typedef enum AgentxxPluginChannelEventKind {
    AGENTXX_PLUGIN_CHANNEL_EVT_READY = 0, ///< 通道可用 (payload 带 rootId/sessionId/model)
    AGENTXX_PLUGIN_CHANNEL_EVT_TURN_START, ///< 一轮开始 (text = 用户输入回显)
    AGENTXX_PLUGIN_CHANNEL_EVT_DELTA_TEXT, ///< 正文增量 (text)
    AGENTXX_PLUGIN_CHANNEL_EVT_DELTA_THINKING, ///< 思考增量 (text)
    AGENTXX_PLUGIN_CHANNEL_EVT_TOOL_START,     ///< 工具开始
    AGENTXX_PLUGIN_CHANNEL_EVT_TOOL_END,       ///< 工具结束
    AGENTXX_PLUGIN_CHANNEL_EVT_GEN_STATUS,     ///< 生成阶段 (waiting/tool_call/done)
    AGENTXX_PLUGIN_CHANNEL_EVT_TURN_END,       ///< 一轮结束 (text = 最终回答)
    AGENTXX_PLUGIN_CHANNEL_EVT_CONTEXT_STATS,  ///< 上下文统计与累计用量
    AGENTXX_PLUGIN_CHANNEL_EVT_INPUT_ACK,      ///< 输入受理回执
    AGENTXX_PLUGIN_CHANNEL_EVT_QUEUE_STATE,    ///< 队列状态
    AGENTXX_PLUGIN_CHANNEL_EVT_INTERRUPT,      ///< 需要人工答复 (interrupt_id + payload)
    AGENTXX_PLUGIN_CHANNEL_EVT_INTERRUPT_EXPIRED, ///< 询问已过期
    AGENTXX_PLUGIN_CHANNEL_EVT_TIP,               ///< 提示 (错误/警告文本)
    AGENTXX_PLUGIN_CHANNEL_EVT_ERROR,             ///< 运行时错误 (payload.code/message)
    AGENTXX_PLUGIN_CHANNEL_EVT_CLOSED,            ///< 通道已关闭 (回调的最后一条)
    AGENTXX_PLUGIN_CHANNEL_EVT_COUNT
} AgentxxPluginChannelEventKind;

/// 一条通道事件
///
/// - 字符串都是**借用视图**, 只在回调期间有效; 要留着就自己拷
/// - `payload_json` 的形状见插件设计文档"通道事件载荷"一节
/// - `struct_size` 由主程序填 `sizeof(...)`; 以后追加字段时插件先判长度
typedef struct AgentxxPluginChannelEvent {
    uint32_t struct_size; ///< 本结构体字节数; 0 = 基线布局
    int32_t  kind;        ///< AgentxxPluginChannelEventKind
    int64_t  channel_id;
    int64_t  interrupt_id;         ///< 仅 INTERRUPT / INTERRUPT_EXPIRED 有效
    PluginxxStringView text;         ///< 增量 / 回显 / 最终回答 / 提示
    PluginxxStringView payload_json; ///< 结构化载荷
} AgentxxPluginChannelEvent;

/* 基线: 重置时 = 完整布局; 以后追加字段保持不变 (改为 offsetof(第一个追加字段)) */
#define AGENTXX_PLUGIN_CHANNEL_EVENT_BASELINE_SIZE sizeof(AgentxxPluginChannelEvent)

/// 通道事件回调 (**在该根所属 io 线程上调用, 必须快速返回**)
/// - 不得在此同步阻塞 (等待网络/人工答复): 整个进程的会话都在这条线程上
/// - 事件只在回调期间有效
/// - 通道关闭时最后一条必为 `AGENTXX_PLUGIN_CHANNEL_EVT_CLOSED`
/// - `user_data` 为 [AgentxxPluginChannelSpec::user_data]
typedef void(PLUGINXX_CALL* AgentxxPluginChannelEventFn)(
    void*                            user_data,
    const AgentxxPluginChannelEvent* event
);

/// 挂载一个根 agent (带自己的配置)
typedef struct AgentxxPluginMountRootSpec {
    uint32_t struct_size; ///< sizeof(AgentxxPluginMountRootSpec); 0 = 基线布局
    /// 展示名 (也用于生成 root id: "qq" → "ext:qq:1")
    PluginxxStringView name;
    /// `AgentConfig` 覆盖 JSON (字段与规则见设计文档"根配置"一节)
    /// - 空 = 只用派生默认值 (输出为 coding 人设)
    /// - 应在里面显式给 `prompt.systemPrompt`
    PluginxxStringView config_json;
    /// 该根的插件挂载策略 JSON (与 yaml `plugin.mount` 同构):
    /// `{"mode":"none|all|listed","list":["agentxx_filesystem"],"args":{...}}`
    /// - 空 = `none`: 根只有内置工具 (share_store / subagent / git_worktree)
    /// - 要文件/命令工具就必须给 `listed` (或 `all`)
    PluginxxStringView mount_plugins;
    /// 0 = 创建者插件**不**挂到该根 (默认, 切断递归);
    /// 1 = 挂 (想让根也能调本插件的工具时, 配合 `mount_plugins` 使用)
    int32_t include_creator;
    int32_t _reserved;
} AgentxxPluginMountRootSpec;

/* 基线: 重置时 = 完整布局; 以后追加字段保持不变 (改为 offsetof(第一个追加字段)) */
#define AGENTXX_PLUGIN_MOUNT_ROOT_SPEC_BASELINE_SIZE sizeof(AgentxxPluginMountRootSpec)

/// 打开一个会话通道 (绑定某个根)
typedef struct AgentxxPluginChannelSpec {
    uint32_t struct_size; ///< sizeof(AgentxxPluginChannelSpec); 0 = 基线布局
    int32_t  _reserved;
    /// 根 id (必需; `mount_root` 返回的那个)
    PluginxxStringView root_id;
    /// 会话 id
    /// - 空 = 框架生成
    /// - 非空时 **(会话存储根, sessionId)** 在进程内必须唯一: 两个通道驱动同一个
    ///   会话会静默互盖数据, 因此撞了直接拒
    PluginxxStringView session_id;
    /// 会话标题 (可空)
    PluginxxStringView title;
    /// 初始模型 (可空 = 用根的默认模型)
    PluginxxStringView model;
    /// 输入队列上限 (0 = 默认 100)
    int32_t max_queue;
    int32_t _reserved2;
    /// 中断等待上限 (毫秒; 0 = 不限; >0 = 到点按取消应答)
    int64_t interrupt_timeout_ms;
    /// 事件回调 (必需)
    AgentxxPluginChannelEventFn on_event;
    void*                        user_data;
} AgentxxPluginChannelSpec;

/* 基线: 重置时 = 完整布局; 以后追加字段保持不变 (改为 offsetof(第一个追加字段)) */
#define AGENTXX_PLUGIN_CHANNEL_SPEC_BASELINE_SIZE sizeof(AgentxxPluginChannelSpec)

/* ---- 触发器投递策略 ---- */
#define AGENTXX_PLUGIN_TRIGGER_POLICY_QUEUE        0 ///< 每条立即投 (会话队列保证串行)
#define AGENTXX_PLUGIN_TRIGGER_POLICY_COALESCE     1 ///< 窗口内合并成一条
#define AGENTXX_PLUGIN_TRIGGER_POLICY_DROP_IF_BUSY 2 ///< 通道在忙时丢弃

/// 登记一个触发器 (订阅一个主题, 按策略转成通道输入)
typedef struct AgentxxPluginTriggerSpec {
    uint32_t struct_size; ///< sizeof(AgentxxPluginTriggerSpec); 0 = 基线布局
    int32_t  policy;      ///< AGENTXX_PLUGIN_TRIGGER_POLICY_*
    /// COALESCE 的合并窗口 (毫秒; 0 = 默认 300)
    int32_t debounce_ms;
    /// 合并后待投递上限 (0 = 默认 20)
    int32_t max_queue;
    int32_t _reserved;
    /// 主题 (不以 `plugin.` / `client.` 开头时自动补 `plugin.`)
    PluginxxStringView topic;
    int64_t            channel_id;
    /// 投递模式 (空 = next-turn; 另见 next-step / inject / collect)
    PluginxxStringView delivery;
} AgentxxPluginTriggerSpec;

/* 基线: 重置时 = 完整布局; 以后追加字段保持不变 (改为 offsetof(第一个追加字段)) */
#define AGENTXX_PLUGIN_TRIGGER_SPEC_BASELINE_SIZE sizeof(AgentxxPluginTriggerSpec)

/// 登记一个定时触发 (一次性或周期)
typedef struct AgentxxPluginTimerSpec {
    uint32_t struct_size; ///< sizeof(AgentxxPluginTimerSpec); 0 = 基线布局
    int32_t  policy;      ///< AGENTXX_PLUGIN_TRIGGER_POLICY_* (默认 DROP_IF_BUSY)
    int64_t  channel_id;
    /// 首次触发延迟 (毫秒; 相对时间, 不用绝对时间戳以免各进程时钟不一致)
    int64_t first_delay_ms;
    /// 周期间隔 (毫秒; 0 = 一次性)
    int64_t interval_ms;
    /// 投递的输入 JSON (形状同 `send_input`)
    PluginxxStringView input_json;
} AgentxxPluginTimerSpec;

/* 基线: 重置时 = 完整布局; 以后追加字段保持不变 (改为 offsetof(第一个追加字段)) */
#define AGENTXX_PLUGIN_TIMER_SPEC_BASELINE_SIZE sizeof(AgentxxPluginTimerSpec)

/* ---- 后台任务 (async work) ---- */

/// 任务事件阶段 (回调的 `phase` 参数)
/// - `QUEUED`: 已受理排队 (`payload_json` 带 `queueAhead`)
/// - `STARTED`: 从队列起跑
/// - `FINISHED` / `CANCELED`: 终态 (取消优先: 取消时即使带错误也按 CANCELED 上报)
#define AGENTXX_PLUGIN_ASYNC_WORK_EVT_QUEUED   0
#define AGENTXX_PLUGIN_ASYNC_WORK_EVT_STARTED  1
#define AGENTXX_PLUGIN_ASYNC_WORK_EVT_FINISHED 2
#define AGENTXX_PLUGIN_ASYNC_WORK_EVT_CANCELED 3

/// 任务事件回调 (**在容器 io 线程调用, 必须快速返回**)
/// - 不得在此同步阻塞 (等待网络/人工答复): 整个进程的会话都在这条线程上
/// - `payload_json` 与其中的字符串只在回调期间有效; 要留着就自己拷
/// - 插件禁用/卸载后不再回调 (主程序解绑时同时取消该插件提交的任务)
typedef void(PLUGINXX_CALL* AgentxxPluginAsyncWorkEventFn)(
    void*                     user_data,
    int32_t                   phase,
    const PluginxxStringView* payload_json
);

/// 提交一个后台任务 (`submit_async_work` 的参数)
///
/// 语义与 C++ 侧 `agentxx::agent::AsyncWorkSpec` 一致: 提交即入队, 结果发到固定事件
/// `agentxx.agent.async.done` (值里用 `asyncId` 区分), 标识不另传。
/// 字段全是可选/有默认; 新字段只能追加在尾部 (读取方用 `specSizeOk` 判)。
typedef struct AgentxxPluginAsyncWorkSpec {
    uint32_t struct_size; ///< sizeof(AgentxxPluginAsyncWorkSpec); 0 = 基线布局
    /// 0 = `agent` (起一个 agent 跑任务) ; 1 = `tool` (把一次工具调用放到后台跑)
    /// - `1` 需要 `tool_name` (追加字段, 旧布局填不出来时按 `ERR_ARG` 拒)
    int32_t kind;
    /// 中断策略: 0 = bubble 1 = deny 2 = allow; -1 = 跟随创建者配置
    int32_t interrupt_policy;
    /// 任务会话是否落盘: 0 = 否, 1 = 是, -1 = 跟随创建者配置
    int32_t session_store;
    int32_t _reserved;
    int64_t interrupt_timeout_ms; ///< 0 = 不限
    /// 运行时限 (`AsyncWorkSpec::runTimeout`): 从起跑算起, 0 = 不限
    /// - `kind=0` (agent): 到点置取消令牌, 引擎在检查点停下, 任务按失败收尾
    /// - `kind=1` (tool): 到点只结束等待, 工具本体可能继续跑 (结果丢弃)
    /// - 原名 `time_limit_ms`: 只改字段名, 类型与偏移未变 (旧插件编好的库照样能用)
    int64_t run_timeout_ms;
    /// 创建者会话 (通道的会话): 结果事件与 HIL 都按它找创建者; 空 = 不关联会话
    PluginxxStringView session_id;
    /// 排队与额度分组; 空 = 创建者 agentId
    PluginxxStringView queue_key;
    PluginxxStringView name;
    PluginxxStringView system_prompt;
    PluginxxStringView message;
    /// 空 = 创建者的 subagent 模型
    PluginxxStringView model;
    /// 工具策略 JSON: 空 = 默认; `[]` = 无工具; `["*"]` = 继承创建者; 否则名单
    PluginxxStringView tools_json;
    /// 插件挂载 (与 yaml `plugin.mount` 同构); 空 = 不挂
    /// - 键: `mode` (`all` / `listed` / `none`)、`list`、`args`、`exclude`
    /// - `exclude` 是剔除名单 (`all` / `listed` 都生效)
    PluginxxStringView plugins_json;
    PluginxxStringView mcp_servers_json; ///< 字符串数组
    PluginxxStringView skills_json;      ///< 字符串数组
    PluginxxStringView memory_files_json; ///< 字符串数组
    PluginxxStringView rag_docs_json;     ///< 字符串数组
    PluginxxStringView work_dir;          ///< 空 = 跟随创建者
    PluginxxStringView language;          ///< 空 = 跟随创建者
    PluginxxStringView permission_allow_json; ///< 字符串数组
    PluginxxStringView permission_deny_json;  ///< 字符串数组
    /// 结果额外投递到创建者会话: 空 = 不投递; `next-step` / `inject` / `next-turn`
    PluginxxStringView notify_delivery;
    /// 任务事件回调 (可空)
    AgentxxPluginAsyncWorkEventFn on_event;
    void*                          user_data;
    /// ---- 以下两项是追加字段: 只在 `kind = 1` (`tool`) 时使用 ----
    /// 要后台执行的工具名 (在**插件自己挂的那个根/会话**的工具表里查)
    PluginxxStringView tool_name;
    /// 工具参数 JSON 对象原文 (空 = `{}`); 不是对象时按 `ERR_ARG` 拒
    PluginxxStringView tool_args_json;
    /// ---- 以下两项也是追加字段 (排队与起跑时机) ----
    /// 起跑延时 (毫秒; <= 0 = 不延时): 到点之前任务一直是 `queued`, 不占名额
    /// - 提交后没有改延时的成员: 要改就 `cancel_async_work` 后重新提交
    int64_t delay_ms;
    /// 排队时限 (毫秒; <= 0 = 不限): 计时起点是**延时结束那一刻** (无延时 = 提交时)
    /// - 到点还没拿到名额: 任务出队并按 `failed` 收尾 (`error` 写 queue timeout)
    int64_t queue_timeout_ms;
} AgentxxPluginAsyncWorkSpec;

/* 基线: 重置时 = 完整布局; 以后追加字段保持不变 (改为 offsetof(第一个追加字段)) */
#define AGENTXX_PLUGIN_ASYNC_WORK_SPEC_BASELINE_SIZE offsetof(AgentxxPluginAsyncWorkSpec, tool_name)

typedef struct AgentxxPluginRuntimeIface {
    int32_t  version; ///< 主程序填 AGENTXX_PLUGIN_IFACE_AGENT_RUNTIME_VERSION
    uint32_t struct_size;

    /* ---- 根 agent ---- */

    /// 挂载一个带自己配置的根 (异步 init, 受理后经 `payload` 交回根 id)
    ///
    /// - 只能在 `start` (或其它容器级时机) 调用; **在 `mount` 回调里调用按
    ///   `invalid_state` 失败** —— 否则新根又要挂载一次, 形成无限嵌套
    /// - 配置非法 / 模型不存在 / 挂载策略非法按 `invalid_argument` 失败;
    ///   超根数上限按 `limit_exceeded`; 主程序没有容器 (未 attach) 按 `unsupported`
    /// - 新根由主程序强注入容器那份插件管理器, 按 `mount_plugins` 策略挂载已加载的插件
    /// - init 完成前对该根 `open_channel` 会排队, 就绪后自动继续
    /// - 完成时 `payload` 是框架分配的根 id (= 该根的 agentId)
    PluginxxOperatorHandle*(PLUGINXX_CALL* mount_root)(
        const PluginxxHost*               host,
        const AgentxxPluginMountRootSpec* spec,
        PluginxxOperatorCallback          cb,
        void*                             ud,
        PluginxxString*                   error_out
    );

    /// 卸载根 (其下通道先全部关闭, 不等待收尾)
    /// - 内置根、别的实例的根、已卸载的 id 都按 `not_found` 失败
    PluginxxOperatorHandle*(PLUGINXX_CALL* unmount_root)(
        const PluginxxHost*       host,
        const PluginxxStringView* root_id,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );

    /* ---- 会话通道 ---- */

    /// 打开通道 (异步就绪; 就绪后回调收到 `EVENT_READY`)
    /// - 同一个 `sessionId` 开两个通道按 `conflict` 失败
    /// - 超每根通道数上限按 `limit_exceeded` 失败
    /// - 完成时 `payload` 是框架分配的 channel id (十进制文本)
    PluginxxOperatorHandle*(PLUGINXX_CALL* open_channel)(
        const PluginxxHost*             host,
        const AgentxxPluginChannelSpec* spec,
        PluginxxOperatorCallback        cb,
        void*                           ud,
        PluginxxString*                 error_out
    );

    /// 关闭通道 (不等待; 回调最后一条是 `EVENT_CLOSED`)
    PluginxxOperatorHandle*(PLUGINXX_CALL* close_channel)(
        const PluginxxHost*      host,
        int64_t                  channel_id,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    );

    /* ---- 输入与控制 ---- */

    /// 投递一条输入 (输入 JSON: `{"text","attachments","model","delivery"}`)
    PluginxxOperatorHandle*(PLUGINXX_CALL* send_input)(
        const PluginxxHost*       host,
        int64_t                   channel_id,
        const PluginxxStringView* input_json,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );

    /// 取消通道当前轮次
    PluginxxOperatorHandle*(PLUGINXX_CALL* cancel)(
        const PluginxxHost*      host,
        int64_t                  channel_id,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    );

    /// 切换通道的模型
    PluginxxOperatorHandle*(PLUGINXX_CALL* select_model)(
        const PluginxxHost*       host,
        int64_t                   channel_id,
        const PluginxxStringView* model_name,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );

    /// 应答一个中断 (values_json 必须是 `{"values":{...}}` 对象形态)
    PluginxxOperatorHandle*(PLUGINXX_CALL* respond_interrupt)(
        const PluginxxHost*       host,
        int64_t                   channel_id,
        int64_t                   interrupt_id,
        const PluginxxStringView* values_json,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );

    /* ---- 查询 ---- */

    /// 通道状态 JSON (不含消息正文)
    PluginxxOperatorHandle*(PLUGINXX_CALL* get_channel_state)(
        const PluginxxHost*      host,
        int64_t                  channel_id,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    );

    /// 通道会话的 LLM 上下文 (JSON 数组文本; `host->alloc`)
    PluginxxOperatorHandle*(PLUGINXX_CALL* get_context_messages)(
        const PluginxxHost*      host,
        int64_t                  channel_id,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    );

    /// 运行时清单: `[{"rootId","name","state","configSummary":{...},"channels":[...]}]`
    PluginxxOperatorHandle*(PLUGINXX_CALL* list_runtime)(
        const PluginxxHost*      host,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    );

    /* ---- 事件驱动 ---- */

    /// 登记触发器
    PluginxxOperatorHandle*(PLUGINXX_CALL* register_trigger)(
        const PluginxxHost*             host,
        const AgentxxPluginTriggerSpec* spec,
        PluginxxOperatorCallback        cb,
        void*                           ud,
        PluginxxString*                 error_out
    );

    /// 注销触发器
    PluginxxOperatorHandle*(PLUGINXX_CALL* unregister_trigger)(
        const PluginxxHost*      host,
        int64_t                  trigger_id,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    );

    /// 发布一条事件 (任意线程, 立即返回)
    PluginxxOperatorHandle*(PLUGINXX_CALL* emit_event)(
        const PluginxxHost*       host,
        const PluginxxStringView* topic,
        const PluginxxStringView* payload_json,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );

    /* ---- 定时触发 ---- */

    /// 登记定时器
    PluginxxOperatorHandle*(PLUGINXX_CALL* register_timer)(
        const PluginxxHost*           host,
        const AgentxxPluginTimerSpec* spec,
        PluginxxOperatorCallback      cb,
        void*                         ud,
        PluginxxString*               error_out
    );

    /// 注销定时器
    PluginxxOperatorHandle*(PLUGINXX_CALL* unregister_timer)(
        const PluginxxHost*      host,
        int64_t                  timer_id,
        PluginxxOperatorCallback cb,
        void*                    ud,
        PluginxxString*          error_out
    );

    /* ---- 后台任务 (v2 尾部追加, v3 起改为异步操作契约) ---- */

    /// 提交一个后台任务 (入队即返回; 结果发到 `agentxx.agent.async.done`)
    /// - 只校验参数, 不因额度/排队被拒 (额度只影响何时起跑)
    /// - 完成时 `payload` 是框架分配的 id (= 该任务 agent 的 sessionId)
    /// - 插件禁用/卸载时本实例提交的任务会被取消 (终态照常记录)
    PluginxxOperatorHandle*(PLUGINXX_CALL* submit_async_work)(
        const PluginxxHost*               host,
        const AgentxxPluginAsyncWorkSpec* spec,
        PluginxxOperatorCallback          cb,
        void*                             ud,
        PluginxxString*                   error_out
    );

    /// 取消一条后台任务 (排队中直接出队; 运行中置取消令牌)
    PluginxxOperatorHandle*(PLUGINXX_CALL* cancel_async_work)(
        const PluginxxHost*       host,
        const PluginxxStringView* async_id,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );

    /// 单条任务 JSON (不存在或不是本实例提交的按 `not_found` 失败)
    PluginxxOperatorHandle*(PLUGINXX_CALL* get_async_work)(
        const PluginxxHost*       host,
        const PluginxxStringView* async_id,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );

    /// 任务清单 JSON (数组; `session_id` 空 = 本实例提交的全部)
    PluginxxOperatorHandle*(PLUGINXX_CALL* list_async_work)(
        const PluginxxHost*       host,
        const PluginxxStringView* session_id,
        PluginxxOperatorCallback  cb,
        void*                     ud,
        PluginxxString*           error_out
    );
} AgentxxPluginRuntimeIface;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

/* C++ 侧: 本头各表的 IfaceMeta 特化 (最低版本 + 基线字节数), 供
 * `pluginxx::validateInterface` / `ifaceHas` 使用。必须放在表声明之后,
 * 且纯 C 编译单元不参与 (本块被 `__cplusplus` 排除)。 */
#ifdef __cplusplus
#include "agentxx/plugin/api/plugin_iface_meta.h"
#include "agentxx/plugin/api/plugin_spec.h"
#endif

#endif /* AGENTXX_PLUGIN_API_H */
