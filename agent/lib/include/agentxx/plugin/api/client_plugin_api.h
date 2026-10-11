/// client 侧插件系统纯 C ABI 契约 (client 业务表)
///
/// ════════════════════════════════════════════════════════════════════
/// 架构: COM 风格接口表查询
/// ════════════════════════════════════════════════════════════════════
/// - 明确字节对齐: 全部跨边界 ABI 结构体严格遵循 8 字节对齐 (#pragma pack(push, 8))
/// - 明确基本类型: 统一使用定长基本类型 (int32_t, int64_t, uint64_t)，杜绝跨平台整型宽度差异
/// - 明确函数调用约定: 接口表函数指针、入口符号与回调全部显式标注 PLUGINXX_CALL (__stdcall)
/// - 结构体传递与返回值规范:
///   * 结构体入参统一采用指针传递 (const Struct*)
///   * 结构体返回值统一改为函数出参 (Struct* out) 并返回 int32_t 状态码
/// - 版本策略:
///   * 插件级: 全局 `AGENTXX_CLIENT_PLUGIN_API_VERSION` (要求 `api_version >=` 该值)
///   * 表级: 每张表四项齐全 (IID / `struct_size` / `version` - `_VERSION` / `_MIN_VERSION` /
///     `_BASELINE_SIZE`); 判定规则与追加策略见 `pluginxx/api/tables.h` 文件头
///     与 docs/zh-cn/design/plugins.md §9
///
/// ════════════════════════════════════════════════════════════════════
/// 归属分层 (插件框架已拆分为 cxx_pluginxx 独立工程)
/// ════════════════════════════════════════════════════════════════════
/// - 插件框架的通用部分 (与主程序业务无关): `pluginxx/api/abi.h` / `pluginxx/api/tables.h`
///   (本头已包含, client 插件只需包含本头即可拿到通用 ABI 声明)
/// - **本头只声明 client 业务表**: 播放界面扩展 (agentxx.client.ui)、事件订阅
///   (agentxx.client.events)、会话与输入 (agentxx.client.session)、通信 (agentxx.client.wire)、
///   自身元信息 (agentxx.client.self)、JSON (agentxx.client.json)、日志 (agentxx.client.log)
/// - agent 侧业务表见 `agentxx/plugin/api/plugin_api.h` (本头不再包含它 —— 两份契约
///   互相独立, 只有同时编写 agent 侧与 client 侧入口的插件才需要都包含)
#ifndef AGENTXX_CLIENT_PLUGIN_API_H
#define AGENTXX_CLIENT_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pluginxx/api/abi.h"
#include "pluginxx/api/tables.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 全局 API 版本 (client 侧)
#define AGENTXX_CLIENT_PLUGIN_API_VERSION 1

/// 主程序支持的**最低**插件 API 版本 (插件 `AgentxxClientPluginInfo.api_version` 检查)
///
/// 判据只有这一条: `api_version >= AGENTXX_CLIENT_PLUGIN_MIN_API_VERSION`。插件声明更高
/// 版本不被拒绝 (它可能用了主程序没有的能力, 由插件自己读 `AGENTXX_CLIENT_PLUGIN_API_VERSION`
/// 决定是否降级)。与 agent 侧 `PLUGINXX_MIN_API_VERSION` 同一规则。
#define AGENTXX_CLIENT_PLUGIN_MIN_API_VERSION 1

#pragma pack(push, 8)

/* ==================== 插件元信息 ==================== */

typedef struct AgentxxClientPluginInfo {
    int32_t            api_version; ///< 必须 >= AGENTXX_CLIENT_PLUGIN_API_VERSION
    uint32_t           _reserved;   ///< 8 字节补齐
    PluginxxStringView name;        ///< 唯一标识 (与 agent 侧插件共用命名空间)
    PluginxxStringView version;
    PluginxxStringView description;
} AgentxxClientPluginInfo;

/* ==================== client 事件类型枚举 ==================== */

/// 事件类型 (payload 均为 JSON 字符串, 主程序构造)
typedef enum AgentxxClientEvent {
    AGENTXX_CLIENT_EVT_READY = 0,      ///< 服务端就绪 {"interfaces":[...],"sessionId"}
    AGENTXX_CLIENT_EVT_CONN_STATE,     ///< 连接状态变化 {"connState","startupProgress"}
    AGENTXX_CLIENT_EVT_USER_INPUT,     ///< 用户输入已发出 {"sessionId","text"}
    AGENTXX_CLIENT_EVT_DELTA,          ///< 增量事件 (同 wire delta JSON 字段)
    AGENTXX_CLIENT_EVT_TURN_END,       ///< 轮次结束 {"sessionId","hasError","interrupted",...}
    AGENTXX_CLIENT_EVT_SESSION_SWITCH, ///< 会话切换 {"sessionId"}
    AGENTXX_CLIENT_EVT_PLUGIN_DATA, ///< 插件事件转发 (WirePluginData) {"plugin","event","data"}
    /// 展示区域尺寸变化 {"regions":[{"id":"<面板/段落 id>","w":60,"h":20}]}
    /// - 插件按新的可用宽度重新排列自己的组件 (例如换用不同的列宽/省略策略)
    /// - 仅在数值变化时投递 (同一帧内多次上报会合并为一次);
    ///   老主程序对新事件值返回订阅失败 (NULL), 插件应据此降级为固定宽度
    AGENTXX_CLIENT_EVT_UI_LAYOUT,
    AGENTXX_CLIENT_EVT_COUNT
} AgentxxClientEvent;

