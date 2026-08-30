/*
 * agentxx/plugin/client_plugin_api.h —— client 侧插件系统纯 C ABI 契约
 *
 * 背景: agent 侧插件 (plugin_api.h) 只扩展 agent 线程能力 (工具/钩子/事件/能力);
 * client (CLI/TUI/未来 GUI) 是另一类宿主, 有自己的生命周期、线程模型与 UI 形态。
 * 本头定义 client 侧插件的跨版本稳定接口, 与 agent 侧入口完全独立:
 *   - 同一动态库可同时导出 agent 入口 (agentxx_plugin_agent_create) 与 client 入口
 *     (agentxx_plugin_client_create), 两个 PluginManager 各自 dlopen/装配, 实例状态独立
 *   - 跨端通信统一走 wire 协议 (WirePluginData agent→client / WirePluginDataUp
 *     client→agent), 插件不感知本地 Channel / 远程 WS 部署形态
 *
 * ════════════════════════════════════════════════════════════════════
 * 架构: COM 风格接口表查询
 * ════════════════════════════════════════════════════════════════════
 * - 核心 vtable 极简且【契约冻结】: alloc/free/strdup + query_interface;
 *   一切宿主能力按稳定 IID 字符串查询独立接口表获取
 * - 每个接口表首字段恒为 int version (独立演进, 与全局 api_version 解耦);
 *   表内函数指针可能为 NULL (宿主未实现该子能力), 调用前必须判空;
 *   查询未知名称返回 NULL (安全失败)
 * - 版本策略: 全局 AGENTXX_CLIENT_PLUGIN_API_VERSION 只覆盖核心契约;
 *   宿主精确匹配门禁, 无历史兼容路径; 接口表各自带 version 独立演进
 *
 * 设计要点 (与 plugin_api.h 一致):
 * - 纯 C 头: 插件可用任意编译器/任意语言实现, 与宿主 STL/异常/RTTI ABI 解耦
 * - 跨 CRT 堆边界: 宿主分配的内存经核心 vtable alloc/free/strdup;
 *   字符串参数一律 AgentxxPluginStringView (data+size 只读借用, 仅本次调用有效)
 * - 每插件一个 AgentxxClientHost (opaque 指向宿主侧插件实例); 卸载时宿主自动
 *   清理其全部注册残留 (status item/panel/command/订阅)
 * - 线程约定:
 *   - entry 运行在宿主线程池, 注册动作由接口表实现内部 post 回 client io 线程执行
 *   - 事件 handler / 命令 execute / panel action 回调均在 client io 线程同步调用,
 *     必须快速返回 (长时间任务请自行投递线程, 结果经 post 回 io 线程再更新 UI)
 *   - UI 线程 (TUI 渲染/事件) 从不直接调用插件代码; 交互经宿主投递回 io 线程
 * - 回调异常不外泄: 宿主接口表全部函数内部捕获异常; 插件侧回调同样不得让
 *   异常逃逸 (宿主调用处已兜底)
 * - 命令 execute 返回值: 动作 JSON 字符串 (host->alloc):
 *     {"action":"none"}                      已处理完毕
 *     {"action":"send","text":"..."}         代为发送一条用户消息 (经 UI 排队语义)
 *     {"action":"toast","text":"...","level":0|1|2}
 *   宿主解释执行动作; 非法/未知 action 仅记日志, 不影响会话
 */
#ifndef AGENTXX_CLIENT_PLUGIN_API_H
#define AGENTXX_CLIENT_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "agentxx/plugin/plugin_api.h" /* AgentxxPluginStringView / AgentxxPluginSubscription /
                                          AGENTXX_PLUGIN_QUERY_IFACE / 核心契约共享类型 */

