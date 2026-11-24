/// agentxx/ffi_api.h —— libagentxx 对外 FFI C API (唯一跨版本稳定接口)
///
/// 用途: 供其他编程语言 (Python/Rust/Go/C#/Java/Node/Dart...) 经 FFI (ctypes/
///       JNA/wasm-abi/ffigen 等) 链接 libagentxx_shared 动态库, 在嵌入方程序进程内
///       嵌入 agent 会话运行。导出符号全部为纯 C (extern "C" + 无 STL/异常外泄),
///       动态库经 version script / 导出列表严格控制导出面 (见 lib/ffi_symbols.map)。
///
/// ABI 规范:
///   - 明确 8 字节结构体对齐: #pragma pack(push, 8) / #pragma pack(pop)
///   - 定长基础类型: int32_t / int64_t / uint32_t / uint64_t
///   - 明确函数调用约定: AGENTXX_FFI_CALL (Windows 下为 __stdcall, x64 Unix 下为空)
///   - 统一字符串结构体:
///       AgentxxStringView: 只读借用视图 (const char* data + uint64_t size)
///       AgentxxString: 跨 CRT 堆分配字符串 (char* data + uint64_t size),
///       由 agentxx_ffi_string_free 释放
///   - 结构体传参与返回值:
///       所有结构体入参均通过指针传递 (const Struct*)
///       结构体返回值均改为指针出参 (Struct* out), 函数返回 int32_t 状态码 (0 成功)
///
/// 调用模型 (异步: 提交 + 事件回执):
///   - **唯一的调用入口是 agentxx_ffi_call**: 传一份
///     `{"op":"model.list","args":{...},"timeoutMs":10000}` 的请求 JSON, 立即返回
///     一个 `reqId`; 执行结果晚些经 `AGENTXX_FFI_EVT_RESULT` 事件送达
///     (`{reqId, op, ok, result?|error?}`)
///   - 同一句柄上可以同时有任意多个未完成调用, 每个 reqId 各自匹配自己的回执
///   - **每个受理成功的 reqId 恰好收到一条回执**: 成功、失败、超时、被取消、收尾中断
///     都算; 只有"同步参数校验失败"不会有回执 (那种情况不写 out_req_id, 返回错误码)
///   - 调用线程上**不会出现跨线程等待** —— 事件回调线程里也可以放心调 call/call_cancel
///   - 需要"前一步真的生效再发下一步"时, 先 await 前一步的回执再提交下一步; 同时提交
///     多个调用不保证完成顺序 (见 docs/zh-cn/design/ffi.md 的"顺序与取消")
///
/// 事件模型 (异步, 运行期在内部 client io 线程回调):
///   - 所有事件经 AgentxxFFICallbacks::on_event 回调, payload 是**归回调所有**的
///     `AgentxxString*`: 结构体与 JSON 正文在同一块堆内存上, 取完用
///     `agentxx_ffi_free(payload)` 释放整块 (想保存就自己拷贝, 别保留指针)
///   - 结构体在回调释放前一直有效, 因此绑定可以放心用 `NativeCallable.listener`
///     (Dart, 回调延后到 isolate 消息循环) / threadsafe function (Node) 一类线程安全投递
///   - 回调里可以调本 API 的任何函数 (call / call_cancel / stop / drain_logs ...);
///     回调必须快速返回, 长任务请自己投递到别的线程
///   - `AGENTXX_FFI_EVT_STOPPED` 是**最后一个**事件; 收到它之后不会再有回调
///
/// 错误处理约定:
///   - 同步错误: 返回值 = 错误码 (AGENTXX_FFI_OK=0 成功, 见 AGENTXX_FFI_ERR_*);
///     可选的 AgentxxString* log 参数非 NULL 时, 函数在出错时填入详情字符串,
///     嵌入方程序用后必须以 agentxx_ffi_string_free 释放; 成功/无日志时 log->data 为 NULL
///   - 异步错误 (执行期失败): 经 `EVT_RESULT` 的 `error` 字段上报, 其中 `error.name`
///     是稳定字符串 (SDK 按名字判定, 不受数字码调整影响); 与具体调用无关的异步错误
///     走 `EVT_ERROR`
///
/// 生命周期与线程:
///   - agentxx_ffi_create 在任意线程调用, 同步完成 (毫秒级: 解析配置 + 构造 agent)
///   - agentxx_ffi_start 受理即返回, 就绪后回调 `EVT_READY`
///   - agentxx_ffi_stop 受理即返回 (任意线程, 含事件回调内), 收尾完成后回调
///     `EVT_STOPPED` (幂等: 重复调用不再发事件); 收尾期间提交的 call 返回 ERR_STATE
///   - agentxx_ffi_destroy 只在 Created / Stopped 状态成功 (纯释放, 不再阻塞收尾);
///     其余状态返回 ERR_STATE 并提示"先 stop 并等 EVT_STOPPED"
#ifndef AGENTXX_FFI_API_H
#define AGENTXX_FFI_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// FFI API 版本
/// - 1 → 2: `agentxx_ffi_send_input` 的文本参数改为输入 JSON (支持附件)
/// - 2 → 3: 展示历史单写者 (wire 协议 v4), delta 改为按消息 id 寻址的操作集合
/// - 3 → 4: 导出面收敛为"提交 + 回执" (本文件)
///   - 35 个符号 → 15 个: 所有查询/命令都变成 `agentxx_ffi_call` 的 op, 新增能力只加
///     op 名, 不再加符号
///   - 删除全部同步阻塞查询 (`get_model_info` / `get_context_messages` /
///     `list_sessions` / `add_model` / `remove_model` / `list_models`)、
///     `send_wire` (与 `EVT_WIRE`) 与事件队列 (`event_queue_*` 4 个符号)
///   - 新增 `agentxx_ffi_call_cancel` 与 `EVT_RESULT` / `EVT_STOPPED`
///   - `on_event` 的 payload 改为归回调所有 (原来是回调期间有效的借用视图)
///   - `stop` 改为受理即返回 + `EVT_STOPPED`; `destroy` 变成纯释放
#define AGENTXX_FFI_API_VERSION 4