/* ==================== 展示扩展句柄 (不透明) ==================== */

typedef struct AgentxxStatusItem  AgentxxStatusItem;  ///< 状态栏项句柄
typedef struct AgentxxPanel       AgentxxPanel;       ///< 侧边栏面板句柄
typedef struct AgentxxInfoSection AgentxxInfoSection; ///< 侧边栏 Info 栏段落句柄

/* ==================== 工具特化渲染器 (Tool Renderer / Template) ==================== */

/// 工具渲染输入 (主程序填, 插件读; 全部只读借用)
///
/// 读取方 (插件) 只需认识自己的前缀字段; 以后主程序在尾部追加字段时, 插件读该字段前
/// 用 `agentxx::plugin::specHas<&AgentxxToolRenderInput::字段>(input->struct_size)` 判定。
typedef struct AgentxxToolRenderInput {
    uint32_t           struct_size;  ///< 主程序填 sizeof; 0 = 基线布局
    int32_t            version;      ///< 结构体版本 (当前 == 1)
    PluginxxStringView tool_call_id; ///< 工具调用 ID
    PluginxxStringView tool_name;    ///< 工具名
    PluginxxStringView args_json;    ///< 参数 JSON 字符串
    PluginxxStringView result_text;  ///< 执行结果文本 (未完成时为空)
    int32_t            is_finished;  ///< 0=运行中, 1=已完成
    int32_t            is_error;     ///< 0=正常, 1=错误
    int32_t            max_width;    ///< 渲染内容区可用列宽预算 (<=0 表示不限)
    uint32_t           _pad;         ///< 8 字节对齐
} AgentxxToolRenderInput;

/* 基线: 重置时 = 完整布局; 以后追加字段保持不变 (改为 offsetof(第一个追加字段)) */
#define AGENTXX_TOOL_RENDER_INPUT_BASELINE_SIZE sizeof(AgentxxToolRenderInput)

typedef struct AgentxxToolRenderOutput {
    PluginxxString displayName; ///< 显示名 (如 "Read", "Edit", "Bash", 空则回退原始 toolName)
    PluginxxString summary; ///< 一行摘要 (如 " · [0, 100] /path/file", 可带或不带前导 " · ")
    PluginxxString items_json; ///< 展开体 items JSON 数组 (可选, 空则走默认 args/result 展示;
                               ///< 界面描述层 schema, 如 Text/Button/Diagram/Divider/Diff)
} AgentxxToolRenderOutput;

typedef int32_t(PLUGINXX_CALL* AgentxxToolRenderFn)(
    void*                         user_data,
    const AgentxxToolRenderInput* input,
    AgentxxToolRenderOutput*      output
);

typedef struct AgentxxToolRenderSpec {
    uint32_t            struct_size; ///< sizeof(AgentxxToolRenderSpec); 0 = 基线布局
    int32_t             version;     ///< 结构体版本 (当前 == 1)
    PluginxxStringView  tool_name; ///< 目标工具名 (必填, 如 "agentxx_filesystem_read")
    AgentxxToolRenderFn render_fn; ///< 渲染回调 (可为 NULL; 非空时优先调用)
    void*               user_data; ///< 回调上下文 (render_fn 非空时有效)
    PluginxxStringView template_json; ///< 预设模版 JSON (render_fn 为 NULL 时由主程序解析执行)
} AgentxxToolRenderSpec;

/* 基线: 重置时 = 完整布局; 以后追加字段保持不变 (改为 offsetof(第一个追加字段)) */
#define AGENTXX_TOOL_RENDER_SPEC_BASELINE_SIZE sizeof(AgentxxToolRenderSpec)

/* ==================== 通用交互: 动作回调与 overlay ==================== */

/// 动作派发上下文 (主程序在 client io 线程构造, 仅本次回调有效; 全部只读借用)
typedef struct AgentxxUiActionContext {
    uint32_t           struct_size; ///< 主程序填 sizeof; 0 = 基线布局
    int32_t            version;     ///< 结构体版本 (当前 == 1)
    PluginxxStringView owner_id;    ///< 归属: section_id / panel_id / tool_call_id / "__overlay"
    PluginxxStringView action_id;   ///< 按钮声明的 action_id
    PluginxxStringView action_args; ///< 参数 JSON object dump (空 = 无参)
} AgentxxUiActionContext;

/* 基线: 重置时 = 完整布局; 以后追加字段保持不变 (改为 offsetof(第一个追加字段)) */
#define AGENTXX_UI_ACTION_CONTEXT_BASELINE_SIZE sizeof(AgentxxUiActionContext)

/// 动作处理器 (client io 线程同步调用; 不得抛异常出边界, 主程序兜底)
typedef void(PLUGINXX_CALL* AgentxxUiActionFn)(const AgentxxUiActionContext* ctx, void* user_data);

/// 通用 overlay 类型 (首版四种, 一次实现)
typedef enum AgentxxOverlayType {
    AGENTXX_OVERLAY_MERMAID = 0, ///< 状态图: payload = mermaid 源码
    AGENTXX_OVERLAY_TEXT    = 1, ///< 文本/Markdown: payload = 原文
    AGENTXX_OVERLAY_DIFF    = 2, ///< 对比: payload = JSON {path,old_str,new_str}
    AGENTXX_OVERLAY_CUSTOM = 3 ///< 自定义: payload = JSON {"items":[...]} (同 panel/items schema)
} AgentxxOverlayType;

