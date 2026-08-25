/*
 * agentxx/plugin/plugin_api.h —— 插件系统纯 C ABI 契约 (agent 侧; 唯一跨版本稳定接口)
 *
 * ════════════════════════════════════════════════════════════════════
 * 架构: COM 风格接口表查询 (v1 全量重构, 不兼容旧版)
 * ════════════════════════════════════════════════════════════════════
 * - 核心 vtable 极简且【契约冻结】: 仅 内存三件套 (alloc/free/strdup) +
 *   query_interface —— 一切宿主能力都按稳定 IID 字符串查询独立接口表获取
 *   (COM QueryInterface 风格), 核心永不再增删成员
 * - 每个接口表为纯 C 结构体, 首字段恒为 int version (该接口自身版本,
 *   独立演进, 与全局 api_version 解耦); 表内函数指针可能为 NULL
 *   (宿主未实现该子能力), 调用前必须判空; 查询未知名称返回 NULL (安全失败)
 * - 版本策略 (双层):
 *   1) 全局 AGENTXX_PLUGIN_API_VERSION 只覆盖核心契约 (核心 vtable 形状 +
 *      Info 结构 + 入口符号 + 本头共享类型); 宿主拒绝 api_version 不匹配的
 *      插件 (仅拒绝, 不崩溃) —— 老版本插件无兼容路径, 直接拒绝加载
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
 * 统一异步操作模型 (v1 核心设计, 见下方 "统一异步操作原语")
 * ════════════════════════════════════════════════════════════════════
 * - 工具执行 / 中间件钩子 / 能力方法 等"可能耗时的被调方操作"一律为
 *   异步三件套 start/poll/cancel —— 宿主在 io 线程驱动轮询, 与内置工具
 *   的 asio 协程在同一线程上交错执行; 插件不再被线程池黑盒阻塞执行,
 *   访问宿主会话数据天然单线程安全 (与宿主 assertIoThread 无锁模型一致)
 * - 被调方不需要任何异步库: 快同步代码在 start 内直接算完并通知完成
 *   (内联完成); 慢同步代码经 scheduler.offload 委托宿主阻塞池; 仅当插件
 *   想要真实并发 IO 时才需要自备 reactor (如私有 asio io_context)
 * - 纯 C 同步垫片 plugin_tool_sync.h 把传统同步函数一行宏包装成三件套
 * - 反向调用 (插件调用宿主工具 call_tool / 其他插件能力 invoke_capability)
 *   提供 AgentxxHostOp 句柄: 宿主内部同样在 io 线程驱动目标插件的三件套,
 *   插件任意线程经句柄查询结果 (阻塞便捷版内部轮询实现, 禁止 io 线程调用)
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
 *   - 异步三件套 start/poll/cancel 由宿主在【io 线程】驱动 (非阻塞快速
 *     返回约定: 单次调用不得超过 ~1ms, 长任务必须切片或委托 offload);
 *     AgentxxOpNotify.done 可从被调方任意线程回调 (线程安全)
 *   - AgentxxHostOp 方法任意线程可调用; 阻塞便捷版 call_tool/
 *     invoke_capability 禁止在宿主 io 线程调用 (会阻塞 io 且死锁, 宿主
 *     fail-fast 报错)
 * - 回调快速返回约定: 事件订阅回调与定时器回调在 io 线程同步调用, 必须
 *   快速返回, 不得阻塞 (长时间任务请经 offload 或自行投递到独立线程)
 * - 异常不外泄: 宿主接口表所有函数内部捕获全部异常 (C ABI 边界无异常);
 *   插件侧 start/poll/cancel/event 回调同样不得让异常逃逸 (宿主调用处
 *   已兜底, 但插件自身应遵循)
 * - 字符串约定:
 *   - 所有跨边界"字符串参数/字段"类型为 AgentxxPluginStringView (data+size,
 *     不要求 NUL 结尾, 生命周期仅覆盖本次调用); 便捷构造见
 *     agentxx_plugin_sv / agentxx_plugin_sv_cstr / AGENTXX_SV
 *   - 所有"宿主分配"的字符串返回值 (工具结果 / error_out / HostOp take
 *     payload / strdup/list_plugins/get_plugin/... ) 仍为 char* (NUL 结尾,
 *     host->alloc), 调用方用完必须 host->free
 */
#ifndef AGENTXX_PLUGIN_API_H
#define AGENTXX_PLUGIN_API_H

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 插件导出符号控制 ==================== */

/// 插件动态库默认隐藏全部符号 (构建时 -fvisibility=hidden / 不自动导出),
/// 仅入口符号经 AGENTXX_PLUGIN_EXPORT 显式导出 (宿主 dlsym/GetProcAddress
/// 按名查找): agentxx_plugin_get_info / agentxx_plugin_entry /
/// agentxx_plugin_unload / agentxx_client_get_info / agentxx_client_entry /
/// agentxx_client_unload。插件源码定义入口函数时必须加该宏前缀, 例如:
///   AGENTXX_PLUGIN_EXPORT extern "C" int agentxx_plugin_entry(...)
/// 内置合并编译模式 (AGENTXX_PLUGIN_BUILTIN=1) 下入口符号直接并入 libagentxx
/// (不经 dlopen), 无需导出, 宏展开为空。
/// 除入口符号外的全部符号 (含插件内部 C++ 符号、第三方静态库符号) 均隐藏,
/// 避免污染宿主动态符号表与多插件符号冲突。
#if defined(AGENTXX_PLUGIN_BUILTIN)
#define AGENTXX_PLUGIN_EXPORT
#elif defined(_WIN32)
#define AGENTXX_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define AGENTXX_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define AGENTXX_PLUGIN_EXPORT
#endif

