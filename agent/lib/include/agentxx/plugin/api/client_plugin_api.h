/*
 * agentxx/plugin/client_plugin_api.h —— client 侧插件系统纯 C ABI 契约 (API v1 重构版)
 *
 * ════════════════════════════════════════════════════════════════════
 * 架构: COM 风格接口表查询
 * ════════════════════════════════════════════════════════════════════
 * - 明确字节对齐: 全部跨边界 ABI 结构体严格遵循 8 字节对齐 (#pragma pack(push, 8))
 * - 明确基本类型: 统一使用定长基本类型 (int32_t, int64_t, uint64_t)，杜绝跨平台整型宽度差异
 * - 明确函数调用约定: 接口表函数指针、入口符号与回调全部显式标注 AGENTXX_PLUGIN_CALL (__stdcall)
 * - 结构体传递与返回值规范:
 *   * 结构体入参统一采用指针传递 (const Struct*)
 *   * 结构体返回值统一改为函数出参 (Struct* out) 并返回 int32_t 状态码
 * - 版本策略:
 *   * 全局 AGENTXX_CLIENT_PLUGIN_API_VERSION 严格匹配门禁 (当前为 1)
 *   * 全部接口表版本统一重置为 1
 */
#ifndef AGENTXX_CLIENT_PLUGIN_API_H
#define AGENTXX_CLIENT_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "agentxx/plugin/api/plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 全局 API 版本 (client 侧)
#define AGENTXX_CLIENT_PLUGIN_API_VERSION 1

#pragma pack(push, 8)

/* ==================== 插件元信息 ==================== */

typedef struct AgentxxClientPluginInfo {
    int32_t                 api_version; ///< 必须 == AGENTXX_CLIENT_PLUGIN_API_VERSION
    uint32_t                _reserved;   ///< 8 字节补齐
    AgentxxPluginStringView name;        ///< 唯一标识 (与 agent 侧插件共用命名空间)
    AgentxxPluginStringView version;
    AgentxxPluginStringView description;
} AgentxxClientPluginInfo;

/* ==================== client 事件类型枚举 ==================== */

/// 事件类型 (payload 均为 JSON 字符串, 宿主构造)
typedef enum AgentxxClientEvent {
    AGENTXX_CLIENT_EVT_READY = 0,      ///< 服务端就绪 {"interfaces":[...],"sessionId"}
    AGENTXX_CLIENT_EVT_CONN_STATE,     ///< 连接状态变化 {"connState","startupProgress"}
    AGENTXX_CLIENT_EVT_USER_INPUT,     ///< 用户输入已发出 {"sessionId","text"}
    AGENTXX_CLIENT_EVT_DELTA,          ///< 增量事件 (同 wire delta JSON 字段)
    AGENTXX_CLIENT_EVT_TURN_END,       ///< 轮次结束 {"sessionId","hasError","interrupted",...}
    AGENTXX_CLIENT_EVT_SESSION_SWITCH, ///< 会话切换 {"sessionId"}
    AGENTXX_CLIENT_EVT_PLUGIN_DATA, ///< 插件事件转发 (WirePluginData) {"plugin","event","data"}
    AGENTXX_CLIENT_EVT_COUNT
} AgentxxClientEvent;

/* ==================== 展示扩展句柄 (不透明) ==================== */

typedef struct AgentxxStatusItem  AgentxxStatusItem;  ///< 状态栏项句柄
typedef struct AgentxxPanel       AgentxxPanel;       ///< 侧边栏面板句柄
typedef struct AgentxxInfoSection AgentxxInfoSection; ///< 侧边栏 Info 栏段落句柄

/* ==================== 工具特化渲染器 (Tool Renderer / Template) ==================== */