/// 通用 overlay 参数 (全部只读借用, 主程序拷贝后使用)
typedef struct AgentxxOverlaySpec {
    int32_t            version;     ///< 结构体版本 (当前 == 1)
    uint32_t           struct_size; ///< sizeof(AgentxxOverlaySpec); 0 = 基线布局
    int32_t            type;        ///< AgentxxOverlayType
    PluginxxStringView title;       ///< 标题 (空则主程序回退默认)
    PluginxxStringView payload;     ///< 内容 (语义见 AgentxxOverlayType)
    PluginxxStringView extra_json;  ///< 扩展 JSON object (可空)
    /// - TEXT: `{"markdown":bool}` (缺省按 markdown 渲染)
    /// - 通用尺寸与外观: `{"size":"auto|compact|normal|large|full",
    ///   "width_frac":0.8,"height_frac":0.9,"footer":bool,"scroll":bool,"stack":bool}`
    ///   (`size` 与显式比例同时给出时比例优先; `stack=true` 表示已有 overlay 时保留
    ///   现有的, 缺省为替换当前 overlay)
    /// - 未知键被忽略 (老主程序同样忽略), 不影响其余字段生效
} AgentxxOverlaySpec;

/* 基线: 重置时 = 完整布局; 以后追加字段保持不变 (改为 offsetof(第一个追加字段)) */
#define AGENTXX_OVERLAY_SPEC_BASELINE_SIZE sizeof(AgentxxOverlaySpec)

/// CUSTOM overlay 内按钮归属 (owner_id 固定值, 走实例级 fallback 派发)
#define AGENTXX_CLIENT_OVERLAY_OWNER "__overlay"

/* ==================== 接口表: 展示/命令/toast (agentxx.client.ui) ==================== */

#define AGENTXX_IFACE_CLIENT_UI             "agentxx.client.ui"
#define AGENTXX_IFACE_CLIENT_UI_VERSION     1   /* 追加成员 / 语义变化 -> +1 */
#define AGENTXX_IFACE_CLIENT_UI_MIN_VERSION 1   /* 只在破坏性改动时 +1 (优先换 IID) */
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_IFACE_CLIENT_UI_BASELINE_SIZE sizeof(AgentxxClientUiIface)