/// 全局 API 版本: 只覆盖核心契约 (核心 vtable 形状 + Info 结构 + 入口符号 +
/// 本头共享类型)。接口表各自带 version 独立演进, 不影响本版本号。
/// 宿主精确匹配门禁: info.api_version != 本值 → 拒绝加载 (无历史兼容路径)。
#define AGENTXX_PLUGIN_API_VERSION 1

/* ==================== 字符串视图 (跨边界字符串参数统一形态) ==================== */

/// 只读字符串视图: 指向调用方内存 (UTF-8), 不要求 NUL 结尾
/// - 生命周期仅覆盖本次调用 (宿主不得保存引用; 需要保存必须拷贝)
/// - 空视图: data == NULL 或 size == 0 (视为空串)
typedef struct AgentxxPluginStringView {
    const char* data; ///< 指向 UTF-8 字节序列 (可含任意字节, 不必 NUL 结尾)
    size_t      size; ///< 字节数
} AgentxxPluginStringView;

/// 构造字符串视图 (data, size)
static inline AgentxxPluginStringView agentxx_plugin_sv(const char* data, size_t size) {
    AgentxxPluginStringView sv;
    sv.data = data;
    sv.size = size;
    return sv;
}

/// 从 NUL 结尾 C 字符串构造视图 (NULL 视为空视图)
static inline AgentxxPluginStringView agentxx_plugin_sv_cstr(const char* s) {
    AgentxxPluginStringView sv;
    sv.data = s;
    sv.size = s ? strlen(s) : 0;
    return sv;
}

/// 视图是否为空 (NULL 或长度 0)
static inline int agentxx_plugin_sv_empty(AgentxxPluginStringView sv) {
    return sv.data == NULL || sv.size == 0;
}

/// 便捷宏: 字符串字面量 / const char* → 视图
#define AGENTXX_SV(s) agentxx_plugin_sv_cstr((s))

/* ==================== 插件元信息 ==================== */

typedef struct AgentxxPluginInfo {
    int                     api_version; ///< 必须 == AGENTXX_PLUGIN_API_VERSION
    AgentxxPluginStringView name; ///< 唯一标识, 如 "agentxx_javascript_engine" (只读借用)
    AgentxxPluginStringView version;
    AgentxxPluginStringView description;
} AgentxxPluginInfo;

/* ==================== 统一异步操作原语 (v1 核心) ====================
 *
 * 所有"可能耗时的被调方操作" (工具执行 / 中间件钩子 / 能力方法) 共用同一
 * 三件套契约: start / poll / cancel —— 由宿主在【io 线程】驱动轮询, 与宿主
 * 内置工具的 asio 协程在同一线程上交错执行。被调方不需要任何异步库。
 *
 * start 的返回值约定 (三档实现模式):
 *   1. 内联完成型 (快同步, <~1ms): 直接算完 → notify->done(OK, 结果) →
 *      返回 NULL 且 *error_out == NULL —— 宿主不再 poll (poll/cancel 可留 NULL)
 *   2. 慢同步型: 打包任务经 scheduler.offload 委托宿主阻塞池 → 返回非 NULL
 *      句柄; poll 留 NULL (只等完成通知) 或返回 AGENTXX_OP_POLL_DONE 提示;
 *      cancel 把取消标志转发给任务
 *   3. 自管异步型 (真实并发 IO/自有 reactor): start 登记工作后立即返回句柄,
 *      poll 非阻塞推进状态机/reactor 并按需返回建议延迟, 完成时 notify
 *   启动失败: 返回 NULL 且 *error_out 输出错误信息 (host->alloc 分配)
 *   违约检测: 返回 NULL、无 error_out 且未触发 done → 宿主按协议错误处理
 */

/// 操作终结状态 (AgentxxOpNotify.done 的 status 参数)
#define AGENTXX_OP_OK        0 ///< 成功 (payload = 结果数据, host->alloc)
#define AGENTXX_OP_CANCELLED 1 ///< 已取消 (payload 可为 NULL)
#define AGENTXX_OP_FAILED    2 ///< 失败 (payload = 错误信息, host->alloc)

/// poll 返回值: 操作已终结 (notifier 已调或将在本次调用内被调);
/// 宿主此后停止 poll。其余返回值 >= 0 = 未完成 + 建议下次推进延迟毫秒
/// (0 = 尽快; 宿主据此让出执行权/小睡, 保证与其他协程交错且不空转)
#define AGENTXX_OP_POLL_DONE (-1)

/// 完成通知器 (宿主实现并随 start 下发; 操作终结时被调方须【恰好回调一次】)
/// - payload: host->alloc 分配的字符串, 所有权移交宿主 (可为 NULL, 如取消时)
/// - 线程安全: 可从被调方的任意线程回调 (含插件自有线程), 宿主内部投递回
///   io 线程唤醒等待协程
typedef struct AgentxxOpNotify {
    void (*done)(void* host_ud, int status, char* payload);
    void* host_ud;
} AgentxxOpNotify;