#if defined(_WIN32)
#define AGENTXX_FFI_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define AGENTXX_FFI_EXPORT __attribute__((visibility("default")))
#else
#define AGENTXX_FFI_EXPORT
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(__GNUC__) || defined(__clang__)
#define AGENTXX_FFI_CALL __attribute__((__stdcall__))
#else
#define AGENTXX_FFI_CALL __stdcall
#endif
#else
#define AGENTXX_FFI_CALL
#endif

#pragma pack(push, 8)

/* ==================== 错误码 ==================== */

#define AGENTXX_FFI_OK            0  ///< 成功
#define AGENTXX_FFI_ERR_INVALID   -1 ///< 参数非法 (NULL 句柄/空串/未知 op/字段类型错)
#define AGENTXX_FFI_ERR_STATE     -2 ///< 状态不允许 (未 start / 已 stop / 运行中 destroy)
#define AGENTXX_FFI_ERR_JSON      -3 ///< JSON 解析失败
#define AGENTXX_FFI_ERR_CONFIG    -4 ///< 配置非法 (模型缺失/字段类型错误)
#define AGENTXX_FFI_ERR_INIT      -5 ///< agent init 失败 (详见 log / EVT_ERROR)
#define AGENTXX_FFI_ERR_INTERRUPT -6 ///< 中断请求无效 (id 不存在/已应答/已过期)
#define AGENTXX_FFI_ERR_TIMEOUT   -7 ///< 本次调用超时 (也可出现在 EVT_RESULT 里)
#define AGENTXX_FFI_ERR_OOM       -8 ///< 内存分配失败
#define AGENTXX_FFI_ERR_CANCELED  -9 ///< 调用被 call_cancel 取消 / 收尾时仍未完成
#define AGENTXX_FFI_ERR_REJECTED -10 ///< 服务端拒绝 (error.name 给结构化原因)
#define AGENTXX_FFI_ERR_INTERNAL -99 ///< 内部异常 (log 含详细信息)

/* ==================== 基础字符串结构体 ==================== */

/// 只读字符串视图: 指向调用方内存 (UTF-8), 不要求 NUL 结尾
typedef struct AgentxxStringView {
    const char* data; ///< 指向 UTF-8 字节序列
    uint64_t    size; ///< 字节数

#ifdef __cplusplus
    AgentxxStringView() :
        data(nullptr),
        size(0) {}

    AgentxxStringView(const char* d, uint64_t n) :
        data(d),
        size(n) {}

    operator const AgentxxStringView*() const {
        return this;
    }

    bool empty() const {
        return data == nullptr || size == 0;
    }
#endif
} AgentxxStringView;