typedef struct AgentxxClientUiIface {
    int32_t  version; ///< 主程序填 AGENTXX_IFACE_CLIENT_UI_VERSION
    uint32_t struct_size;

    /* ---- 状态栏项 ---- */
    /// 注册状态栏项; 返回句柄 (主程序持有; 卸载自动清理)
    /// - id: 全局唯一, 建议 "{插件名}.{项名}" (如 "codegraph.index")
    /// - initialJson: {"text": "...", "tooltip": "..."} (text 必填)
    /// - align: 0=左侧 1=右侧; order: 组内排序 (小在前)
    /// - 主程序不支持该子能力 (函数指针 NULL) 或 id 冲突时返回 NULL
    AgentxxStatusItem*(PLUGINXX_CALL* register_status_item)(
        const PluginxxHost*       host,
        const PluginxxStringView* id,
        const PluginxxStringView* initialJson,
        int32_t                   align,
        int32_t                   order
    );
    /// 更新状态栏项文本 ({"text": "..."}); 句柄无效返回非 0
    int32_t(PLUGINXX_CALL* update_status_item)(
        const PluginxxHost*       host,
        AgentxxStatusItem*        item,
        const PluginxxStringView* json
    );
    /// 注销状态栏项 (句柄随后失效)
    void(PLUGINXX_CALL* unregister_status_item)(const PluginxxHost* host, AgentxxStatusItem* item);

    /* ---- 侧边栏面板 ---- */
    /// 注册侧边栏面板; 返回句柄 (主程序持有; 卸载自动清理)
    /// - id: 全局唯一, 建议 "{插件名}.{面板名}"
    /// - propsJson: {"title": "..."} (title 必填; 显示在 tab 栏)
    AgentxxPanel*(PLUGINXX_CALL* register_panel)(
        const PluginxxHost*       host,
        const PluginxxStringView* id,
        const PluginxxStringView* propsJson
    );
    /// 更新面板内容: itemsJson = {"items":[ …组件项… ]}
    /// - 组件来自**界面描述层** (`pluginxx::ui`, 独立库 cxx_pluginxx_ui): 规范写法是
    ///   组件名 PascalCase、字段 camelCase, 例:
    ///   {"kind":"Text","text":"CPU","type":"body","tone":"normal"},
    ///   {"kind":"Progress","label":"cpu","value":0.5},
    ///   {"kind":"Button","label":"Rebuild","action":{"kind":"dispatch","name":"rebuild"}}
    /// - 文本样式用 `tone` (normal/hint/accent/title/error/…) 与 `type`
    ///   (body/caption/title); 插件用 SDK 的构建器 (`pluginxx::ui::build`) 或 kit
    ///   拼装即可, 不必手写 JSON
    /// - 未知组件与本客户端不支持的组件由主程序按自身能力降级 (最多降到文本),
    ///   不会因此清空面板; 客户端支持哪些组件见 `get_client_state().ui`
    /// - button 项被用户点击时: 主程序 post 到 client io 线程按动作派发
    ///   (dispatch 走能力/动作绑定, 见 `bind_action_handler`)
    int32_t(PLUGINXX_CALL* update_panel)(
        const PluginxxHost*       host,
        AgentxxPanel*             panel,
        const PluginxxStringView* itemsJson
    );
    void(PLUGINXX_CALL* unregister_panel)(const PluginxxHost* host, AgentxxPanel* panel);

    /* ---- 侧边栏 Info 栏段落 ---- */
    /// 注册 Info 栏段落; 返回句柄 (主程序持有; 卸载自动清理)
    /// - id: 全局唯一, 建议 "{插件名}.{段名}"
    /// - propsJson: {"title": "..."} (title 可选; 空则无段落标题)
    AgentxxInfoSection*(PLUGINXX_CALL* register_info_section)(
        const PluginxxHost*       host,
        const PluginxxStringView* id,
        const PluginxxStringView* propsJson
    );
    /// 更新 Info 栏段落内容: itemsJson 同 update_panel 的 items schema
    ///   (如 {"kind":"Text","text":"index: 42","tone":"hint"});
    ///   列表项由主程序按侧边栏 Append 段样式以 "|  xxx" 前缀展示
    int32_t(PLUGINXX_CALL* update_info_section)(
        const PluginxxHost*       host,
        AgentxxInfoSection*       section,
        const PluginxxStringView* itemsJson
    );
    /// 注销 Info 栏段落 (句柄随后失效)
    void(PLUGINXX_CALL* unregister_info_section)(
        const PluginxxHost* host,
        AgentxxInfoSection* section
    );

    /* ---- 斜杠命令 ---- */
    /// 注册斜杠命令: 用户输入 "/{name}" 触发 (name 不含 '/' 与空格)
    /// - name: 全局唯一; description: 帮助/自动补全用
    /// - execute: client io 线程同步调用; 返回动作 JSON (host->alloc), 失败返回
    ///   空串并经 errorOut 输出错误 (host->alloc); 主程序解释动作 (见文件头)
    /// - 返回 0 成功; 名字冲突或参数非法返回非 0
    int32_t(PLUGINXX_CALL* register_command)(
        const PluginxxHost*       host,
        const PluginxxStringView* name,
        const PluginxxStringView* description,
        int32_t(PLUGINXX_CALL* execute)(
            void*                     ud,
            const PluginxxStringView* argsJson,
            PluginxxString*           actionOut,
            PluginxxString*           errorOut
        ),
        void* ud
    );
    int32_t(PLUGINXX_CALL* unregister_command)(
        const PluginxxHost*       host,
        const PluginxxStringView* name
    );

    /* ---- toast 提示 ---- */
    /// 显示 toast 提示 (level: 0=info 1=warning 2=error; 实现可忽略级别差异)
    void(PLUGINXX_CALL*
             show_toast)(const PluginxxHost* host, const PluginxxStringView* text, int32_t level);

    /* ---- 工具消息装饰 ---- */
    /// 更新/删除本插件对某次工具调用的消息装饰 (io 线程约束):
    /// - 装饰内容为语义 JSON, UI 按通用渲染器展示: 折叠头 displayName/summary +
    ///   展开体 items (界面描述层 schema, 同 update_panel)
    ///   {"displayName": "Plan",              // 可选; 折叠头显示名 (缺省原始 toolName)
    ///    "summary": "[~] a; [ ] b",          // 可选; 折叠头一行摘要 (缺省回退参数预览)
    ///    "items": [                          // 可选; 展开体内容
    ///      {"kind":"Text","text":"Todos:","type":"title"},
    ///      {"kind":"Text","text":"[~] do task A","tone":"hint"},
    ///      {"kind":"Diagram","mermaid":"stateDiagram-v2..."} ]}
    /// - 按钮 (可点): {"kind":"Button","label":"Graph",
    ///   "action":{"kind":"dispatch","name":"planning.open_graph"}}
    /// - 不使用构建器时按上面的规范写法手写即可 (组件名 PascalCase, 字段 camelCase);
    ///   本客户端不支持的组件由主程序按能力降级, 不会丢内容
    /// - tool_call_id: 目标工具调用 id (取自 DELTA/tool_start 的 tool_call_id);
    ///   空视图 = 操作本插件的全部装饰
    /// - decor_json: 空串 = 删除 (tool_call_id 为空时删除本插件全部);
    ///   主程序在插件卸载/禁用时自动摘除其全部装饰, 启用时恢复
    /// - 典型数据源: 订阅 EVT_DELTA (tool_start 携带完整 arguments) 推送;
    ///   参考实现: agentxx_planning (Plan 渲染完全由插件驱动, TUI 无特化代码)
    /// 返回 0 成功; 非 0 失败 (主程序不支持/JSON 非法)
    int32_t(PLUGINXX_CALL* update_tool_decor)(
        const PluginxxHost*       host,
        const PluginxxStringView* tool_call_id,
        const PluginxxStringView* decor_json
    );

    /* ---- 工具特化渲染器 (按 tool_name 注册) ---- */
    /// 注册工具特化渲染器 (按 tool_name 键控):
    /// - 主程序在渲染该工具消息 (折叠头/展开体) 时调用, 无论实时流式还是历史消息
    /// - 支持两种模式:
    ///   1. <key, render_fn>: spec->render_fn != NULL, 主程序回调该函数输出
    ///   displayName/summary/items_json
    ///   2. 预设模版: spec->render_fn == NULL 且 template_json 非空,
    ///      主程序按模版自动提取参数字段并格式化 (如 {"displayName":"Search","summaryKey":"query"})
    /// - 展开体 items_json 支持 {"kind":"Diff","path":"...","oldStr":"...","newStr":"..."}
    /// - 插件卸载/禁用时主程序自动注销
    /// 返回 0 成功, 非 0 失败
    int32_t(PLUGINXX_CALL* register_tool_renderer)(
        const PluginxxHost*          host,
        const AgentxxToolRenderSpec* spec
    );
    int32_t(PLUGINXX_CALL* unregister_tool_renderer)(
        const PluginxxHost*       host,
        const PluginxxStringView* tool_name
    );

    /* ---- 通用交互 (动作回调与 overlay)
     *
     * 本段成员属于本表的首版布局 (= 基线), 因此只判空即可: 成员为 NULL 表示主程序
     * 不支持该子能力 (精简客户端), 插件据此降级 (能力名协商: `agentxx.client.action`
     * / `agentxx.client.overlay`)。
     * 以后新增能力优先走"新接口表或新能力名"; 确实要往本表加成员时只能追加在表尾,
     * `_BASELINE_SIZE` 保持当前值并用 `pluginxx::ifaceHas` 判定 (见
     * docs/zh-cn/design/plugins.md §9 的追加策略)。
     */
    /// 绑定动作处理器 (client io 线程约束):
    /// - target_id: 归属键 (section_id / panel_id / tool_call_id); 空串 = 本实例
    ///   兜底 (方案 A fallback: 精确匹配优先, 未命中回落到 "")
    /// - 同一 (plugin, target_id) 重复绑定覆盖; on_action 为 NULL 时返回非 0
    /// - 返回 0 成功
    int32_t(PLUGINXX_CALL* bind_action_handler)(
        const PluginxxHost*       host,
        const PluginxxStringView* target_id,
        AgentxxUiActionFn         on_action,
        void*                     user_data
    );
    /// 解绑动作处理器 (不存在忽略, 返回 0)
    int32_t(PLUGINXX_CALL* unbind_action_handler)(
        const PluginxxHost*       host,
        const PluginxxStringView* target_id
    );
    /// 通用 overlay (client io 线程约束; 插件 on_action 内直接调用):
    /// - 已有 overlay/核心弹窗时的策略由 spec->extra_json 的 `stack` 决定:
    ///   缺省替换当前 overlay (last-wins), `stack=true` 时保留现有 overlay 并丢弃本次
    /// - spec->version 必须 == 1; type 越界返回非 0
    /// - 主程序拷贝字符串后 postToUi, 不阻塞插件
    /// - 返回 0 成功
    int32_t(PLUGINXX_CALL* open_overlay)(const PluginxxHost* host, const AgentxxOverlaySpec* spec);
    /// 关闭当前 overlay (任何插件都可关; manager 传插件名仅记日志/鉴权预留)
    void(PLUGINXX_CALL* close_overlay)(const PluginxxHost* host);
} AgentxxClientUiIface;