typedef struct AgentxxToolRenderInput {
    int32_t                 version;      ///< 结构体版本 (必须 == 1)
    uint32_t                _reserved;    ///< 8 字节对齐
    AgentxxPluginStringView tool_call_id; ///< 工具调用 ID
    AgentxxPluginStringView tool_name;    ///< 工具名
    AgentxxPluginStringView args_json;    ///< 参数 JSON 字符串
    AgentxxPluginStringView result_text;  ///< 执行结果文本 (未完成时为空)
    int32_t                 is_finished;  ///< 0=运行中, 1=已完成
    int32_t                 is_error;     ///< 0=正常, 1=错误
    int32_t                 max_width;    ///< 渲染内容区可用列宽预算 (<=0 表示不限)
    uint32_t                _pad;         ///< 8 字节对齐
} AgentxxToolRenderInput;

typedef struct AgentxxToolRenderOutput {
    AgentxxPluginString displayName; ///< 显示名 (如 "Read", "Edit", "Bash", 空则回退原始 toolName)
    AgentxxPluginString summary;     ///< 一行摘要 (如 " · [0, 100] /path/file", 可带或不带前导 " · ")
    AgentxxPluginString items_json;  ///< 展开体 items JSON 数组 (可选, 空则走默认 args/result 展示; 支持 text/button/diagram/separator/diff)
} AgentxxToolRenderOutput;

typedef int32_t(AGENTXX_PLUGIN_CALL* AgentxxToolRenderFn)(
    void*                         user_data,
    const AgentxxToolRenderInput* input,
    AgentxxToolRenderOutput*      output
);

typedef struct AgentxxToolRenderSpec {
    int32_t                 version;       ///< 结构体版本 (必须 == 1)
    uint32_t                _reserved;     ///< 8 字节对齐
    AgentxxPluginStringView tool_name;     ///< 目标工具名 (必填, 如 "agentxx_filesystem_read")
    AgentxxToolRenderFn     render_fn;     ///< 渲染回调 (可为 NULL; 非空时优先调用)
    void*                   user_data;     ///< 回调上下文 (render_fn 非空时有效)
    AgentxxPluginStringView template_json; ///< 预设模版 JSON (render_fn 为 NULL 时由宿主解析执行)
} AgentxxToolRenderSpec;

/* ==================== 接口表: 展示/命令/toast (agentxx.client.ui) ==================== */

#define AGENTXX_IFACE_CLIENT_UI         "agentxx.client.ui"
#define AGENTXX_IFACE_CLIENT_UI_VERSION 2

