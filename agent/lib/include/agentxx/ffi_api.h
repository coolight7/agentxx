/**
 * agentxx/ffi_api.h —— libagentxx 对外 FFI C API (唯一跨版本稳定接口)
 *
 * 用途: 供其他编程语言 (Python/Rust/Go/C#/Java/Node...) 经 FFI (ctypes/
 *       JNA/wasm-abi 等) 链接 libagentxx_shared 动态库, 在宿主进程内嵌入
 *       agent 会话运行。导出符号全部为纯 C (extern "C" + 无 STL/异常外泄),
 *       动态库经 version script / 导出列表严格控制导出面 (见 lib/ffi_symbols.map)。
 *
 * ABI 规范:
 *   - 明确 8 字节结构体对齐: #pragma pack(push, 8) / #pragma pack(pop)
 *   - 定长基础类型: int32_t / int64_t / uint32_t / uint64_t
 *   - 明确函数调用约定: AGENTXX_FFI_CALL (Windows 下为 __stdcall, x64 Unix 下为空)
 *   - 统一字符串结构体:
 *       AgentxxStringView: 只读借用视图 (const char* data + uint64_t size)
 *       AgentxxString: 跨 CRT 堆分配字符串 (char* data + uint64_t size), 由 agentxx_ffi_string_free
 * 释放
 *   - 结构体传参与返回值:
 *       所有结构体入参均通过指针传递 (const Struct*)
 *       结构体返回值均改为指针出参 (Struct* out), 函数返回 int32_t 状态码 (0 成功)
 *
 * 架构: 本 API 是 "client 端点" 的 C 抽象 —— 内部实现一个自定义 AgentIOBase
 *       端点 (FfiClientAgentIO), 经进程内 ChannelAgentIOTransport 与
 *       SessionServerAgentIO (由 BaseAgent 驱动) 通信, 与 TUI/CLI client
 *       完全同构 (参考 agent/client/src/mode_runners.cpp setupLocalUnifiedDirect)。
 *
 * 事件模型 (异步, client io 线程回调):
 *   - 所有事件经 AgentxxFFICallbacks::on_event 回调, payload 为 JSON 字符串视图
 *     (与服务端 wire 协议 JSON 字段一致, 见 agentxx/agent/io/wire_protocol.h)
 *   - 回调在 client io 线程同步调用, 必须快速返回 (不得阻塞; 长时间任务请
 *     投递到宿主自己的线程池)
 *   - payload 字符串视图仅回调期间有效, 需保存必须自行拷贝
 *   - 回调内可调用本 API (自动投递回 io 线程, 不死锁); 但不得调用
 *     agentxx_ffi_stop/agentxx_ffi_destroy
 *
 * 错误处理约定:
 *   - 同步错误: 返回值 = 错误码 (AGENTXX_FFI_OK=0 成功, 见 AGENTXX_FFI_ERR_*);
 *     可选的 AgentxxString* log 参数非 NULL 时, 函数在出错时填入详情字符串,
 *     宿主用后必须以 agentxx_ffi_string_free 释放; 成功/无日志时 log->data 为 NULL
 *   - 异步错误: 经 EVT_ERROR / EVT_TURN_END(hasError) 事件上报
 *
 * 生命周期与线程:
 *   - agentxx_ffi_create 在宿主任意线程调用; 成功后:
 *       agentxx_ffi_start 启动内置 agent io 线程, 异步就绪 (EVT_READY)
 *       agentxx_ffi_stop 同步停止并等待 io 线程退出 (幂等)
 *       agentxx_ffi_destroy 销毁句柄 (未 stop 时自动 stop)
 *   - stop/destroy 必须在回调线程以外调用 (内部 io 线程内调用返回
 *     AGENTXX_FFI_ERR_STATE); 其余 API 任意线程可调用
 *   - 同一句柄的所有非同步 API 可并发调用 (内部投递 io 线程串行执行);
 *     同步查询类 API (get_model_info/get_context_messages/list_sessions)
 *     同一时刻同一句柄仅允许一个调用尚未返回
 */
#ifndef AGENTXX_FFI_API_H
#define AGENTXX_FFI_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AGENTXX_FFI_API_VERSION 1

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
#define AGENTXX_FFI_ERR_INVALID   -1 ///< 参数非法 (NULL 句柄/空串/非法 JSON 等)
#define AGENTXX_FFI_ERR_STATE     -2 ///< 状态错误 (未 start/已 stop 后调用、io 线程内 stop 等)
#define AGENTXX_FFI_ERR_JSON      -3  ///< JSON 解析失败
#define AGENTXX_FFI_ERR_CONFIG    -4  ///< 配置非法 (模型缺失/字段类型错误)
#define AGENTXX_FFI_ERR_INIT      -5  ///< agent init 失败 (详见 log/EVT_ERROR)
#define AGENTXX_FFI_ERR_INTERRUPT -6  ///< 中断请求无效 (id 不存在/已应答/已过期)
#define AGENTXX_FFI_ERR_TIMEOUT   -7  ///< 同步查询等待响应超时
#define AGENTXX_FFI_ERR_OOM       -8  ///< 内存分配失败
#define AGENTXX_FFI_ERR_INTERNAL  -99 ///< 内部异常 (log 含详细信息)

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