/* ==================== 接口表: 事件订阅 (agentxx.client.events) ==================== */

#define AGENTXX_IFACE_CLIENT_EVENTS             "agentxx.client.events"
#define AGENTXX_IFACE_CLIENT_EVENTS_VERSION     1
#define AGENTXX_IFACE_CLIENT_EVENTS_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_IFACE_CLIENT_EVENTS_BASELINE_SIZE sizeof(AgentxxClientEventsIface)

typedef struct AgentxxClientEventsIface {
    int32_t  version; ///< 主程序填 AGENTXX_IFACE_CLIENT_EVENTS_VERSION
    uint32_t struct_size;

    /// 订阅 client 事件 (payload JSON 字符串; 卸载自动退订); event 为
    /// AgentxxClientEvent 枚举值; 失败返回 NULL
    PluginxxSubscription*(PLUGINXX_CALL* subscribe)(
        const PluginxxHost* host,
        int32_t             event, /* AgentxxClientEvent */
        void(PLUGINXX_CALL* handler)(const PluginxxStringView* payloadJson, void* ud),
        void* ud
    );
    void(PLUGINXX_CALL* unsubscribe)(PluginxxSubscription* sub);
} AgentxxClientEventsIface;

/* ==================== 接口表: 会话上下文与操作 (agentxx.client.session) ==================== */

#define AGENTXX_IFACE_CLIENT_SESSION             "agentxx.client.session"
#define AGENTXX_IFACE_CLIENT_SESSION_VERSION     1
#define AGENTXX_IFACE_CLIENT_SESSION_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_IFACE_CLIENT_SESSION_BASELINE_SIZE sizeof(AgentxxClientSessionIface)