/// 推进函数通用形态 (【宿主 io 线程调用】, 非阻塞快速返回):
/// - user_data/op: 注册时与 start 返回的被调方私有数据/句柄
/// - 返回 AGENTXX_OP_POLL_DONE 或建议延迟毫秒 (见宏注释)
/// - 内联完成型可留 NULL (宿主只等完成通知)
typedef int (*AgentxxOpPollFn)(void* user_data, void* op);

/// 协作式取消请求函数 (【宿主 io 线程调用】, 非阻塞):
/// - 被调方应尽快收尾并经 notifier 上报 CANCELLED; 也允许选择继续完成并
///   上报 OK/FAILED (与内置工具"取消仅通知, 终态语义由实现决定"一致)
/// - 不可取消的操作可留 NULL
typedef void (*AgentxxOpCancelFn)(void* user_data, void* op);

/* ==================== 工具定义 ==================== */

#define AGENTXX_TOOL_FLAG_NONE         0
#define AGENTXX_TOOL_FLAG_AUTO_SUMMARY (1 << 0) ///< 输出超限时自动压缩 (经 share_store 卸载)

typedef struct AgentxxToolSpec {
    AgentxxPluginStringView name; ///< 须全局唯一 (与内置工具/MCP 工具同名将注册失败)
    AgentxxPluginStringView description;
    AgentxxPluginStringView parameters_json; ///< JSON Schema 字符串 (json object)

    /// 启动执行 (【宿主 io 线程调用】, 非阻塞; 通用契约见"统一异步操作原语"):
    /// - args_json/thread_id/tool_call_id: 只读借用, 仅本次调用有效
    /// - 快同步工具: 算完 → notify->done(AGENTXX_OP_OK, 结果 json) → 返回 NULL
    ///   (结果/错误字符串均须 host->alloc 分配)
    /// - 回调内可调用 call_tool / log / json_* 等 (注意 io 线程约束:
    ///   阻塞便捷版 call_tool/invoke_capability 在 io 线程会被 fail-fast 拒绝)
    void* (*execute_start)(
        void*                   user_data,
        AgentxxPluginStringView args_json,
        AgentxxPluginStringView thread_id,
        AgentxxPluginStringView tool_call_id,
        const AgentxxOpNotify*  notify,
        char**                  error_out
    );
    /// 推进执行 (io 线程, 非阻塞; 内联完成型可留 NULL)
    int (*execute_poll)(void* user_data, void* op);
    /// 协作式取消请求 (io 线程, 非阻塞; 不可取消可留 NULL)
    /// - 会话取消/宿主超时/放弃等待时由宿主调用; 插件应尽快收尾并上报终态
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

/// 钩子规格: 异步三件套形态 (与工具同构; 见"统一异步操作原语")
/// - hook_start (【宿主 io 线程调用】, 非阻塞):
///   * node_input_json: 节点输入摘要 ({"sessionId","point","messages_count",...})
///   * 快钩子: 处理完 → notify->done(OK, NULL) → 返回 NULL
///   * 慢钩子: 委托 offload / 登记异步工作 → 返回句柄 (宿主轮询推进)
/// - 每插件每钩子点至多注册一个 (重复注册覆盖旧值); 注销按 point + user_data
typedef struct AgentxxHookSpec {
    AgentxxHookPoint point;
    void* (*hook_start)(
        void*                   user_data,
        AgentxxHookPoint        point,
        AgentxxPluginStringView node_input_json,
        const AgentxxOpNotify*  notify,
        char**                  error_out
    );
    int (*hook_poll)(void* user_data, void* op);   ///< 可为 NULL (内联完成型)
    void (*hook_cancel)(void* user_data, void* op); ///< 可为 NULL
    void* user_data;
} AgentxxHookSpec;

/* ==================== 事件订阅句柄 / 宿主侧异步操作句柄 ==================== */

typedef struct AgentxxSubscription AgentxxSubscription;

struct AgentxxHost; ///< 前向声明 (能力方法处理器签名引用宿主句柄)

/// 能力方法处理器启动函数 (提供者注册; 通用插件间通信, 如 JS 引擎提供
/// "interpreter.js" 能力的 load/unload 方法):
/// - ctx: 提供者私有上下文
/// - caller_host: 调用方插件宿主句柄 (如脚本插件的 C++ 壳, 脚本内注册的
///   工具经此挂到调用方实例)
/// - method/args_json: 提供者自定义方法契约 (字符串视图, 只读借用)
/// - 三件套通用契约同工具 execute_start (见"统一异步操作原语"):
///   快方法内联完成; 慢方法 (如 JS 引擎加载脚本) 登记工作后返回句柄
/// - 结果 JSON 字符串经 notify->done(OK, payload) 上报 (host->alloc)
typedef void* (*AgentxxCapStartFn)(
    void*                   ctx,
    const AgentxxHost*      caller_host,
    AgentxxPluginStringView method,
    AgentxxPluginStringView args_json,
    const AgentxxOpNotify*  notify,
    char**                  error_out
);

/* ---- 宿主侧异步操作句柄 (反向原语: 插件驱动宿主内部驱动的操作) ----
 * call_tool_async / invoke_capability_async 返回: 目标插件的三件套由宿主
 * 在 io 线程自动驱动推进, 本句柄仅查询/取消/收尸 —— 全部方法任意线程可调
 * 用 (线程安全), 但【同一句柄的方法不得并发调用】(典型用法单线程轮询)。
 * 典型轮询循环:
 *   int st; char* payload;
 *   while (op->poll(op) != AGENTXX_OP_POLL_DONE) { sleep_ms(op->poll(op)); }
 *   op->take(op, &st, &payload);   // 恰一次; 之后句柄失效
 */
typedef struct AgentxxHostOp {
    /// 查询进度: AGENTXX_OP_POLL_DONE = 已终结 (随后 take); >=0 未完成 +
    /// 建议延迟毫秒 (宿主内部驱动在 io 线程进行, 本函数不阻塞)
    int (*poll)(struct AgentxxHostOp* op);
    /// 取终结结果 (恰一次): 0 成功并填充 out_status/out_payload (payload
    /// host->alloc, 调用方 free); 非 0 = 尚未终结 (应继续 poll)
    int (*take)(
        struct AgentxxHostOp* op,
        int*                  out_status,
        char**                out_payload
    );
    /// 请求协作式取消 (转发给目标操作; 可多次调用; 句柄仍须 poll→take 收尸)
    void (*cancel)(struct AgentxxHostOp* op);
    /// 放弃句柄 (未终结时转为后台收割, 终态结果丢弃并释放资源;
    /// 之后句柄失效不得再用)
    void (*free)(struct AgentxxHostOp* op);
    void* internal; ///< 宿主内部状态 (插件不得使用/释放)
} AgentxxHostOp;

/* ==================== 核心宿主函数表 (契约冻结) ==================== */

typedef struct AgentxxHost AgentxxHost;

/// 核心 vtable: 仅内存三件套 + COM 风格接口表查询。
/// 【契约冻结】本结构自 v1 起不再增删成员: 一切宿主能力经 query_interface
/// 按稳定 IID 查询独立接口表获取, 未来新增能力不修改本结构。
typedef struct AgentxxHostVtable {
    /* ---- 内存 (跨 CRT 堆边界的唯一分配通道; 任意线程可调用) ---- */
    void* (*alloc)(size_t size);
    void (*free)(void* ptr);
    char* (*strdup)(const char* s);

    /* ---- COM 风格接口表查询 (QueryInterface; 任意线程可调用) ---- */
    /// 按稳定 IID 字符串查询接口表; 返回以 int version 为首字段的只读函数表
    /// (类型按 IID 对应头文件结构体解释), 未实现/未知名称返回 NULL (安全失败)
    /// - 已知 IID 见下方各 AGENTXX_IFACE_AGENT_* 宏与对应 *Iface 结构体
    /// - 接口表为进程级静态数据: 返回指针长期有效, 可在 entry 时查询缓存;
    ///   表内函数指针可能为 NULL (宿主未实现该子能力), 调用前必须判空
    const void* (*query_interface)(const AgentxxHost* host, AgentxxPluginStringView iid);
} AgentxxHostVtable;

struct AgentxxHost {
    const AgentxxHostVtable* vtable; ///< 核心函数表 (宿主静态)
    void* opaque; ///< 宿主内部 (指向插件实例状态, 插件不得使用)
};

/// 便捷宏: 查询接口表并转型 (iid_name 为 AGENTXX_IFACE_* 宏或等价字符串字面量)
#define AGENTXX_QUERY_IFACE(host, IfaceType, iid_name)                                        \
    ((const IfaceType*)(host)->vtable->query_interface((host), AGENTXX_SV(iid_name)))

/* ==================== 接口表: 工具 (agentxx.agent.tools) ==================== */

#define AGENTXX_IFACE_AGENT_TOOLS         "agentxx.agent.tools"
#define AGENTXX_IFACE_AGENT_TOOLS_VERSION 2

typedef struct AgentxxToolsIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_TOOLS_VERSION

    /// 注册工具 (io 线程约束, 非 io 线程由宿主投递同步等待); 名称冲突返回非 0
    /// - spec 内容注册时拷贝 (字符串深拷贝), 调用后即可释放
    int (*register_tool)(const AgentxxHost* host, const AgentxxToolSpec* spec);
    /// 注销工具 (按名称); 不存在返回非 0
    int (*unregister_tool)(const AgentxxHost* host, AgentxxPluginStringView name);

    /* ---- 插件互调: 异步原语 (推荐) ---- */
    /// 异步调用插件工具 (仅限插件注册的工具, 不暴露宿主内置工具):
    /// - 目标工具三件套由宿主在 io 线程自动驱动, 调用方任意线程经句柄轮询
    /// - 查表/装配失败返回 NULL 并 error_out
    AgentxxHostOp* (*call_tool_async)(
        const AgentxxHost*      host,
        AgentxxPluginStringView name,
        AgentxxPluginStringView args_json,
        AgentxxPluginStringView thread_id,
        char**                  error_out
    );
    /// 阻塞便捷版 (内部轮询 call_tool_async 实现): 返回结果 JSON 字符串
    /// (host->alloc), 失败返回 NULL 并 error_out
    /// - 【禁止在宿主 io 线程调用】(io 线程阻塞会饿死内部驱动 → 死锁,
    ///   宿主 fail-fast 报错); 适用于 offload 工作线程 / entry / 自有线程
    char* (*call_tool)(
        const AgentxxHost*      host,
        AgentxxPluginStringView name,
        AgentxxPluginStringView args_json,
        AgentxxPluginStringView thread_id,
        char**                  error_out
    );
} AgentxxToolsIface;