/// 跨 CRT 堆分配字符串 (具有显式所有权: 由嵌入方程序库分配, 调用方接管并负责释放)
typedef struct AgentxxString {
    char*    data; ///< 指向堆分配的 UTF-8 字节序列 (以 \0 结尾; 空时为 NULL)
    uint64_t size; ///< 字节数 (不含结尾 \0)

#ifdef __cplusplus
    AgentxxString() :
        data(nullptr),
        size(0) {}

    AgentxxString(char* d, uint64_t n) :
        data(d),
        size(n) {}

    operator const AgentxxString*() const {
        return this;
    }

    bool empty() const {
        return data == nullptr || size == 0;
    }
#endif
} AgentxxString;

/* ==================== 句柄 ==================== */

/// agent 会话运行时句柄 (不透明; 由 agentxx_ffi_create 创建, agentxx_ffi_destroy 销毁)
typedef struct AgentxxFFIAgent AgentxxFFIAgent;

/* ==================== 内存管理 ==================== */

/// 跨 CRT 堆分配原始字节
AGENTXX_FFI_EXPORT void* AGENTXX_FFI_CALL agentxx_ffi_malloc(uint64_t size);

/// 释放由 agentxx_ffi_malloc 分配的裸内存
AGENTXX_FFI_EXPORT void AGENTXX_FFI_CALL agentxx_ffi_free(const void* ptr);

/// 释放由 FFI 接口分配并返回的 AgentxxString 的**内容** (结构体由调用方持有)
/// - 用于出参式接口 (`agentxx_ffi_create` 的 log / `agentxx_ffi_drain_logs` 的 out 等)
/// - 事件回调的 payload 不是这种形态: 结构体与正文同在一块堆内存上,
///   用 `agentxx_ffi_free(payload)` 释放
AGENTXX_FFI_EXPORT void AGENTXX_FFI_CALL agentxx_ffi_string_free(AgentxxString* str);

/// 分配并拷贝字符串视图为 AgentxxString
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_strdup_n(const AgentxxStringView* s, AgentxxString* out);

/* ==================== 版本与错误信息 ==================== */

/// 当前 FFI API 版本 (>= AGENTXX_FFI_API_VERSION)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL agentxx_ffi_api_version(void);

/// 库版本字符串视图 (静态存储, 勿释放)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL agentxx_ffi_library_version(AgentxxStringView* out);

/// 错误码 → 静态字符串视图 (线程安全, 静态存储, 勿释放)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_strerror(int32_t code, AgentxxStringView* out);

/// 查询当前 FFI 运行时能力清单 (JSON):
/// `{"apiVersion":4,"libraryVersion":"<版本>","ops":[...],"features":[...]}`
/// - out 需经 agentxx_ffi_string_free 释放
/// - `ops` 是可用的 op 名全集, `features` 是本文档约定的能力标记
///   (`call_api` / `call_cancel` / `event_owned_payload` / `async_stop` / `op_ack`)
/// - 句柄可以为 NULL (此时给本库编译进来的能力清单)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_get_capabilities(AgentxxFFIAgent* a, AgentxxString* out, AgentxxString* log);

/* ==================== 事件回调 ==================== */