typedef struct AgentxxClientSessionIface {
    int32_t  version; ///< 主程序填 AGENTXX_IFACE_CLIENT_SESSION_VERSION
    uint32_t struct_size;

    /// 当前 client 状态 JSON 快照 (host->alloc):
    /// {"sessionId","connState","model","models":[],"isStreaming",
    ///  "interfaces":["agentxx.client.panel",...],
    ///  "ui":{"apiVersion","kind","blocks":[],"controls":[],"cell":{},"gap":12,
    ///        "icons":[],"percent":true,"aspect":true,"limits":{}},
    ///  "regions":[{"id","w","h"},...],
    ///  "agentPlugins":[{"name","version","interfaces":[...]},...]}
    /// - interfaces: 主程序支持的接口名 / 能力名清单 (插件据此决定启用哪些功能)
    /// - ui: 客户端界面能力段 (界面描述层) —— 客户端能画哪些组件/控件、每个字符格
    ///   相当于多少 u、默认行距; 老主程序 / 未发布的客户端没有该字段, 插件应按
    ///   "描述层全支持"处理 (内容仍由客户端按自身能力降级)
    /// - regions: 展示区域尺寸快照 (面板/Info 段落可用宽高)
    /// (model/models/agentPlugins 依赖服务端推送; 未收到时为空)
    int32_t(PLUGINXX_CALL* get_client_state)(const PluginxxHost* host, PluginxxString* out);
    /// 代发一条用户消息 (sessionId 与当前会话不符时仍按当前会话发送并记日志)
    /// - 与用户输入同排队语义 (流式中进 pendingInputs), 不绕过 UI 状态机
    /// - 返回 0 成功; 非 0 表示主程序不可用 (未连接等)
    int32_t(PLUGINXX_CALL* send_user_input)(
        const PluginxxHost*       host,
        const PluginxxStringView* sessionId,
        const PluginxxStringView* text
    );
    /// 请求取消当前会话轮次 (与用户按 Esc 等价)
    void(PLUGINXX_CALL* request_cancel)(
        const PluginxxHost*       host,
        const PluginxxStringView* sessionId
    );
} AgentxxClientSessionIface;

/* ==================== 接口表: 跨端数据通道 (agentxx.client.wire) ==================== */

#define AGENTXX_IFACE_CLIENT_WIRE             "agentxx.client.wire"
#define AGENTXX_IFACE_CLIENT_WIRE_VERSION     1
#define AGENTXX_IFACE_CLIENT_WIRE_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_IFACE_CLIENT_WIRE_BASELINE_SIZE sizeof(AgentxxClientWireIface)

typedef struct AgentxxClientWireIface {
    int32_t  version; ///< 主程序填 AGENTXX_IFACE_CLIENT_WIRE_VERSION
    uint32_t struct_size;

    /// 发送事件到 agent 侧: 服务端发布到事件总线 topic `client.{插件名}.{event}`
    /// (agent 侧同名插件可订阅; 载荷 JSON 原样透传, 语义由插件定义)
    /// - 返回 0 成功; 非 0 表示未连接或载荷非法
    int32_t(PLUGINXX_CALL* send_plugin_data)(
        const PluginxxHost*       host,
        const PluginxxStringView* event,
        const PluginxxStringView* json
    );
} AgentxxClientWireIface;

/* ==================== 接口表: 自描述/配置 (agentxx.client.self) ==================== */

#define AGENTXX_IFACE_CLIENT_SELF             "agentxx.client.self"
#define AGENTXX_IFACE_CLIENT_SELF_VERSION     1
#define AGENTXX_IFACE_CLIENT_SELF_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_IFACE_CLIENT_SELF_BASELINE_SIZE sizeof(AgentxxClientSelfIface)

typedef struct AgentxxClientSelfIface {
    int32_t  version; ///< 主程序填 AGENTXX_IFACE_CLIENT_SELF_VERSION
    uint32_t struct_size;

    /// 本插件信息 JSON {"name","version","description","path"}
    /// (加载时常用: 从 path 推导资源目录; host->alloc)
    int32_t(PLUGINXX_CALL* get_own_info)(const PluginxxHost* host, PluginxxString* out);
    /// 本插件配置参数 JSON (yaml `plugins` 条目 args; io 线程; host->alloc):
    /// 主程序不解析 args 字段语义, 整体原样传递; 未配置时返回 "{}"
    int32_t(PLUGINXX_CALL* get_plugin_args)(const PluginxxHost* host, PluginxxString* out);
    /// 本插件配置文件所在目录或文件路径 (yaml `plugins` 条目 config; io 线程;
    /// host->alloc; 未指定返回空串)
    /// - 可指向文件或目录 (由插件自行判断类型并加载)
    /// - 主程序已归一化为绝对路径 (正斜杠, lexically_normal)
    int32_t(PLUGINXX_CALL* get_plugin_config_path)(const PluginxxHost* host, PluginxxString* out);
    /// 读取当前 client 使用的语言 (client io 线程; 返回 0 成功, out 填入如 "en" / "zh-cn",
    /// host->alloc 分配)
    int32_t(PLUGINXX_CALL* get_language)(const PluginxxHost* host, PluginxxString* out);
    /// 指定使用的语言 (client io 线程; 返回 0 成功; 不支持 auto, 为空或 auto 时回退为 "en")
    int32_t(PLUGINXX_CALL* set_language)(
        const PluginxxHost*       host,
        const PluginxxStringView* language
    );
} AgentxxClientSelfIface;

/* ==================== 接口表: JSON 辅助 (agentxx.client.json) ==================== */

#define AGENTXX_IFACE_CLIENT_JSON             "agentxx.client.json"
#define AGENTXX_IFACE_CLIENT_JSON_VERSION     1
#define AGENTXX_IFACE_CLIENT_JSON_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_IFACE_CLIENT_JSON_BASELINE_SIZE sizeof(AgentxxClientJsonIface)

typedef struct AgentxxClientJsonIface {
    int32_t  version; ///< 主程序填 AGENTXX_IFACE_CLIENT_JSON_VERSION
    uint32_t struct_size;

    /// 从 JSON 字符串提取指定 key 的字符串值 (主程序解析; 结果 host->alloc)
    int32_t(PLUGINXX_CALL* json_get_string)(
        const PluginxxHost*       host,
        const PluginxxStringView* json,
        const PluginxxStringView* key,
        PluginxxString*           out
    );
    /// 字符串 → JSON 字符串字面量 (含引号包裹与转义; 结果 host->alloc)
    int32_t(PLUGINXX_CALL* json_escape)(
        const PluginxxHost*       host,
        const PluginxxStringView* s,
        PluginxxString*           out
    );
} AgentxxClientJsonIface;