/* ==================== 接口表: 中间件钩子 (agentxx.agent.hooks) ==================== */

#define AGENTXX_IFACE_AGENT_HOOKS         "agentxx.agent.hooks"
#define AGENTXX_IFACE_AGENT_HOOKS_VERSION 2

typedef struct AgentxxHooksIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_HOOKS_VERSION

    /// 注册钩子 (异步三件套规格; 热插拔, 轮次边界生效; io 线程约束)
    /// - 每插件每钩子点至多一个, 重复注册覆盖旧值; 返回 0 成功
    int (*register_hook)(const AgentxxHost* host, const AgentxxHookSpec* spec);
    /// 注销钩子 (按 point 匹配 —— 每插件每钩子点至多一个); 不存在返回非 0
    int (*unregister_hook)(const AgentxxHost* host, AgentxxHookPoint point);
} AgentxxHooksIface;

/* ==================== 接口表: 事件 (agentxx.agent.events) ==================== */

#define AGENTXX_IFACE_AGENT_EVENTS         "agentxx.agent.events"
#define AGENTXX_IFACE_AGENT_EVENTS_VERSION 1

typedef struct AgentxxEventsIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_EVENTS_VERSION

    /// 订阅 (topic 自动加 "plugin." 前缀, 载荷为 JSON 字符串); 返回句柄
    /// (宿主侧持有, 插件卸载时自动退订)
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
#define AGENTXX_IFACE_AGENT_CAPABILITIES_VERSION 2

typedef struct AgentxxCapabilitiesIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_CAPABILITIES_VERSION

    /// 声明能力 (无方法处理器; 仅标记/互查; io 线程约束)
    int (*register_capability)(const AgentxxHost* host, AgentxxPluginStringView capability);
    /// 注册能力并附带异步方法处理器三件套 (通用插件间通信通道; 如 JS 引擎
    /// 注册 "interpreter.js" 提供 load/unload 方法; 三件套契约见
    /// "统一异步操作原语"): 同名能力重复注册失败 (能力委派需唯一 provider)
    int (*register_capability_ex)(
        const AgentxxHost*      host,
        AgentxxPluginStringView capability,
        AgentxxCapStartFn       start,
        AgentxxOpPollFn         poll,   ///< 可为 NULL (内联完成型方法)
        AgentxxOpCancelFn       cancel, ///< 可为 NULL
        void*                   ctx
    );
    int (*unregister_capability)(const AgentxxHost* host, AgentxxPluginStringView capability);
    /// 是否存在指定能力 (io 线程查表)
    int (*has_capability)(const AgentxxHost* host, AgentxxPluginStringView capability);

    /* ---- 能力调用: 异步原语 (推荐) ---- */
    /// 异步调用能力提供者的方法: 提供者三件套由宿主在 io 线程自动驱动,
    /// 调用方任意线程经句柄轮询; 查表/装配失败返回 NULL 并 error_out
    AgentxxHostOp* (*invoke_capability_async)(
        const AgentxxHost*      host,
        AgentxxPluginStringView capability,
        AgentxxPluginStringView method,
        AgentxxPluginStringView args_json,
        char**                  error_out
    );
    /// 阻塞便捷版 (内部轮询 invoke_capability_async 实现): 返回结果 JSON
    /// (host->alloc), 失败返回 NULL 并 error_out
    /// - 【禁止在宿主 io 线程调用】(同 call_tool 说明);
    ///   适用于 offload 工作线程 / entry / 自有线程
    char* (*invoke_capability)(
        const AgentxxHost*      host,
        AgentxxPluginStringView capability,
        AgentxxPluginStringView method,
        AgentxxPluginStringView args_json,
        char**                  error_out
    );
} AgentxxCapabilitiesIface;

/* ==================== 接口表: 任务调度 (agentxx.agent.scheduler) ==================== */

#define AGENTXX_IFACE_AGENT_SCHEDULER         "agentxx.agent.scheduler"
#define AGENTXX_IFACE_AGENT_SCHEDULER_VERSION 2