/// agent 事件种类
typedef enum AgentxxFFIEventType {
    AGENTXX_FFI_EVT_READY = 0,          ///< 服务端就绪, 可开始发送输入: {"sessionId"}
    AGENTXX_FFI_EVT_STOPPED,            ///< 收尾完成 (最后一个事件): {}
    AGENTXX_FFI_EVT_RESULT,             ///< 调用回执: {reqId, op, ok, result?|error?}
    AGENTXX_FFI_EVT_ERROR,              ///< 与具体调用无关的内部错误: {code, name, message}
    AGENTXX_FFI_EVT_SYNC,               ///< 完整/部分历史同步: wire sync JSON
    AGENTXX_FFI_EVT_DELTA,              ///< 流式增量: wire delta JSON (kind=text_token/...)
    AGENTXX_FFI_EVT_DELTA_TEXT_BATCH,   ///< 文本/思考增量合批: {msgId, kind, text}
    AGENTXX_FFI_EVT_TURN_END,           ///< 轮次结束: wire turn_result JSON
    AGENTXX_FFI_EVT_CONTEXT_STATS,      ///< 上下文统计: wire context_stats JSON
    AGENTXX_FFI_EVT_MODEL_INFO,         ///< 模型状态广播 (非回执): wire model_info JSON
    AGENTXX_FFI_EVT_COMPONENTS,         ///< 启动组件 (MCP/Skill/Memory/插件): wire JSON
    AGENTXX_FFI_EVT_MESSAGE_QUEUE,      ///< 消息队列变化: {sessionId, items[], state}
    AGENTXX_FFI_EVT_PERMISSION_STATE,   ///< 权限状态广播 (非回执): {fullAuth}
    AGENTXX_FFI_EVT_INTERRUPT_REQ,      ///< HIL 中断询问: {interruptId, sessionId, node, value, argJson}
    AGENTXX_FFI_EVT_INTERRUPT_EXPIRED,  ///< 中断已过期/已取消: {interruptId}
    AGENTXX_FFI_EVT_HOST_TOOL_CALL,     ///< 嵌入方程序工具调用请求
    AGENTXX_FFI_EVT_HOST_TOOL_CANCELED, ///< 嵌入方程序工具调用取消: {callId, reason}
    AGENTXX_FFI_EVT_PLUGIN_DATA,        ///< 插件事件转发: {topic, data}
} AgentxxFFIEventType;

/// 事件回调与嵌入方程序上下文
typedef struct AgentxxFFICallbacks {
    uint32_t struct_size; ///< = sizeof(AgentxxFFICallbacks); 用于跨版本扩展时判定字段可见性
    uint32_t reserved;    ///< 必须为 0
    /// 事件回调 (运行期在内部 client io 线程, 收尾时可能在收尾线程)
    /// - [payload] **归回调所有**: 它是库分配的一块内存, `data`/`size` 是 UTF-8 JSON 文本,
    ///   取完用 `agentxx_ffi_free(payload)` 释放整块 (不要用 `agentxx_ffi_string_free`:
    ///   那会把结构体当成调用方自己的栈变量)
    /// - 结构体与正文活到回调释放为止, 所以可以投递到别的线程/事件循环再处理
    ///   (Dart 的 `NativeCallable.listener` 就是这么用的); 不释放就等于内存泄漏
    void(AGENTXX_FFI_CALL* on_event)(int32_t type, AgentxxString* payload, void* user_data);
    /// 嵌入方程序上下文 (原样转给 on_event)
    void* user_data;
} AgentxxFFICallbacks;

/* ==================== 生命周期 ==================== */

/// 创建 agent 运行时句柄 (未启动; 同步构造对象与配置, 不启动线程)
/// - `args`:
///   - [config_json] NULL 或 AgentConfig 覆盖 JSON (支持 `syncTailCount` 等, 见 ffi.md)
///   - [model_json] 主模型 ModelConfig JSON (建议必填)
///   - [cb] 事件回调 (可 NULL = 纯 headless: 不做事件序列化)
///     结构体按值复制, 之后可以释放; 其中 `struct_size` 须填 `sizeof(AgentxxFFICallbacks)`,
///     `reserved` 须为 0
///   - [log] 非 NULL 时失败填入错误详情 (用后 agentxx_ffi_string_free 释放)
/// - `return` 句柄; 失败返回 NULL
AGENTXX_FFI_EXPORT AgentxxFFIAgent* AGENTXX_FFI_CALL agentxx_ffi_create(
    const AgentxxStringView*   config_json,
    const AgentxxStringView*   model_json,
    const AgentxxFFICallbacks* cb,
    AgentxxString*             log
);

/// 异步启动 (创建 io 线程 + init + 会话驱动循环): 受理即返回, 就绪后回调 EVT_READY
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_start(AgentxxFFIAgent* a, AgentxxString* log);