/// 跨 CRT 堆分配字符串 (具有显式所有权: 由宿主库分配, 调用方接管并负责释放)
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

/// 释放由 FFI 接口分配并返回的 AgentxxString
AGENTXX_FFI_EXPORT void AGENTXX_FFI_CALL agentxx_ffi_string_free(AgentxxString* str);

/// 分配并拷贝字符串视图为 AgentxxString
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_strdup_n(const AgentxxStringView* s, AgentxxString* out);

/* ==================== 版本与错误信息 ==================== */

/// 当前 FFI API 版本 (== AGENTXX_FFI_API_VERSION)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL agentxx_ffi_api_version(void);

/// 库版本字符串视图 (静态存储, 勿释放)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL agentxx_ffi_library_version(AgentxxStringView* out);

/// 错误码 → 静态字符串视图 (线程安全, 静态存储, 勿释放)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_strerror(int32_t code, AgentxxStringView* out);

/* ==================== 事件回调 ==================== */

/// agent 事件种类
typedef enum AgentxxFFIEventType {
    AGENTXX_FFI_EVT_READY = 0,     ///< 服务端就绪, 可开始发送输入: {"sessionId"}
    AGENTXX_FFI_EVT_SYNC,          ///< 全量/部分历史同步: wire sync JSON
    AGENTXX_FFI_EVT_DELTA,         ///< 流式增量: wire delta JSON (kind=text_token/...)
    AGENTXX_FFI_EVT_TURN_END,      ///< 轮次结束: wire turn_result JSON
    AGENTXX_FFI_EVT_CONTEXT_STATS, ///< 上下文统计: wire context_stats JSON
    AGENTXX_FFI_EVT_MODEL_INFO,    ///< 模型信息 (查询/切换结果): wire model_info JSON
    AGENTXX_FFI_EVT_COMPONENTS, ///< 启动组件 (MCP/Skill/Memory/插件): wire append_component_info
                                ///< JSON
    AGENTXX_FFI_EVT_INTERRUPT_REQ,     ///< HIL 中断询问 (权限确认/输入收集)
    AGENTXX_FFI_EVT_INTERRUPT_EXPIRED, ///< 中断已过期/已取消: {"interruptId"}
    AGENTXX_FFI_EVT_PLUGIN_DATA,       ///< 插件事件转发: wire plugin_data JSON
    AGENTXX_FFI_EVT_ERROR,             ///< 内部错误: {"code","message"}
} AgentxxFFIEventType;

typedef struct AgentxxFFICallbacks {
    /// 事件回调 (client io 线程; payload_json 仅回调期间有效)
    void(AGENTXX_FFI_CALL*
             on_event)(int32_t type, const AgentxxStringView* payload_json, void* user_data);
    /// 宿主上下文 (原样透传给 on_event)
    void* user_data;
} AgentxxFFICallbacks;

/* ==================== 生命周期 ==================== */

/**
 * 创建 agent 运行时句柄 (未启动; 构造对象, 不启动线程)
 * @param config_json NULL 或 AgentConfig 覆盖 JSON
 * @param model_json 主模型 ModelConfig JSON (建议必填)
 * @param cb 事件回调 (可 NULL = 纯 headless; 内部值拷贝, 回调期间必有效)
 * @param log 非 NULL 时失败填入错误详情 (用后 agentxx_ffi_string_free 释放)
 * @return 句柄; 失败返回 NULL
 */
AGENTXX_FFI_EXPORT AgentxxFFIAgent* AGENTXX_FFI_CALL agentxx_ffi_create(
    const AgentxxStringView*   config_json,
    const AgentxxStringView*   model_json,
    const AgentxxFFICallbacks* cb,
    AgentxxString*             log
);

/// 异步启动 (创建 io 线程 + init + 会话驱动循环):
/// - 立即返回 AGENTXX_FFI_OK 表示已受理; 就绪后回调 EVT_READY
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_start(AgentxxFFIAgent* a, AgentxxString* log);

/// 同步停止并回收 (阻塞到 io 线程退出; 幂等; 不得在回调线程内调用)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_stop(AgentxxFFIAgent* a, AgentxxString* log);