/* ==================== 接口表: 日志 (agentxx.client.log) ==================== */

#define AGENTXX_IFACE_CLIENT_LOG             "agentxx.client.log"
#define AGENTXX_IFACE_CLIENT_LOG_VERSION     1
#define AGENTXX_IFACE_CLIENT_LOG_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_IFACE_CLIENT_LOG_BASELINE_SIZE sizeof(AgentxxClientLogIface)

typedef struct AgentxxClientLogIface {
    int32_t  version; ///< 主程序填 AGENTXX_IFACE_CLIENT_LOG_VERSION
    uint32_t struct_size;

    /// 日志 (线程安全; 0=trace 1=debug 2=info 3=warn 4=error)
    void(PLUGINXX_CALL*
             log)(const PluginxxHost* host, int32_t level, const PluginxxStringView* msg);
} AgentxxClientLogIface;

/* ==================== 接口表: 定时器 (agentxx.client.timer) ==================== */

#define AGENTXX_IFACE_CLIENT_TIMER             "agentxx.client.timer"
#define AGENTXX_IFACE_CLIENT_TIMER_VERSION     1
#define AGENTXX_IFACE_CLIENT_TIMER_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_IFACE_CLIENT_TIMER_BASELINE_SIZE sizeof(AgentxxClientTimerIface)

/// 定时器句柄 (主程序拥有; 取消或实例卸载后失效)
typedef struct AgentxxTimer AgentxxTimer;

/// 定时器参数 (全部只读借用, 主程序拷贝后使用)
typedef struct AgentxxTimerSpec {
    uint32_t struct_size; ///< sizeof(AgentxxTimerSpec); 0 = 基线布局
    int32_t  version;     ///< 结构体版本 (当前 == 1)
    /// 触发间隔毫秒; 主程序持有下限 (当前 50, 更小按 50 处理)
    int32_t interval_ms;
    /// 0 = 仅触发一次; > 0 = 周期触发 (回调次数上限, 到达后自动取消)
    int32_t repeat;
    /// 非 0: 关联区域 ([owner_id]) 不可见时跳过本次回调并顺延 (计时继续)
    /// - 顺延期间不消耗触发次数: 一次性定时器在区域可见后的下一次到时触发,
    ///   周期定时器只统计真正回调的次数
    /// - 高频 (< 200ms) 刷新面板/overlay 的插件应置 1, 避免不可见时白耗 CPU
    int32_t pause_when_hidden;
    /// 关联的展示区域 id (空 = 无关联, [is_visible] 恒为 0)
    /// - 可取值: 面板 id / Info 段落 id / 通用 overlay (`"__overlay"`) /
    ///   工具消息装饰的 `tool_call_id` (主程序按该消息是否在消息列表角度内判定)
    PluginxxStringView owner_id;
    /// 超时回调 (client io 线程同步调用; 不得抛异常出边界, 主程序兜底)
    void(PLUGINXX_CALL* on_timer)(void* user_data);
    /// 回调上下文 (建议 = 插件实例上下文; 可为 NULL)
    void* user_data;
} AgentxxTimerSpec;

/* 基线: 重置时 = 完整布局; 以后追加字段保持不变 (改为 offsetof(第一个追加字段)) */
#define AGENTXX_TIMER_SPEC_BASELINE_SIZE sizeof(AgentxxTimerSpec)

typedef struct AgentxxClientTimerIface {
    int32_t  version; ///< 主程序填 AGENTXX_IFACE_CLIENT_TIMER_VERSION
    uint32_t struct_size;

    /// 注册定时器 (client io 线程; 返回句柄, 失败返回 NULL)
    /// - 失败原因: spec 非法 (版本/回调为空) / 主程序动画等级为 Disabled /
    ///   单实例定时器数量超上限 (当前 8)
    /// - 句柄由主程序持有, 实例禁用/卸载时自动取消 (插件无需在 stop 里逐个取消,
    ///   但主动取消更及时)
    AgentxxTimer*(PLUGINXX_CALL* set_timer)(
        const PluginxxHost*       host,
        const AgentxxTimerSpec*   spec
    );
    /// 取消定时器 (io 线程; 句柄随后失效, 重复取消/空句柄忽略)
    void(PLUGINXX_CALL* cancel_timer)(const PluginxxHost* host, AgentxxTimer* timer);
    /// 关联区域当前是否可见 (1/0; owner_id 为空或主程序未上报过返回 0)
    int32_t(PLUGINXX_CALL* is_visible)(
        const PluginxxHost*       host,
        const PluginxxStringView* owner_id
    );
} AgentxxClientTimerIface;

/* ==================== 接口表: 全局快捷键 (agentxx.client.keybind) ==================== */

#define AGENTXX_IFACE_CLIENT_KEYBIND             "agentxx.client.keybind"
#define AGENTXX_IFACE_CLIENT_KEYBIND_VERSION     1
#define AGENTXX_IFACE_CLIENT_KEYBIND_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_IFACE_CLIENT_KEYBIND_BASELINE_SIZE sizeof(AgentxxClientKeybindIface)

/// 快捷键句柄 (主程序拥有; 注销或实例卸载后失效)
typedef struct AgentxxKeybind AgentxxKeybind;