typedef struct AgentxxSchedulerIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_SCHEDULER_VERSION

    /// 当前线程是否为宿主 io 线程 (任意线程可调用)
    int (*is_io_thread)(const AgentxxHost* host);
    /// 投递任务到宿主 io 线程异步执行 (不等待, 线程安全)
    void (*post_to_io)(const AgentxxHost* host, void (*fn)(void* ud), void* ud);
    /// 周期定时器 (io 线程触发; 回调必须快速返回, 不得阻塞 io 线程)
    /// - interval_ms > 0; 返回句柄 (宿主持有); 插件卸载时宿主自动取消全部
    ///   定时器, 回调不会在插件代码段卸载后触发
    /// - 回调执行期间插件代码段由宿主保活 (inflight 计数); 回调内可调用
    ///   publish / offload / log 等任意 API
    void* (*add_timer)(const AgentxxHost* host, long interval_ms, void (*fn)(void* ud), void* ud);
    /// 取消定时器 (句柄随后失效; 插件卸载后句柄自动失效, 不得再调用)
    void (*cancel_timer)(const AgentxxHost* host, void* timer);

    /* ---- 阻塞池委托 (慢同步操作的官方逃生通道) ---- */
    /// 在宿主阻塞线程池执行同步回调 (阻塞操作专用: 文件遍历/HTTP/子进程等;
    /// 池线程数有限, 禁止长时间占用)
    /// - cancel_flag: 【调用方持有】的取消标志 (volatile int; 0=未取消
    ///   1=已取消), done 返回前必须保持有效 —— 典型用法: 封装进任务上下文,
    ///   异步操作被 cancel 时置 1, work 内周期轮询协作退出; 无取消需求时
    ///   可传入指向静态 0 值的指针
    /// - work: 在阻塞池线程执行, 收到上述 cancel_flag; 返回结果与 error_out
    ///   须 host->alloc 分配
    /// - done: work 返回后投递回 io 线程执行 (快速返回约定; result 为 work
    ///   返回值, error 为 work 填充的错误; 两者均须在 done 内 host->free)
    /// - work/done 执行期间插件代码段由宿主保活 (inflight 计数); 插件卸载
    ///   时宿主等待在途 offload 完成后再调 unload 回调
    /// - 任意线程可调用
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
} AgentxxSessionIface;

/* ==================== 接口表: 插件互查 (agentxx.agent.plugins) ==================== */

#define AGENTXX_IFACE_AGENT_PLUGINS         "agentxx.agent.plugins"
#define AGENTXX_IFACE_AGENT_PLUGINS_VERSION 1

typedef struct AgentxxPluginsIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_PLUGINS_VERSION

    /// 全部已安装插件信息 JSON 数组 (host->alloc 分配):
    /// [{"name","version","description","type","enabled","tools":[],"capabilities":[],
    ///   "depends":[],"optional_depends":[]}, ...]
    char* (*list_plugins)(const AgentxxHost* host);
    /// 单个插件信息 JSON (同上单对象; 未安装返回 NULL, host->alloc)
    char* (*get_plugin)(const AgentxxHost* host, AgentxxPluginStringView name);
    /// 调用方插件自身信息 JSON {"name","version","description","path","depends":[],...}
    /// (插件加载时常用: 从 path 推导资源目录等; host->alloc)
    char* (*get_own_info)(const AgentxxHost* host);
} AgentxxPluginsIface;

/* ==================== 接口表: 宿主配置 (agentxx.agent.config) ==================== */

#define AGENTXX_IFACE_AGENT_CONFIG         "agentxx.agent.config"
#define AGENTXX_IFACE_AGENT_CONFIG_VERSION 2

typedef struct AgentxxConfigIface {
    int version; ///< 必须 >= AGENTXX_IFACE_AGENT_CONFIG_VERSION

    /// 宿主 AgentConfig 关键字段 JSON (io 线程; host->alloc):
    /// {"dataDir": "...", "projectRoot": "..."(可为空),
    ///  "platform": "windows"|"linux"|"macos"}
    /// - 通用宿主信息; 插件业务参数请用 get_plugin_args (宿主不解析 args)
    char* (*get_config)(const AgentxxHost* host);
    /// 本插件配置参数 JSON (yaml `plugins` 条目 args; io 线程; host->alloc):
    /// - 宿主对 args 内容完全不解析, 整体原样传递 (参数语义由插件定义,
    ///   如 agentxx_codegraph 的 loadPaths/ignorePaths/loadCwd/useGitignore)
    /// - 未配置时返回 "{}"
    char* (*get_plugin_args)(const AgentxxHost* host);
    /// 宿主 toolPrompt 配置 (io 线程; host->alloc):
    /// {"depict": "...", "args": {"参数名": "参数说明", ...}}
    /// - 工具未配置 prompt 时返回 NULL (插件回退内置默认描述)
    /// - 供插件注册工具时生成与内置工具一致的动态描述 (用户可经 yaml 覆盖)
    char* (*get_tool_prompt)(const AgentxxHost* host, AgentxxPluginStringView tool_name);

    /* ---- v2 追加: 会话工作目录 ---- */
    /// 解析后的会话工作目录 (io 线程; host->alloc; 失败/未装配返回 NULL):
    /// - AgentConfig::resolvedWorkDir(): yaml work_dir 优先, 为空回退进程 cwd
    /// - 文件系统/命令执行类插件以此为相对路径基准与子进程初始目录
    ///   (嵌入多实例场景下各 agent 实例的工作目录彼此独立)
    char* (*get_work_dir)(const AgentxxHost* host);
} AgentxxConfigIface;

/* ==================== 接口表: 主模型配置 (agentxx.agent.model) ====================
 * 供插件按需获取宿主主模型配置 (embedding / 模型搜索等衍生能力复用同一
 * 服务商配置, 与原 lib 内置工具行为一致); 注意 apiKey 会透出给查询者,
 * 仅本项目内置插件使用
 */