typedef struct AgentxxClientUiIface {
    int32_t  version; ///< 必须 == AGENTXX_IFACE_CLIENT_UI_VERSION
    uint32_t _reserved;

    /* ---- 状态栏项 ---- */
    /// 注册状态栏项; 返回句柄 (宿主持有; 卸载自动清理)
    /// - id: 全局唯一, 建议 "{插件名}.{项名}" (如 "codegraph.index")
    /// - initialJson: {"text": "...", "tooltip": "..."} (text 必填)
    /// - align: 0=左侧 1=右侧; order: 组内排序 (小在前)
    /// - 宿主不支持该子能力 (函数指针 NULL) 或 id 冲突时返回 NULL
    AgentxxStatusItem*(AGENTXX_PLUGIN_CALL* register_status_item)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* id,
        const AgentxxPluginStringView* initialJson,
        int32_t                        align,
        int32_t                        order
    );
    /// 更新状态栏项文本 ({"text": "..."}); 句柄无效返回非 0
    int32_t(AGENTXX_PLUGIN_CALL* update_status_item)(
        const AgentxxPluginHost*       host,
        AgentxxStatusItem*             item,
        const AgentxxPluginStringView* json
    );
    /// 注销状态栏项 (句柄随后失效)
    void(AGENTXX_PLUGIN_CALL* unregister_status_item)(
        const AgentxxPluginHost* host,
        AgentxxStatusItem*       item
    );

    /* ---- 侧边栏面板 ---- */
    /// 注册侧边栏面板; 返回句柄 (宿主持有; 卸载自动清理)
    /// - id: 全局唯一, 建议 "{插件名}.{面板名}"
    /// - propsJson: {"title": "..."} (title 必填; 显示在 tab 栏)
    AgentxxPanel*(AGENTXX_PLUGIN_CALL* register_panel)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* id,
        const AgentxxPluginStringView* propsJson
    );
    /// 更新面板内容: itemsJson = {"items":[{"kind":"text","role":"normal","text":"..."},
    ///   {"kind":"progress","label":"...","value":0.5},
    ///   {"kind":"action","id":"rebuild","label":"Rebuild"}, ...]}
    /// - text.role 指定文本样式: "title"=高亮强调 / "normal"=普通文本(默认) /
    ///   "hint"=减淡提示 (缺省按 normal 渲染; 其余 role 值等同 normal)
    /// - action 项被用户点击时: 宿主 post 到 client io 线程回调面板注册时经
    ///   register_panel 关联的 on_action (见 entry 注册流程; 经回调参数注入)
    int32_t(AGENTXX_PLUGIN_CALL* update_panel)(
        const AgentxxPluginHost*       host,
        AgentxxPanel*                  panel,
        const AgentxxPluginStringView* itemsJson
    );
    void(AGENTXX_PLUGIN_CALL* unregister_panel)(const AgentxxPluginHost* host, AgentxxPanel* panel);

    /* ---- 侧边栏 Info 栏段落 ---- */
    /// 注册 Info 栏段落; 返回句柄 (宿主持有; 卸载自动清理)
    /// - id: 全局唯一, 建议 "{插件名}.{段名}"
    /// - propsJson: {"title": "..."} (title 可选; 空则无段落标题)
    AgentxxInfoSection*(AGENTXX_PLUGIN_CALL* register_info_section)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* id,
        const AgentxxPluginStringView* propsJson
    );
    /// 更新 Info 栏段落内容: itemsJson 同 update_panel 的 items schema
    ///   ({"items":[{"kind":"text","role":"title|normal|hint","text":"..."}, ...]});
    ///   列表项由宿主按侧边栏 Append 段样式以 "|  xxx" 前缀展示
    int32_t(AGENTXX_PLUGIN_CALL* update_info_section)(
        const AgentxxPluginHost*       host,
        AgentxxInfoSection*            section,
        const AgentxxPluginStringView* itemsJson
    );
    /// 注销 Info 栏段落 (句柄随后失效)
    void(AGENTXX_PLUGIN_CALL* unregister_info_section)(
        const AgentxxPluginHost* host,
        AgentxxInfoSection*      section
    );

    /* ---- 斜杠命令 ---- */
    /// 注册斜杠命令: 用户输入 "/{name}" 触发 (name 不含 '/' 与空格)
    /// - name: 全局唯一; description: 帮助/自动补全用
    /// - execute: client io 线程同步调用; 返回动作 JSON (host->alloc), 失败返回
    ///   空串并经 errorOut 输出错误 (host->alloc); 宿主解释动作 (见文件头)
    /// - 返回 0 成功; 名字冲突或参数非法返回非 0
    int32_t(AGENTXX_PLUGIN_CALL* register_command)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* name,
        const AgentxxPluginStringView* description,
        int32_t(AGENTXX_PLUGIN_CALL* execute)(
            void*                          ud,
            const AgentxxPluginStringView* argsJson,
            AgentxxPluginString*           actionOut,
            AgentxxPluginString*           errorOut
        ),
        void* ud
    );
    int32_t(AGENTXX_PLUGIN_CALL* unregister_command)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* name
    );

    /* ---- toast 提示 ---- */
    /// 显示 toast 提示 (level: 0=info 1=warning 2=error; 实现可忽略级别差异)
    void(AGENTXX_PLUGIN_CALL* show_toast)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* text,
        int32_t                        level
    );

    /* ---- 工具消息装饰 ---- */
    /// 更新/删除本插件对某次工具调用的消息装饰 (io 线程约束):
    /// - 装饰内容为宿主定义 schema 的语义 JSON (非组件), UI 按通用渲染器
    ///   展示: 折叠头 displayName/summary + 展开体 items
    ///   {"displayName": "Plan",              // 可选; 折叠头显示名 (缺省原始 toolName)
    ///    "summary": "[~] a; [ ] b",          // 可选; 折叠头一行摘要 (缺省回退参数预览)
    ///    "items": [                          // 可选; 展开体内容 (schema 同面板 items,
    ///                                        //   另有 diagram kind)
    ///      {"kind":"text","role":"title|normal|hint","text":"..."},
    ///      {"kind":"diagram","mermaid":"stateDiagram-v2..."} ]}
    /// - tool_call_id: 目标工具调用 id (取自 DELTA/tool_start 的 tool_call_id);
    ///   空视图 = 操作本插件的全部装饰
    /// - decor_json: 空串 = 删除 (tool_call_id 为空时删除本插件全部);
    ///   宿主在插件卸载/禁用时自动摘除其全部装饰, 启用时恢复
    /// - 典型数据源: 订阅 EVT_DELTA (tool_start 携带完整 arguments) 推送;
    ///   参考实现: agentxx_planning (Plan 渲染完全由插件驱动, TUI 无特化代码)
    /// 返回 0 成功; 非 0 失败 (宿主不支持/JSON 非法)
    int32_t(AGENTXX_PLUGIN_CALL* update_tool_decor)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* tool_call_id,
        const AgentxxPluginStringView* decor_json
    );

    /* ---- 工具特化渲染器 (按 tool_name 注册) ---- */
    /// 注册工具特化渲染器 (按 tool_name 键控):
    /// - 宿主在渲染该工具消息 (折叠头/展开体) 时调用, 无论实时流式还是历史消息
    /// - 支持两种模式:
    ///   1. <key, render_fn>: spec->render_fn != NULL, 宿主回调该函数输出 displayName/summary/items_json
    ///   2. 预设模版: spec->render_fn == NULL 且 template_json 非空,
    ///      宿主按模版自动提取参数字段并格式化 (如 {"displayName":"Search","summaryKey":"query"})
    /// - 展开体 items_json 支持新增的 {"kind":"diff","path":"...","old_str":"...","new_str":"..."}
    /// - 插件卸载/禁用时宿主自动注销
    /// 返回 0 成功, 非 0 失败
    int32_t(AGENTXX_PLUGIN_CALL* register_tool_renderer)(
        const AgentxxPluginHost*     host,
        const AgentxxToolRenderSpec* spec
    );
    int32_t(AGENTXX_PLUGIN_CALL* unregister_tool_renderer)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* tool_name
    );
} AgentxxClientUiIface;