/// 快捷键参数 (全部只读借用, 主程序拷贝后使用)
typedef struct AgentxxKeybindSpec {
    uint32_t struct_size; ///< sizeof(AgentxxKeybindSpec); 0 = 基线布局
    int32_t  version;     ///< 结构体版本 (当前 == 1)
    /// 键位描述: 修饰键 + 主键, 用 `+` 连接, 不区分大小写与前后空格
    /// - 修饰键: `ctrl` (或 `control`) / `alt` / `shift` / `super` (或 `cmd`/`win`)
    /// - 主键: 单个可打印字符 (a-z / 0-9 / 符号) 或
    ///   `f1`~`f24` / `esc` / `enter` / `tab` / `space` / `backspace` / `delete` /
    ///   `up` / `down` / `left` / `right` / `home` / `end` / `pageup` / `pagedown` /
    ///   `insert`
    /// - 例: `"ctrl+alt+k"` / `"f9"` / `"ctrl+shift+space"`
    /// - 无修饰键的可打印字符**不参与匹配** (避免与输入框抢字符), 注册会失败
    PluginxxStringView keys;
    /// 说明文本 (界面/帮助展示; 可空)
    PluginxxStringView description;
    /// 触发回调 (client io 线程同步调用; 不得抛异常出边界, 主程序兜底)
    void(PLUGINXX_CALL* on_keybind)(void* user_data);
    /// 回调上下文 (建议 = 插件实例上下文; 可为 NULL)
    void* user_data;
} AgentxxKeybindSpec;

/* 基线: 重置时 = 完整布局; 以后追加字段保持不变 (改为 offsetof(第一个追加字段)) */
#define AGENTXX_KEYBIND_SPEC_BASELINE_SIZE sizeof(AgentxxKeybindSpec)

typedef struct AgentxxClientKeybindIface {
    int32_t  version; ///< 主程序填 AGENTXX_IFACE_CLIENT_KEYBIND_VERSION
    uint32_t struct_size;

    /// 注册全局快捷键 (client io 线程; 返回句柄, 失败返回 NULL)
    /// - 失败原因: 键位无法解析 / 无修饰键的可打印字符 / 已被其它实例占用
    ///   (先注册者优先) / 单实例数量超上限 (当前 16)
    /// - 快捷键在表单控件有焦点时同样生效 (全局优先级最高); 输入框内输入字符
    ///   不受影响 (可打印字符本身不参与匹配)
    AgentxxKeybind*(PLUGINXX_CALL* register_keybind)(
        const PluginxxHost*        host,
        const AgentxxKeybindSpec*  spec
    );
    /// 注销快捷键 (io 线程; 句柄随后失效, 重复注销/空句柄忽略)
    void(PLUGINXX_CALL* unregister_keybind)(const PluginxxHost* host, AgentxxKeybind* bind);
    /// 当前已注册的快捷键列表 (io 线程; JSON 数组, 元素
    /// `{"keys","description","plugin"}`; 结果 host->alloc, 空列表返回 `[]`)
    int32_t(PLUGINXX_CALL* list_keybinds)(const PluginxxHost* host, PluginxxString* out);
} AgentxxClientKeybindIface;

/* ==================== 插件入口符号 (dlsym) ==================== */

/// 可选: 查询插件元信息 (加载前调用, 用于版本/信息校验; 未导出则跳过;
/// 纯静态元数据, 不得读取/依赖任何实例状态)
typedef const AgentxxClientPluginInfo*(PLUGINXX_CALL* AgentxxClientPluginGetInfoFn)(void);
/// 必需: client 侧插件实例创建 (主程序线程池调用; 内部注册动作主程序自动投递回
/// client io 线程; 语义同 agent 侧 agentxx_plugin_agent_create)
/// 【多实例契约】可重入, 每次调用产出独立存活实例 —— 一切实例状态只能
/// 存于 *plugin_ctx 指向的堆块 (禁止可变全局/函数级 static 缓存); 一切注册
/// 回调必须设置 user_data = 实例上下文。
/// - host: 本实例专属主程序句柄 (opaque 已关联本实例)
/// - plugin_ctx: 输出本实例私有上下文 (透传给 destroy)
/// - 返回 0 成功; 非 0 创建失败 (主程序走失败清理路径并报告错误)
typedef int32_t(PLUGINXX_CALL* AgentxxClientPluginCreateFn)(
    const PluginxxHost* host,
    void**              plugin_ctx
);
/// 可选: 插件实例销毁 (主程序等全部执行中回调完成后调用; 主程序会在此之前自动
/// 反注册该实例的一切 status item/panel/command/订阅)。只销毁对应 create
/// 产出的实例上下文, 与其他并存实例无关。
typedef void(PLUGINXX_CALL* AgentxxClientPluginDestroyFn)(void* plugin_ctx);

#define AGENTXX_PLUGIN_CLIENT_SYMBOL_GET_INFO "agentxx_plugin_client_get_info"
#define AGENTXX_PLUGIN_CLIENT_SYMBOL_CREATE   "agentxx_plugin_client_create"
#define AGENTXX_PLUGIN_CLIENT_SYMBOL_START    "agentxx_plugin_client_start"
#define AGENTXX_PLUGIN_CLIENT_SYMBOL_STOP     "agentxx_plugin_client_stop"
#define AGENTXX_PLUGIN_CLIENT_SYMBOL_DESTROY  "agentxx_plugin_client_destroy"

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

#endif /* AGENTXX_CLIENT_PLUGIN_API_H */