/// 异步停止: 受理即返回 (任意线程, 含事件回调内); 收尾完成后回调 EVT_STOPPED
/// - 幂等: 重复调用不再发事件; 一次成功的 stop 之后恰好一条 EVT_STOPPED
/// - 收尾期间仍在等待的调用会以 `code=-9 canceled` 收尾 (排在 EVT_STOPPED 之前)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_stop(AgentxxFFIAgent* a, AgentxxString* log);

/// 销毁句柄 (纯释放, 不阻塞收尾): 只在 Created / Stopped 状态成功
/// - 其余状态返回 AGENTXX_FFI_ERR_STATE (先 stop 并等 EVT_STOPPED 再 destroy)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_destroy(AgentxxFFIAgent* a, AgentxxString* log);

/* ==================== 调用 (唯一的异步入口) ==================== */

/// 提交一次调用
///
/// `request_json`:
/// ```json
/// { "op": "model.list", "args": { }, "timeoutMs": 10000 }
/// ```
/// - `op` 必填且必须是能力清单里给出的名字 (见 docs/zh-cn/design/ffi.md 的 op 表)
/// - `args` 可选, 必须是对象
/// - `timeoutMs` 可选: 本次调用的超时 (毫秒); `0` = 不超时; 缺省按 op 的默认值
///
/// - `args`:
///   - [a]            句柄
///   - [request_json] 请求 JSON 文本 (UTF-8)
///   - [out_req_id]   成功时写入本次调用的序号 (非 NULL); 同步失败时不写 (保持 0)
///   - [log]          同步失败的原因
/// - `return` AGENTXX_FFI_OK = 已受理 (结果经 `EVT_RESULT`); 其余为同步参数/状态错误
///   (这种失败不会产生回执)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL agentxx_ffi_call(
    AgentxxFFIAgent*         a,
    const AgentxxStringView* request_json,
    uint64_t*                out_req_id,
    AgentxxString*           log
);

/// 取消一次仍在等待的调用
///
/// - 只停止"这次等待", 不撤回已经提交给服务端的操作 (要撤回输入请用 `input.cancel`)
/// - 该调用立刻以 `code=-9 canceled` 收尾; 迟到的服务端应答被丢弃 (不会产生第二条回执)
/// - `req_id` 不存在 (已完成/从未发出) 返回 AGENTXX_FFI_OK, 并在 log 里说明
///   "already completed" —— 让竞态里的调用方不必把幂等取消当成错误
/// - 可以取消本地 op (它们本就立即完成, 实际上取消不到)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_call_cancel(AgentxxFFIAgent* a, uint64_t req_id, AgentxxString* log);

/* ==================== 本地同步 (不跨线程等待) ==================== */

/// 取走运行期间积压的日志条目 (JSON 数组 [{"level","message"},...]; 取走后清空;
/// agentxx_ffi_string_free 释放)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_drain_logs(AgentxxFFIAgent* a, AgentxxString* out, AgentxxString* log);

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

/* ==================== C/C++ 便捷内联函数 ====================
 *
 * 语言链接说明: 本区位于 extern "C" 之外。原因 —— C++ 下含成员函数的
 * struct (AgentxxStringView/AgentxxString) 是与 C 不兼容的类型, 带 C 链接
 * 的函数按值返回它们会触发 MSVC C4190 (warning C4190)。这些 static
 * inline 便捷工具仅供调用方源码内部使用, 不参与跨边界 ABI (无跨 TU
 * 符号); 纯 C 编译下本区等价普通 C 顶层函数 (类型为纯 POD), 语义与
 * 可用性不受影响。
 */

static inline AgentxxStringView agentxx_string_view(const char* s, uint64_t size) {
    AgentxxStringView sv;
    sv.data = s;
    sv.size = size;
    return sv;
}

static inline AgentxxStringView agentxx_string_view_cstr(const char* s) {
    AgentxxStringView sv;
    sv.data = s;
    sv.size = 0;
    if (s != NULL) {
        while (s[sv.size] != '\0') {
            sv.size++;
        }
    }
    return sv;
}

static inline AgentxxString agentxx_string_empty(void) {
    AgentxxString s;
    s.data = NULL;
    s.size = 0;
    return s;
}

#ifdef __cplusplus
static inline void agentxx_ffi_string_free(AgentxxString& str) {
    agentxx_ffi_string_free(&str);
}
#endif

#endif /* AGENTXX_FFI_API_H */