/* ==================== 接口表: 事件订阅 (agentxx.client.events) ==================== */

#define AGENTXX_IFACE_CLIENT_EVENTS         "agentxx.client.events"
#define AGENTXX_IFACE_CLIENT_EVENTS_VERSION 1

typedef struct AgentxxClientEventsIface {
    int32_t  version; ///< 必须 == AGENTXX_IFACE_CLIENT_EVENTS_VERSION
    uint32_t _reserved;

    /// 订阅 client 事件 (payload JSON 字符串; 卸载自动退订); event 为
    /// AgentxxClientEvent 枚举值; 失败返回 NULL
    AgentxxPluginSubscription*(AGENTXX_PLUGIN_CALL* subscribe)(
        const AgentxxPluginHost* host,
        int32_t                  event, /* AgentxxClientEvent */
        void(AGENTXX_PLUGIN_CALL* handler)(const AgentxxPluginStringView* payloadJson, void* ud),
        void* ud
    );
    void(AGENTXX_PLUGIN_CALL* unsubscribe)(AgentxxPluginSubscription* sub);
} AgentxxClientEventsIface;

/* ==================== 接口表: 会话上下文与操作 (agentxx.client.session) ==================== */

#define AGENTXX_IFACE_CLIENT_SESSION         "agentxx.client.session"
#define AGENTXX_IFACE_CLIENT_SESSION_VERSION 1

