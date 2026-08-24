/*
 * agentxx/plugin/client_plugin_api.h —— client 侧插件系统纯 C ABI 契约
 *
 * 背景: agent 侧插件 (plugin_api.h) 只扩展 agent 线程能力 (工具/钩子/事件/能力);
 * client (CLI/TUI/未来 GUI) 是另一类宿主, 有自己的生命周期、线程模型与 UI 形态。
 * 本头定义 client 侧插件的跨版本稳定接口, 与 agent 侧入口完全独立:
 *   - 同一动态库可同时导出 agent 入口 (agentxx_plugin_entry) 与 client 入口
 *     (agentxx_client_entry), 两个 PluginManager 各自 dlopen/装配, 实例状态独立
 *   - 跨端通信统一走 wire 协议 (WirePluginData agent→client / WirePluginDataUp
 *     client→agent), 插件不感知本地 Channel / 远程 WS 部署形态
 *
 * 设计要点 (与 plugin_api.h 一致):
 * - 纯 C 头: 插件可用任意编译器/任意语言实现, 与宿主 STL/异常/RTTI ABI 解耦
 * - 跨 CRT 堆边界: 宿主分配的内存经 AgentxxClientHostVtable alloc/free/strdup;
 *   字符串参数一律 AgentxxPluginStringView (data+size 只读借用, 仅本次调用有效)
 * - 每插件一个 AgentxxClientHost (opaque 指向宿主侧插件实例); 卸载时宿主自动
 *   清理其全部注册残留 (status item/panel/command/订阅)
 * - 线程约定:
 *   - entry 运行在宿主线程池, 注册动作经 vtable 内部 post 回 client io 线程执行
 *   - 事件 handler / 命令 execute / panel action 回调均在 client io 线程同步调用,
 *     必须快速返回 (长时间任务请自行投递线程, 结果经 post 回 io 线程再更新 UI)
 *   - UI 线程 (TUI 渲染/事件) 从不直接调用插件代码; 交互经宿主投递回 io 线程
 * - 回调异常不外泄: 宿主 vtable 全部函数内部捕获异常; 插件侧回调同样不得让
 *   异常逃逸 (宿主调用处已兜底)
 * - 命令 execute 返回值: 动作 JSON 字符串 (host->alloc):
 *     {"action":"none"}                      已处理完毕
 *     {"action":"send","text":"..."}         代为发送一条用户消息 (经 UI 排队语义)
 *     {"action":"toast","text":"...","level":0|1|2}
 *   宿主解释执行动作; 非法/未知 action 仅记日志, 不影响会话
 *
 * 版本策略: 修改本契约时递增 AGENTXX_CLIENT_PLUGIN_API_VERSION; 宿主拒绝
 * api_version 不匹配的插件 (仅拒绝, 不崩溃)
 */
#ifndef AGENTXX_CLIENT_PLUGIN_API_H
#define AGENTXX_CLIENT_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "agentxx/plugin/plugin_api.h" /* AgentxxPluginStringView / AgentxxSubscription */