#ifdef __cplusplus
extern "C" {
#endif

/// 全局 API 版本 (client 侧): 只覆盖核心契约 (核心 vtable 形状 + Info 结构 +
/// 入口符号)。接口表各自带 version 独立演进, 不影响本版本号。
/// 宿主精确匹配门禁: info.api_version != 本值 → 拒绝加载
#define AGENTXX_CLIENT_PLUGIN_API_VERSION 1

/* ==================== 插件元信息 ==================== */

typedef struct AgentxxClientPluginInfo {
    int                     api_version; ///< 必须 == AGENTXX_CLIENT_PLUGIN_API_VERSION
    AgentxxPluginStringView name;        ///< 唯一标识 (与 agent 侧插件共用命名空间)
    AgentxxPluginStringView version;
    AgentxxPluginStringView description;
} AgentxxClientPluginInfo;

/* ==================== client 事件类型枚举 (载荷 JSON 字符串) ==================== */

/// 事件类型 (payload 均为 JSON 字符串, 宿主构造; 语义见注释)
typedef enum AgentxxClientEvent {
    AGENTXX_CLIENT_EVT_READY = 0, ///< 服务端就绪 {"interfaces":[...],"sessionId"} (启动后最早事件)
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

/* ==================== 核心宿主函数表 (契约冻结) ==================== */

typedef struct AgentxxClientHost AgentxxClientHost;

/// 核心 vtable: 仅内存三件套 + COM 风格接口表查询。
/// 【契约冻结】本结构自 v1 起不再增删成员: 一切宿主能力经 query_interface
/// 按稳定 IID 查询独立接口表获取。
typedef struct AgentxxClientHostVtable {
    /* ---- 内存 (跨 CRT 堆边界的唯一分配通道; 任意线程可调用) ---- */
    void* (*alloc)(size_t size);
    void (*free)(void* ptr);
    char* (*strdup)(const char* s);

    /* ---- COM 风格接口表查询 (QueryInterface; 任意线程可调用) ---- */
    /// 按稳定 IID 字符串查询接口表; 未实现/未知名称返回 NULL (安全失败)
    /// - 已知 IID 见下方各 AGENTXX_IFACE_CLIENT_* 宏与对应 *Iface 结构体
    /// - 接口表为进程级静态数据: 返回指针长期有效, 可在 entry 时查询缓存;
    ///   表内函数指针可能为 NULL (宿主未实现该子能力), 调用前必须判空 ——
    ///   等价于该子能力不存在 (如 CLI 宿主的 register_panel 为 NULL)
    const void* (*query_interface)(const AgentxxClientHost* host, AgentxxPluginStringView iid);
} AgentxxClientHostVtable;

struct AgentxxClientHost {
    const AgentxxClientHostVtable* vtable; ///< 核心函数表 (宿主静态)
    void* opaque; ///< 宿主内部 (指向插件实例状态, 插件不得使用)
};

/* ==================== 接口表: 展示/命令/toast (agentxx.client.ui) ==================== */

#define AGENTXX_IFACE_CLIENT_UI         "agentxx.client.ui"
#define AGENTXX_IFACE_CLIENT_UI_VERSION 1

typedef struct AgentxxClientUiIface {
    int version; ///< 必须 >= AGENTXX_IFACE_CLIENT_UI_VERSION (v2 追加 update_tool_decor)

    /* ---- 状态栏项 ---- */
    /// 注册状态栏项; 返回句柄 (宿主持有; 卸载自动清理)
    /// - id: 全局唯一, 建议 "{插件名}.{项名}" (如 "codegraph.index")
    /// - initialJson: {"text": "...", "tooltip": "..."} (text 必填)
    /// - align: 0=左侧 1=右侧; order: 组内排序 (小在前)
    /// - 宿主不支持该子能力 (函数指针 NULL) 或 id 冲突时返回 NULL
    AgentxxStatusItem* (*register_status_item)(
        const AgentxxClientHost* host,
        AgentxxPluginStringView  id,
        AgentxxPluginStringView  initialJson,
        int                      align,
        int                      order
    );
    /// 更新状态栏项文本 ({"text": "..."}); 句柄无效返回非 0
    int (*update_status_item)(
        const AgentxxClientHost* host,
        AgentxxStatusItem*       item,
        AgentxxPluginStringView  json
    );
    /// 注销状态栏项 (句柄随后失效)
    void (*unregister_status_item)(const AgentxxClientHost* host, AgentxxStatusItem* item);

    /* ---- 侧边栏面板 ---- */
    /// 注册侧边栏面板; 返回句柄 (宿主持有; 卸载自动清理)
    /// - id: 全局唯一, 建议 "{插件名}.{面板名}"
    /// - propsJson: {"title": "..."} (title 必填; 显示在 tab 栏)
    AgentxxPanel* (*register_panel)(
        const AgentxxClientHost* host,
        AgentxxPluginStringView  id,
        AgentxxPluginStringView  propsJson
    );
    /// 更新面板内容: itemsJson = {"items":[{"kind":"text","role":"normal","text":"..."},
    ///   {"kind":"progress","label":"...","value":0.5},
    ///   {"kind":"action","id":"rebuild","label":"Rebuild"}, ...]}
    /// - text.role 指定文本样式: "title"=高亮强调 / "normal"=普通文本(默认) /
    ///   "hint"=减淡提示 (缺省按 normal 渲染; 其余 role 值等同 normal)
    /// - action 项被用户点击时: 宿主 post 到 client io 线程回调面板注册时经
    ///   register_panel 关联的 on_action (见 entry 注册流程; 经回调参数注入)
    int (*update_panel)(
        const AgentxxClientHost* host,
        AgentxxPanel*            panel,
        AgentxxPluginStringView  itemsJson
    );
    /// 注销面板 (句柄随后失效)
    void (*unregister_panel)(const AgentxxClientHost* host, AgentxxPanel* panel);

    /* ---- 侧边栏 Info 栏段落 ---- */
    /// 注册 Info 栏段落; 返回句柄 (宿主持有; 卸载自动清理)
    /// - id: 全局唯一, 建议 "{插件名}.{段名}"
    /// - propsJson: {"title": "..."} (title 可选; 空则无段落标题)
    AgentxxInfoSection* (*register_info_section)(
        const AgentxxClientHost* host,
        AgentxxPluginStringView  id,
        AgentxxPluginStringView  propsJson
    );
    /// 更新 Info 栏段落内容: itemsJson 同 update_panel 的 items schema
    ///   ({"items":[{"kind":"text","role":"title|normal|hint","text":"..."}, ...]});
    ///   列表项由宿主按侧边栏 Append 段样式以 "|  xxx" 前缀展示
    int (*update_info_section)(
        const AgentxxClientHost* host,
        AgentxxInfoSection*      section,
        AgentxxPluginStringView  itemsJson
    );
    /// 注销 Info 栏段落 (句柄随后失效)
    void (*unregister_info_section)(const AgentxxClientHost* host, AgentxxInfoSection* section);

    /* ---- 斜杠命令 ---- */
    /// 注册斜杠命令: 用户输入 "/{name}" 触发 (name 不含 '/' 与空格)
    /// - name: 全局唯一; description: 帮助/自动补全用
    /// - execute: client io 线程同步调用; 返回动作 JSON (host->alloc), 失败返回
    ///   NULL 并经 errorOut 输出错误 (host->alloc); 宿主解释动作 (见文件头)
    /// - 返回 0 成功; 名字冲突或参数非法返回非 0
    int (*register_command)(
        const AgentxxClientHost* host,
        AgentxxPluginStringView  name,
        AgentxxPluginStringView  description,
        char* (*execute)(void* ud, AgentxxPluginStringView argsJson, char** errorOut),
        void* ud
    );
    /// 注销斜杠命令 (按名称); 不存在返回非 0
    int (*unregister_command)(const AgentxxClientHost* host, AgentxxPluginStringView name);

    /* ---- toast 提示 ---- */
    /// 显示 toast 提示 (level: 0=info 1=warning 2=error; 实现可忽略级别差异)
    void (*show_toast)(const AgentxxClientHost* host, AgentxxPluginStringView text, int level);

    /* ---- v2 追加: 工具消息装饰 (UI 无关语义层) ---- */
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
    int (*update_tool_decor)(
        const AgentxxClientHost* host,
        AgentxxPluginStringView  tool_call_id,
        AgentxxPluginStringView  decor_json
    );
} AgentxxClientUiIface;

/* ==================== 接口表: 事件订阅 (agentxx.client.events) ==================== */

#define AGENTXX_IFACE_CLIENT_EVENTS         "agentxx.client.events"
#define AGENTXX_IFACE_CLIENT_EVENTS_VERSION 1

typedef struct AgentxxClientEventsIface {
    int version; ///< 必须 == AGENTXX_IFACE_CLIENT_EVENTS_VERSION

    /// 订阅 client 事件 (payload JSON 字符串; 卸载自动退订); event 为
    /// AgentxxClientEvent 枚举值; 失败返回 NULL
    AgentxxPluginSubscription* (*subscribe)(
        const AgentxxClientHost* host,
        int                      event, /* AgentxxClientEvent */
        void (*handler)(AgentxxPluginStringView payloadJson, void* ud),
        void* ud
    );
    void (*unsubscribe)(AgentxxPluginSubscription* sub);
} AgentxxClientEventsIface;

/* ==================== 接口表: 会话上下文与操作 (agentxx.client.session) ==================== */

#define AGENTXX_IFACE_CLIENT_SESSION         "agentxx.client.session"
#define AGENTXX_IFACE_CLIENT_SESSION_VERSION 1

typedef struct AgentxxClientSessionIface {
    int version; ///< 必须 == AGENTXX_IFACE_CLIENT_SESSION_VERSION

    /// 当前 client 状态 JSON 快照 (host->alloc):
    /// {"sessionId","connState","model","models":[],"isStreaming",
    ///  "interfaces":["agentxx.client.panel",...],
    ///  "agentPlugins":[{"name","version","interfaces":[...]},...]}
    /// (model/models/agentPlugins 依赖服务端推送; 未收到时为空)
    char* (*get_client_state)(const AgentxxClientHost* host);
    /// 代发一条用户消息 (sessionId 与当前会话不符时仍按当前会话发送并记日志)
    /// - 与用户输入同排队语义 (流式中进 pendingInputs), 不绕过 UI 状态机
    /// - 返回 0 成功; 非 0 表示宿主不可用 (未连接等)
    int (*send_user_input)(
        const AgentxxClientHost* host,
        AgentxxPluginStringView  sessionId,
        AgentxxPluginStringView  text
    );
    /// 请求取消当前会话轮次 (与用户按 Esc 等价)
    void (*request_cancel)(const AgentxxClientHost* host, AgentxxPluginStringView sessionId);
} AgentxxClientSessionIface;

/* ==================== 接口表: 跨端数据通道 (agentxx.client.wire) ==================== */

#define AGENTXX_IFACE_CLIENT_WIRE         "agentxx.client.wire"
#define AGENTXX_IFACE_CLIENT_WIRE_VERSION 1

typedef struct AgentxxClientWireIface {
    int version; ///< 必须 == AGENTXX_IFACE_CLIENT_WIRE_VERSION

    /// 发送事件到 agent 侧: 服务端发布到事件总线 topic `client.{插件名}.{event}`
    /// (agent 侧同名插件可订阅; 载荷 JSON 原样透传, 语义由插件定义)
    /// - 返回 0 成功; 非 0 表示未连接或载荷非法
    int (*send_plugin_data)(
        const AgentxxClientHost* host,
        AgentxxPluginStringView  event,
        AgentxxPluginStringView  json
    );
} AgentxxClientWireIface;

/* ==================== 接口表: 自描述/配置 (agentxx.client.self) ==================== */

#define AGENTXX_IFACE_CLIENT_SELF         "agentxx.client.self"
#define AGENTXX_IFACE_CLIENT_SELF_VERSION 1

typedef struct AgentxxClientSelfIface {
    int version; ///< 必须 == AGENTXX_IFACE_CLIENT_SELF_VERSION

    /// 本插件信息 JSON {"name","version","description","path"}
    /// (加载时常用: 从 path 推导资源目录; host->alloc)
    char* (*get_own_info)(const AgentxxClientHost* host);
    /// 本插件配置参数 JSON (yaml `plugins` 条目 args; io 线程; host->alloc):
    /// 宿主不解析 args 字段语义, 整体原样传递; 未配置时返回 "{}"
    char* (*get_plugin_args)(const AgentxxClientHost* host);
} AgentxxClientSelfIface;

/* ==================== 接口表: JSON 辅助 (agentxx.client.json) ==================== */

#define AGENTXX_IFACE_CLIENT_JSON         "agentxx.client.json"
#define AGENTXX_IFACE_CLIENT_JSON_VERSION 1

typedef struct AgentxxClientJsonIface {
    int version; ///< 必须 == AGENTXX_IFACE_CLIENT_JSON_VERSION

    /// 从 JSON 字符串提取指定 key 的字符串值 (宿主解析; 结果 host->alloc)
    char* (*json_get_string)(
        const AgentxxClientHost* host,
        AgentxxPluginStringView  json,
        AgentxxPluginStringView  key
    );
    /// 字符串 → JSON 字符串字面量 (含引号包裹与转义; 结果 host->alloc)
    char* (*json_escape)(const AgentxxClientHost* host, AgentxxPluginStringView s);
} AgentxxClientJsonIface;

/* ==================== 接口表: 日志 (agentxx.client.log) ==================== */

#define AGENTXX_IFACE_CLIENT_LOG         "agentxx.client.log"
#define AGENTXX_IFACE_CLIENT_LOG_VERSION 1

typedef struct AgentxxClientLogIface {
    int version; ///< 必须 == AGENTXX_IFACE_CLIENT_LOG_VERSION

    /// 日志 (线程安全; 0=trace 1=debug 2=info 3=warn 4=error)
    void (*log)(const AgentxxClientHost* host, int level, AgentxxPluginStringView msg);
} AgentxxClientLogIface;

/* ==================== 插件入口符号 (dlsym) ==================== */

/// 可选: 查询插件元信息 (加载前调用, 用于版本/信息校验; 未导出则跳过;
/// 纯静态元数据, 不得读取/依赖任何实例状态)
typedef const AgentxxClientPluginInfo* (*AgentxxClientPluginGetInfoFn)(void);

/// 必需: client 侧插件实例创建 (宿主线程池调用; 内部注册动作宿主自动投递回
/// client io 线程; 语义同 agent 侧 agentxx_plugin_agent_create)
/// 【多实例契约】可重入, 每次调用产出独立存活实例 —— 一切实例状态只能
/// 存于 *plugin_ctx 指向的堆块 (禁止可变全局/函数级 static 缓存); 一切注册
/// 回调必须设置 user_data = 实例上下文。
/// - host: 本实例专属宿主句柄 (opaque 已关联本实例)
/// - plugin_ctx: 输出本实例私有上下文 (透传给 destroy)
/// - 返回 0 成功; 非 0 创建失败 (宿主走失败清理路径并报告错误)
typedef int (*AgentxxClientPluginCreateFn)(const AgentxxClientHost* host, void** plugin_ctx);

/// 可选: 插件实例销毁 (宿主等全部在途回调完成后调用; 宿主会在此之前自动
/// 反注册该实例的一切 status item/panel/command/订阅)。只销毁对应 create
/// 产出的实例上下文, 与其他并存实例无关。
typedef void (*AgentxxClientPluginDestroyFn)(void* plugin_ctx);

#define AGENTXX_PLUGIN_CLIENT_SYMBOL_GET_INFO "agentxx_plugin_client_get_info"
#define AGENTXX_PLUGIN_CLIENT_SYMBOL_CREATE   "agentxx_plugin_client_create"
#define AGENTXX_PLUGIN_CLIENT_SYMBOL_DESTROY  "agentxx_plugin_client_destroy"

#ifdef __cplusplus
}
#endif

#endif /* AGENTXX_CLIENT_PLUGIN_API_H */