#define AGENTXX_IFACE_AGENT_MODEL         "agentxx.agent.model"
#define AGENTXX_IFACE_AGENT_MODEL_VERSION 1

typedef struct AgentxxModelIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_MODEL_VERSION

    /// 宿主主模型及关联配置 JSON (io 线程; host->alloc; 未装配返回 NULL):
    /// {"baseUrl": "...", "apiKey": "...", "modelName": "...",
    ///  "websearchApiUrl": "...", "websearchConvertHtml2markdown": true|false,
    ///  "websearchModel": {...}|null,      // 模型搜索覆盖配置 (ModelConfig 同构)
    ///  "ragDocsPaths": ["...", ...]}      // RAG 文档扫描路径
    char* (*get_config)(const AgentxxHost* host);
} AgentxxModelIface;

/* ==================== 接口表: 会话取消状态 (agentxx.agent.cancel) ====================
 * 长任务型工具/钩子 (如命令执行) 可经本接口轮询会话取消令牌, 在 poll 切片
 * 或 offload work 内自行提前终止子任务; 宿主也会在会话取消时调用操作的
 * cancel 回调 (协作式通知)
 */
#define AGENTXX_IFACE_AGENT_CANCEL         "agentxx.agent.cancel"
#define AGENTXX_IFACE_AGENT_CANCEL_VERSION 1

typedef struct AgentxxCancelIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_CANCEL_VERSION

    /// 查询会话当前轮次是否已取消 (任意线程可调用, 宿主内部同步到 io 线程;
    /// 会话不存在或无取消令牌返回 0): 1 = 已取消, 0 = 未取消
    int (*is_cancelled)(const AgentxxHost* host, AgentxxPluginStringView thread_id);
} AgentxxCancelIface;

/* ==================== 接口表: 任务规划 (agentxx.agent.planning) ====================
 * planning_write 工具的宿主侧落地接口: 写入 PlanningMiddlewareHandle 的
 * 会话规划 state (system prompt 注入链路读取), 见 middlewares/planning.h
 */
#define AGENTXX_IFACE_AGENT_PLANNING         "agentxx.agent.planning"
#define AGENTXX_IFACE_AGENT_PLANNING_VERSION 1

typedef struct AgentxxPlanningIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_PLANNING_VERSION

    /// 写入指定会话的两层规划 + 备忘录 (任意线程可调用, 宿主内部同步到 io
    /// 线程): roadmap 必填 (Mermaid stateDiagram-v2 文本); todos_json 为
    /// todo 数组的 JSON 字符串 (空视图跳过); notes 为备忘录文本 (空视图跳过)。
    /// 返回 0 成功; 非 0 失败 (宿主未装配 PlanningMiddleware 等)
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

    /// 宿主完整提示词 JSON (io 线程; host->alloc):
    /// {"systemPrompt": "...", "systemPlanningPrompt": "...", "systemSkillPrompt": "...",
    ///  "toolPrompt": {"工具名": {"depict": "...", "args": {"参数名": "说明"}}}}
    /// - 宿主未装配 AgentConfig 时返回 NULL
    char* (*get_prompt)(const AgentxxHost* host);
    /// 合并更新宿主提示词 (io 线程; 仅覆盖 JSON 中出现的字段, 未出现字段保持不变)
    /// - 与宿主 AgentPrompt::mergeFromJson 语义一致: toolPrompt 条目不存在时插入
    /// - 插件卸载时, 其加载期间经本函数写入的提示词自动回滚 (恢复加载前状态),
    ///   不会残留插件默认文本; 返回 0 成功, 非 0 失败 (JSON 非法/宿主未就绪)
    /// - 典型用法: 插件注册工具前把内置默认描述写入宿主 toolPrompt, 用户可
    ///   继续经 yaml 覆盖 (覆盖发生在插件加载前, 插件写入前应先 get_prompt
    ///   检查条目是否已存在, 已存在则尊重用户配置不覆盖)
    int (*set_prompt)(const AgentxxHost* host, AgentxxPluginStringView prompt_json);
} AgentxxPromptIface;

/* ==================== 接口表: JSON 辅助 (agentxx.agent.json) ==================== */

#define AGENTXX_IFACE_AGENT_JSON         "agentxx.agent.json"
#define AGENTXX_IFACE_AGENT_JSON_VERSION 1

typedef struct AgentxxJsonIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_JSON_VERSION

    /// 从 JSON 字符串中提取指定 key 的字符串值 (宿主解析; 结果 host->alloc;
    /// 线程安全纯函数): key 缺失 / 值非字符串 / JSON 非法 返回 NULL
    char* (*json_get_string)(
        const AgentxxHost*      host,
        AgentxxPluginStringView json,
        AgentxxPluginStringView key
    );
    /// 字符串 → JSON 字符串字面量 (含引号包裹与转义; 结果 host->alloc;
    /// 线程安全纯函数): 用于拼装 JSON 时转义字段值 (防注入/语法错误)
    char* (*json_escape)(const AgentxxHost* host, AgentxxPluginStringView s);
} AgentxxJsonIface;

/* ==================== 接口表: 日志 (agentxx.agent.log) ==================== */