#ifdef __cplusplus
extern "C" {
#endif

#define AGENTXX_CLIENT_PLUGIN_API_VERSION 4

/* ==================== 接口协商 (字符串集; 取代 v3 及以前的位图方案) ====================
 *
 * 宿主与插件间的能力协商统一为【稳定命名字符串接口集】(常量见
 * plugin_common.h plugin_interfaces; 第三方私有接口用 "<vendor>.<name>",
 * 宿主不认识的名称一律视为不支持 —— 安全失败):
 * - 清单声明层: 插件 plugin.yaml `interfaces.require/optional`, 宿主加载前
 *   门禁 (机制总览见 plugin_common.h 接口协商节);
 * - 运行时查询层: has_interface() 同步判单名; EVT_READY 与
 *   get_client_state 的 "interfaces" JSON 数组做全量发现;
 * - 展示类子能力 (status item/panel/info section/command/toast) 物理迁移到
 *   "client.ui" 扩展表 (AgentxxClientExtUiVtable, 经 query_extension 获取):
 *   表内不支持的子能力函数指针为 NULL, 调用前必须判空; has_interface 对
 *   应成员同步返回 0。
 * v3 的 AGENTXX_UI_CAP_* / AGENTXX_IFACE_* 位图与本结构体 min_ui_caps 字段
 * 已移除 (v4 不兼容变更, 迁移说明见 docs/agent/plugins.md 版本历史)。
 */

/* ==================== 插件元信息 ==================== */

typedef struct AgentxxClientPluginInfo {
    int                     api_version; ///< 必须 == AGENTXX_CLIENT_PLUGIN_API_VERSION
    AgentxxPluginStringView name;        ///< 唯一标识 (与 agent 侧插件共用命名空间)
    AgentxxPluginStringView version;
    AgentxxPluginStringView description;
} AgentxxClientPluginInfo;

/* ==================== client 事件订阅 ==================== */

/// 事件类型 (payload 均为 JSON 字符串, 宿主构造; 语义见注释)
typedef enum AgentxxClientEvent {
    AGENTXX_CLIENT_EVT_READY = 0,      ///< 服务端就绪 {"interfaces":[...],"sessionId"} (启动后最早事件)
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

/* ==================== 宿主函数表 ==================== */

typedef struct AgentxxClientHost AgentxxClientHost;

typedef struct AgentxxClientHostVtable {
    /* ---- 内存 (跨 CRT 堆边界的唯一分配通道) ---- */
    void* (*alloc)(size_t size);
    void (*free)(void* ptr);
    char* (*strdup)(const char* s);

    /* ---- 接口协商 (字符串集; 见本头 "接口协商" 节) ---- */
    /// 宿主是否支持指定接口 ("client.panel"/"client.toast"/"<vendor>.x" 等;
    /// 常量见 plugin_common.h plugin_interfaces; 未知名称返回 0 —— 安全失败)
    int (*has_interface)(const AgentxxClientHost* host, AgentxxPluginStringView name);
    /// COM 风格扩展接口表查询 (未实现/未知名称返回 NULL):
    /// - 已定义扩展表: AGENTXX_CLIENT_EXT_UI ("client.ui", 展示/命令/toast)
    /// - 扩展表首字段恒为 int version (该扩展接口自身版本, 独立演进, 与全局
    ///   api_version 解耦); 表内函数指针可能为 NULL (宿主未实现该子能力),
    ///   调用前必须判空 —— 等价于 has_interface 对应名为假
    /// - 【契约冻结】核心 vtable 自 v4 起不再增删成员: 未来新增能力一律定义
    ///   新的扩展表经本函数分发 (三期 COM 化演进, 见 docs/agent/plugins.md)
    const void* (*query_extension)(const AgentxxClientHost* host, AgentxxPluginStringView name);

    /* ---- 事件订阅 (payload JSON 字符串; 卸载自动退订) ---- */
    AgentxxSubscription* (*subscribe)(
        const AgentxxClientHost* host,
        int                      event, /* AgentxxClientEvent */
        void (*handler)(AgentxxPluginStringView payloadJson, void* ud),
        void* ud
    );
    void (*unsubscribe)(AgentxxSubscription* sub);

    /* ---- 会话上下文 (快照; host->alloc) ---- */
    /// 当前 client 状态 JSON:
    /// {"sessionId","connState","model","models":[],"isStreaming",
    ///  "interfaces":["client.panel",...],
    ///  "agentPlugins":[{"name","version","interfaces":[...]},...]}
    /// (model/models/agentPlugins 依赖服务端推送; 未收到时为空)
    char* (*get_client_state)(const AgentxxClientHost* host);

    /* ---- 会话操作 (受限; 见插件设计文档安全节) ---- */
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

    /* ---- 跨端插件数据通道: client 实例 → wire → agent 侧实例 ---- */
    /// 发送事件到 agent 侧: 服务端发布到事件总线 topic `client.{插件名}.{event}`
    /// (agent 侧同名插件可订阅; 载荷 JSON 原样透传, 语义由插件定义)
    /// - 返回 0 成功; 非 0 表示未连接或载荷非法
    int (*send_plugin_data)(
        const AgentxxClientHost* host,
        AgentxxPluginStringView  event,
        AgentxxPluginStringView  json
    );

    /* ---- 自描述/配置 ---- */
    /// 本插件信息 JSON {"name","version","description","path"}
    /// (加载时常用: 从 path 推导资源目录; host->alloc)
    char* (*get_own_info)(const AgentxxClientHost* host);
    /// 本插件配置参数 JSON (yaml `plugins` 条目 args; io 线程; host->alloc):
    /// 宿主不解析 args 字段语义, 整体原样传递; 未配置时返回 "{}"
    char* (*get_plugin_args)(const AgentxxClientHost* host);

    /* ---- 日志 (线程安全; 0=trace 1=debug 2=info 3=warn 4=error) ---- */
    void (*log)(const AgentxxClientHost* host, int level, AgentxxPluginStringView msg);

    /* ---- JSON 辅助 (线程安全) ---- */
    /// 从 JSON 字符串提取指定 key 的字符串值 (宿主解析; 结果 host->alloc)
    char* (*json_get_string)(
        const AgentxxClientHost* host,
        AgentxxPluginStringView  json,
        AgentxxPluginStringView  key
    );
    /// 字符串 → JSON 字符串字面量 (含引号包裹与转义; 结果 host->alloc)
    char* (*json_escape)(const AgentxxClientHost* host, AgentxxPluginStringView s);
} AgentxxClientHostVtable;

/* ==================== 扩展接口表 (COM 风格; 经 query_extension 获取) ==================== */

/// "client.ui" 展示扩展表 (自核心 vtable 物理迁移; v4 起展示/命令/toast 全部
/// 经此表访问): 表内函数指针可能为 NULL (宿主未实现该子能力), 调用前必须判空;
/// 各函数签名与迁移前完全一致, 仅获取途径变化。
/// 典型用法:
///   auto ui = (const AgentxxClientExtUiVtable*)
///       host->vtable->query_extension(host, AGENTXX_CLIENT_EXT_UI);
///   if (ui && ui->register_panel) { ... }
#define AGENTXX_CLIENT_EXT_UI         "client.ui"
#define AGENTXX_CLIENT_EXT_UI_VERSION 1

typedef struct AgentxxClientExtUiVtable {
    int version; ///< 必须 == AGENTXX_CLIENT_EXT_UI_VERSION

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
    ///   register_panel 关联的 on_action (见 entry 注册流程; 一期经回调参数注入)
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
} AgentxxClientExtUiVtable;

struct AgentxxClientHost {
    const AgentxxClientHostVtable* vtable; ///< 函数表 (宿主静态)
    void* opaque; ///< 宿主内部 (指向插件实例状态, 插件不得使用)
};

/* ==================== 插件入口符号 (dlsym) ==================== */

/// 可选: 查询插件元信息 (加载前调用, 用于版本/信息校验; 未导出则跳过)
typedef const AgentxxClientPluginInfo* (*AgentxxClientPluginGetInfoFn)(void);

/// 必需: client 侧插件入口 (宿主线程池调用; 内部注册动作宿主自动投递回
/// client io 线程; 语义同 agent 侧 agentxx_plugin_entry)
/// - host: 本插件专属宿主句柄 (opaque 已关联本插件)
/// - plugin_ctx: 输出插件私有上下文 (透传给 unload)
/// - 返回 0 成功; 非 0 加载失败 (宿主 dlclose 并报告错误)
typedef int (*AgentxxClientPluginEntryFn)(const AgentxxClientHost* host, void** plugin_ctx);

/// 可选: 插件卸载通知 (宿主等全部在途回调完成后调用; 宿主会在此之前自动
/// 反注册该插件的一切 status item/panel/command/订阅)
typedef void (*AgentxxClientPluginUnloadFn)(void* plugin_ctx);

#define AGENTXX_CLIENT_SYMBOL_GET_INFO "agentxx_client_get_info"
#define AGENTXX_CLIENT_SYMBOL_ENTRY    "agentxx_client_entry"
#define AGENTXX_CLIENT_SYMBOL_UNLOAD   "agentxx_client_unload"

#ifdef __cplusplus
}
#endif

#endif /* AGENTXX_CLIENT_PLUGIN_API_H */