typedef struct AgentxxClientSessionIface {
    int32_t  version; ///< 必须 == AGENTXX_IFACE_CLIENT_SESSION_VERSION
    uint32_t _reserved;

    /// 当前 client 状态 JSON 快照 (host->alloc):
    /// {"sessionId","connState","model","models":[],"isStreaming",
    ///  "interfaces":["agentxx.client.panel",...],
    ///  "agentPlugins":[{"name","version","interfaces":[...]},...]}
    /// (model/models/agentPlugins 依赖服务端推送; 未收到时为空)
    int32_t(AGENTXX_PLUGIN_CALL* get_client_state)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
    /// 代发一条用户消息 (sessionId 与当前会话不符时仍按当前会话发送并记日志)
    /// - 与用户输入同排队语义 (流式中进 pendingInputs), 不绕过 UI 状态机
    /// - 返回 0 成功; 非 0 表示宿主不可用 (未连接等)
    int32_t(AGENTXX_PLUGIN_CALL* send_user_input)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* sessionId,
        const AgentxxPluginStringView* text
    );
    /// 请求取消当前会话轮次 (与用户按 Esc 等价)
    void(AGENTXX_PLUGIN_CALL* request_cancel)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* sessionId
    );
} AgentxxClientSessionIface;

/* ==================== 接口表: 跨端数据通道 (agentxx.client.wire) ==================== */

#define AGENTXX_IFACE_CLIENT_WIRE         "agentxx.client.wire"
#define AGENTXX_IFACE_CLIENT_WIRE_VERSION 1

typedef struct AgentxxClientWireIface {
    int32_t  version; ///< 必须 == AGENTXX_IFACE_CLIENT_WIRE_VERSION
    uint32_t _reserved;

    /// 发送事件到 agent 侧: 服务端发布到事件总线 topic `client.{插件名}.{event}`
    /// (agent 侧同名插件可订阅; 载荷 JSON 原样透传, 语义由插件定义)
    /// - 返回 0 成功; 非 0 表示未连接或载荷非法
    int32_t(AGENTXX_PLUGIN_CALL* send_plugin_data)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* event,
        const AgentxxPluginStringView* json
    );
} AgentxxClientWireIface;

/* ==================== 接口表: 自描述/配置 (agentxx.client.self) ==================== */

#define AGENTXX_IFACE_CLIENT_SELF         "agentxx.client.self"
#define AGENTXX_IFACE_CLIENT_SELF_VERSION 1

typedef struct AgentxxClientSelfIface {
    int32_t  version; ///< 必须 == AGENTXX_IFACE_CLIENT_SELF_VERSION
    uint32_t _reserved;

    /// 本插件信息 JSON {"name","version","description","path"}
    /// (加载时常用: 从 path 推导资源目录; host->alloc)
    int32_t(AGENTXX_PLUGIN_CALL* get_own_info)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
    /// 本插件配置参数 JSON (yaml `plugins` 条目 args; io 线程; host->alloc):
    /// 宿主不解析 args 字段语义, 整体原样传递; 未配置时返回 "{}"
    int32_t(AGENTXX_PLUGIN_CALL* get_plugin_args)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
    /// 本插件配置文件所在目录或文件路径 (yaml `plugins` 条目 config; io 线程;
    /// host->alloc; 未指定返回空串)
    /// - 可指向文件或目录 (由插件自行判断类型并加载)
    /// - 宿主已归一化为绝对路径 (正斜杠, lexically_normal)
    int32_t(AGENTXX_PLUGIN_CALL* get_plugin_config_path)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
} AgentxxClientSelfIface;