#define AGENTXX_IFACE_AGENT_LOG         "agentxx.agent.log"
#define AGENTXX_IFACE_AGENT_LOG_VERSION 1

typedef struct AgentxxLogIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_LOG_VERSION

    /// 日志 (线程安全); level 与宿主 XX_LOG 级别对应
    /// (0=trace 1=debug 2=info 3=warn 4=error)
    void (*log)(const AgentxxHost* host, int level, AgentxxPluginStringView msg);
} AgentxxLogIface;

/* ==================== 接口表: 会话资源贡献 (agentxx.agent.resources) ==================== */

#define AGENTXX_IFACE_AGENT_RESOURCES         "agentxx.agent.resources"
#define AGENTXX_IFACE_AGENT_RESOURCES_VERSION 1

typedef struct AgentxxResourcesIface {
    int version; ///< 必须 == AGENTXX_IFACE_AGENT_RESOURCES_VERSION

    /* ---- 插件向宿主贡献 Skill/Memory/MCP 组件, 由 agent-io 管线加载并经
            appendComponentInfo 上报客户端; 所有权归本插件: 卸载时自动摘除,
            禁用时摘除/启用时恢复; 冲突规则: 主配置 yaml 优先, 插件间先到先得,
            同插件重复注册幂等成功 (以下均为 io 线程约束) ---- */

    /// 追加 skill 扫描目录 (path 为目录, 含 SKILL.md 或其父目录;
    /// 绝对路径或相对程序工作目录); 与主配置冲突时拒绝并返回非 0
    int (*register_skill_dir)(const AgentxxHost* host, AgentxxPluginStringView path);
    /// 摘除本插件注册的 skill 目录; 不存在或不属于本插件返回非 0
    int (*unregister_skill_dir)(const AgentxxHost* host, AgentxxPluginStringView path);
    /// 追加 memory 上下文文件 (内容注入系统提示词); 冲突/所有权规则同上
    int (*register_memory_file)(const AgentxxHost* host, AgentxxPluginStringView path);
    int (*unregister_memory_file)(const AgentxxHost* host, AgentxxPluginStringView path);
    /// 注册 MCP server (异步连接; 命名空间查重通过即返回 0, 立即返回不等待网络):
    /// spec_json: {"namespace": "...", "url": "https://...", "timeout": 60(秒,可选)}
    /// - 连接完成后工具动态进入工具表 (下一轮对模型可见); 连接失败仅记日志,
    ///   命名空间随即释放 (可重新注册); 命名空间冲突返回非 0 (yaml 优先)
    int (*register_mcp_server)(const AgentxxHost* host, AgentxxPluginStringView spec_json);
    /// 注销 MCP server (断开连接 + 摘除其全部动态工具);
    /// 不存在或不属于本插件的命名空间返回非 0
    int (*unregister_mcp_server)(const AgentxxHost* host, AgentxxPluginStringView name_space);
    /// 本插件当前注册的资源快照 JSON (调试/自检; io 线程; host->alloc):
    /// {"skills":[...],"memory":[...],"mcp":[ns,...]}
    /// 宿主未装配资源应用器 (BaseAgent 场景) 时返回 NULL
    char* (*get_own_resources)(const AgentxxHost* host);
} AgentxxResourcesIface;

/* ==================== 插件入口符号 (dlsym) ==================== */

/// 可选: 查询插件元信息 (加载前调用, 用于版本/信息校验; 未导出则跳过)
typedef const AgentxxPluginInfo* (*AgentxxPluginGetInfoFn)(void);

/// 必需: 插件入口 (宿主线程池调用; 内部注册动作宿主会自动投递回 io 线程)
/// - host: 本插件专属宿主句柄 (opaque 已关联本插件)
/// - plugin_ctx: 输出插件私有上下文 (透传给 unload)
/// - 返回 0 成功; 非 0 加载失败 (宿主 dlclose 并报告错误)
/// - 线程说明: entry 运行在宿主线程池, 但其中经接口表的 io 线程约束操作
///   由宿主自动投递回 io 线程串行执行 (宿主内部处理, 插件无感; 因此 entry
///   内可安全调用 register_tool / invoke_capability 等任意 API)
typedef int (*AgentxxPluginEntryFn)(const AgentxxHost* host, void** plugin_ctx);

/// 可选: 插件卸载通知 (宿主等全部在途回调完成后调用; 用于插件业务清理;
/// 宿主会在此之前自动反注册该插件的一切工具/钩子/订阅/能力)
typedef void (*AgentxxPluginUnloadFn)(void* plugin_ctx);

#define AGENTXX_PLUGIN_SYMBOL_GET_INFO "agentxx_plugin_get_info"
#define AGENTXX_PLUGIN_SYMBOL_ENTRY    "agentxx_plugin_entry"
#define AGENTXX_PLUGIN_SYMBOL_UNLOAD   "agentxx_plugin_unload"

/* ==================== 便捷宏 (插件侧使用) ==================== */

/// 在插件侧分配跨边界字符串 (必须用它, 不能直接用 malloc/strdup)
#define AGENTXX_STRDUP(host, s) ((host)->vtable->strdup((s)))

#ifdef __cplusplus
}
#endif

#endif /* AGENTXX_PLUGIN_API_H */