/// 销毁句柄 (未 stop 时自动 stop; 之后句柄失效; 不得在回调线程内调用)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_destroy(AgentxxFFIAgent* a, AgentxxString* log);

/* ==================== 会话交互 (异步, 投递 io 线程执行) ==================== */

/// 发送用户输入 (EVT_READY 前发送会缓存, 就绪后按序处理)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_send_input(AgentxxFFIAgent* a, const AgentxxStringView* text, AgentxxString* log);

/// 请求取消当前轮次
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_cancel(AgentxxFFIAgent* a, AgentxxString* log);

/// 切换当前会话所用模型 (结果经 EVT_MODEL_INFO 通知)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL agentxx_ffi_select_model(
    AgentxxFFIAgent*         a,
    const AgentxxStringView* model_name,
    AgentxxString*           log
);

/// 记住权限选择 (服务端注册路径规则, 后续同路径不再询问):
/// op: 0=读取 1=写入; allow: 1=允许 0=拒绝
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL agentxx_ffi_set_permission(
    AgentxxFFIAgent*         a,
    const AgentxxStringView* path,
    int32_t                  allow,
    int32_t                  op,
    AgentxxString*           log
);

/// 切换当前连接会话 (sessionId 为空 = 关闭持久化时非法):
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL agentxx_ffi_switch_session(
    AgentxxFFIAgent*         a,
    const AgentxxStringView* sessionId,
    AgentxxString*           log
);

/* ==================== 同步查询 (阻塞等待服务端响应, 最长 10s) ====================
 * 返回值: int32_t 状态码 (AGENTXX_FFI_OK 成功); out 填入 JSON 结果 (agentxx_ffi_string_free 释放);
 * 失败时 log 含详情 */

/// 当前模型信息: {"currentModel","models":[...]}
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_get_model_info(AgentxxFFIAgent* a, AgentxxString* out, AgentxxString* log);

/// 当前会话 LLM 上下文消息: {"messages":[chat message...]}
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_get_context_messages(AgentxxFFIAgent* a, AgentxxString* out, AgentxxString* log);

/// 持久化会话列表: {"sessions":[{"sessionId","title","lastActiveMs"},...]}
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_list_sessions(AgentxxFFIAgent* a, AgentxxString* out, AgentxxString* log);

/* ==================== HIL 中断应答 ==================== */

/// 提交 EVT_INTERRUPT_REQ 的应答 (values_json 经 JSON 数组表示)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL agentxx_ffi_interrupt_respond(
    AgentxxFFIAgent*         a,
    int64_t                  interrupt_id,
    const AgentxxStringView* values_json,
    AgentxxString*           log
);

/* ==================== 日志 ==================== */

/// 取走运行期间积压的日志条目 (JSON 数组 [{"level","message"},...]; 取走后清空;
/// agentxx_ffi_string_free 释放)
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL
    agentxx_ffi_drain_logs(AgentxxFFIAgent* a, AgentxxString* out, AgentxxString* log);

/* ==================== 异步安全事件队列 ==================== */

/// 事件队列句柄 (不透明)
typedef struct AgentxxFFIEventQueue AgentxxFFIEventQueue;

/// 创建空事件队列 (任意线程; 失败返回 NULL)
AGENTXX_FFI_EXPORT AgentxxFFIEventQueue* AGENTXX_FFI_CALL agentxx_ffi_event_queue_create(void);

/// 销毁队列并释放积压事件 (唤醒全部等待者; 之后句柄失效; 勿与 pop 并发调用)
AGENTXX_FFI_EXPORT void AGENTXX_FFI_CALL agentxx_ffi_event_queue_free(AgentxxFFIEventQueue* q);

/// 内置 on_event 桥接实现 (user_data 必须为 agentxx_ffi_event_queue_create 返回值)
AGENTXX_FFI_EXPORT void AGENTXX_FFI_CALL agentxx_ffi_event_queue_on_event(
    int32_t                  type,
    const AgentxxStringView* payload_json,
    void*                    user_data
);

/**
 * 取出一条事件 (阻塞至多 timeout_ms; 0 = 非阻塞仅探测):
 * - 成功: 返回 AGENTXX_FFI_OK, *type_out 填事件种类, *json_out 填入 payload
 * (agentxx_ffi_string_free 释放)
 * - 队列为空且等待超时: 返回 AGENTXX_FFI_ERR_TIMEOUT
 * - 参数非法/队列已销毁: 返回 AGENTXX_FFI_ERR_INVALID / AGENTXX_FFI_ERR_STATE
 */
AGENTXX_FFI_EXPORT int32_t AGENTXX_FFI_CALL agentxx_ffi_event_queue_pop(
    AgentxxFFIEventQueue* q,
    int32_t*              type_out,
    AgentxxString*        json_out,
    uint32_t              timeout_ms
);

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