/* ==================== 接口表: JSON 辅助 (agentxx.client.json) ==================== */

#define AGENTXX_IFACE_CLIENT_JSON         "agentxx.client.json"
#define AGENTXX_IFACE_CLIENT_JSON_VERSION 1

typedef struct AgentxxClientJsonIface {
    int32_t  version; ///< 必须 == AGENTXX_IFACE_CLIENT_JSON_VERSION
    uint32_t _reserved;

    /// 从 JSON 字符串提取指定 key 的字符串值 (宿主解析; 结果 host->alloc)
    int32_t(AGENTXX_PLUGIN_CALL* json_get_string)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* json,
        const AgentxxPluginStringView* key,
        AgentxxPluginString*           out
    );
    /// 字符串 → JSON 字符串字面量 (含引号包裹与转义; 结果 host->alloc)
    int32_t(AGENTXX_PLUGIN_CALL* json_escape)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* s,
        AgentxxPluginString*           out
    );
} AgentxxClientJsonIface;

/* ==================== 接口表: 日志 (agentxx.client.log) ==================== */

#define AGENTXX_IFACE_CLIENT_LOG         "agentxx.client.log"
#define AGENTXX_IFACE_CLIENT_LOG_VERSION 1

typedef struct AgentxxClientLogIface {
    int32_t  version; ///< 必须 == AGENTXX_IFACE_CLIENT_LOG_VERSION
    uint32_t _reserved;

    /// 日志 (线程安全; 0=trace 1=debug 2=info 3=warn 4=error)
    void(AGENTXX_PLUGIN_CALL*
             log)(const AgentxxPluginHost* host, int32_t level, const AgentxxPluginStringView* msg);
} AgentxxClientLogIface;

/* ==================== 插件入口符号 (dlsym) ==================== */

/// 可选: 查询插件元信息 (加载前调用, 用于版本/信息校验; 未导出则跳过;
/// 纯静态元数据, 不得读取/依赖任何实例状态)
typedef const AgentxxClientPluginInfo*(AGENTXX_PLUGIN_CALL* AgentxxClientPluginGetInfoFn)(void);
/// 必需: client 侧插件实例创建 (宿主线程池调用; 内部注册动作宿主自动投递回
/// client io 线程; 语义同 agent 侧 agentxx_plugin_agent_create)
/// 【多实例契约】可重入, 每次调用产出独立存活实例 —— 一切实例状态只能
/// 存于 *plugin_ctx 指向的堆块 (禁止可变全局/函数级 static 缓存); 一切注册
/// 回调必须设置 user_data = 实例上下文。
/// - host: 本实例专属宿主句柄 (opaque 已关联本实例)
/// - plugin_ctx: 输出本实例私有上下文 (透传给 destroy)
/// - 返回 0 成功; 非 0 创建失败 (宿主走失败清理路径并报告错误)
typedef int32_t(AGENTXX_PLUGIN_CALL* AgentxxClientPluginCreateFn)(
    const AgentxxPluginHost* host,
    void**                   plugin_ctx
);
/// 可选: 插件实例销毁 (宿主等全部在途回调完成后调用; 宿主会在此之前自动
/// 反注册该实例的一切 status item/panel/command/订阅)。只销毁对应 create
/// 产出的实例上下文, 与其他并存实例无关。
typedef void(AGENTXX_PLUGIN_CALL* AgentxxClientPluginDestroyFn)(void* plugin_ctx);

#define AGENTXX_PLUGIN_CLIENT_SYMBOL_GET_INFO "agentxx_plugin_client_get_info"
#define AGENTXX_PLUGIN_CLIENT_SYMBOL_CREATE   "agentxx_plugin_client_create"
#define AGENTXX_PLUGIN_CLIENT_SYMBOL_DESTROY  "agentxx_plugin_client_destroy"

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* AGENTXX_CLIENT_PLUGIN_API_H */
