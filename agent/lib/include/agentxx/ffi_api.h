/*
 * agentxx/ffi_api.h —— libagentxx 对外 FFI C API (唯一跨版本稳定接口)
 *
 * 用途: 供其他编程语言 (Python/Rust/Go/C#/Java/Node...) 经 FFI (ctypes/
 *       JNA/wasm-abi 等) 链接 libagentxx_shared 动态库, 在宿主进程内嵌入
 *       agent 会话运行。导出符号全部为纯 C (extern "C" + 无 STL/异常外泄),
 *       动态库经 version script / 导出列表严格控制导出面 (见 lib/ffi_symbols.map)。
 *
 * 架构: 本 API 是 "client 端点" 的 C 抽象 —— 内部实现一个自定义 AgentIOBase
 *       端点 (FfiClientAgentIO), 经进程内 ChannelAgentIOTransport 与
 *       SessionServerAgentIO (由 BaseAgent 驱动) 通信, 与 TUI/CLI client
 *       完全同构 (参考 agent/client/src/mode_runners.cpp setupLocalUnifiedDirect)。
 *
 * 事件模型 (异步, agent io 线程回调):
 *   - 所有事件经 AgentxxCallbacks::on_event 回调, payload 为 JSON 字符串
 *     (与服务端 wire 协议 JSON 字段一致, 见 agentxx/agent/io/wire_protocol.h)
 *   - 回调在 agent io 线程同步调用, 必须快速返回 (不得阻塞; 长时间任务请
 *     投递到宿主自己的线程池)
 *   - payload 字符串仅回调期间有效, 需保存必须自行拷贝
 *   - 回调内可调用本 API (自动投递回 io 线程, 不死锁); 但不得调用
 *     agentxx_stop/agentxx_destroy (见下)
 *
 * 错误处理约定:
 *   - 同步错误: 返回值 = 错误码 (AGENTXX_OK=0 成功, 见 AGENTXX_ERR_*);
 *     可选的 char** log 参数非 NULL 时, 函数会 (按需) 填入 NUL 结尾 UTF-8
 *     详情字符串, 宿主用后必须 agentxx_free 释放; 成功/无日志时 *log 为 NULL
 *   - 异步错误: 经 EVT_ERROR / EVT_TURN_END(hasError) 事件上报
 *
 * 生命周期与线程:
 *   - agentxx_create 在宿主任意线程调用; 成功后:
 *       agentxx_start 启动内置 agent io 线程, 异步就绪 (EVT_READY)
 *       agentxx_stop 同步停止并等待 io 线程退出 (幂等)
 *       agentxx_destroy 销毁句柄 (未 stop 时自动 stop)
 *   - stop/destroy 必须在回调线程以外调用 (agent io 线程内调用返回
 *     AGENTXX_ERR_STATE); 其余 API 任意线程可调用
 *   - 同一句柄的所有非同步 API 可并发调用 (内部投递 io 线程串行执行);
 *     同步查询类 API (get_model_info/get_context_messages/list_sessions)
 *     同一时刻同一句柄仅允许一个在途 (见各函数注释)
 *
 * 内存: 宿主分配的内存经 agentxx_free 释放 (跨 CRT 堆边界唯一通道,
 *       见 agentxx.h); 字符串参数均须 NUL 结尾 UTF-8。
 *
 * 版本策略: 修改本契约时递增 AGENTXX_FFI_API_VERSION; 宿主应在使用前以
 * agentxx_ffi_api_version() 校验。符号新增/扩展 (如新增事件值、新增字段) 为
 * 非破坏性, 不递增版本; 删除/重命名符号或修改参数语义时递增。
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

/* ==================== 错误码 ==================== */

#define AGENTXX_OK            0  ///< 成功
#define AGENTXX_ERR_INVALID   -1 ///< 参数非法 (NULL 句柄/空串/非法 JSON 等)
#define AGENTXX_ERR_STATE     -2 ///< 状态错误 (未 start/已 stop 后调用、io 线程内 stop 等)
#define AGENTXX_ERR_JSON      -3  ///< JSON 解析失败
#define AGENTXX_ERR_CONFIG    -4  ///< 配置非法 (模型缺失/字段类型错误)
#define AGENTXX_ERR_INIT      -5  ///< agent init 失败 (详见 log/EVT_ERROR)
#define AGENTXX_ERR_INTERRUPT -6  ///< 中断请求无效 (id 不存在/已应答/已过期)
#define AGENTXX_ERR_TIMEOUT   -7  ///< 同步查询等待响应超时
#define AGENTXX_ERR_OOM       -8  ///< 内存分配失败
#define AGENTXX_ERR_INTERNAL  -99 ///< 内部异常 (log 含详细信息)

/// 错误码 → 静态字符串 (线程安全, 静态存储, 勿释放)
AGENTXX_FFI_EXPORT const char* agentxx_ffi_strerror(int code);

/* ==================== 句柄 ==================== */

/// agent 会话运行时句柄 (不透明; 由 agentxx_create 创建, agentxx_destroy 销毁)
typedef struct AgentxxAgent AgentxxAgent;

/* ==================== 内存 (跨 CRT 堆边界; 参照 agentxx.h) ==================== */

AGENTXX_FFI_EXPORT void* agentxx_malloc(size_t size);   ///< 分配
AGENTXX_FFI_EXPORT void  agentxx_free(const void* ptr); ///< 释放
/// 分配并拷贝 NUL 结尾字符串 (字符串返回值的统一分配通道)
AGENTXX_FFI_EXPORT char* agentxx_strdup_n(const char* s, size_t size);

/* ==================== 版本 ==================== */

/// 当前 FFI API 版本 (== AGENTXX_FFI_API_VERSION)
AGENTXX_FFI_EXPORT int agentxx_ffi_api_version(void);
/// 库版本字符串 (静态存储, 勿释放)
AGENTXX_FFI_EXPORT const char* agentxx_ffi_library_version(void);

/* ==================== 事件回调 ==================== */

/// agent 事件种类
typedef enum AgentxxEventType {
    AGENTXX_EVT_READY = 0,     ///< 服务端就绪, 可开始发送输入: {"sessionId"}
    AGENTXX_EVT_SYNC,          ///< 全量/部分历史同步: wire sync JSON
    AGENTXX_EVT_DELTA,         ///< 流式增量: wire delta JSON (kind=text_token/...)
    AGENTXX_EVT_TURN_END,      ///< 轮次结束: wire turn_result JSON
    AGENTXX_EVT_CONTEXT_STATS, ///< 上下文统计: wire context_stats JSON
    AGENTXX_EVT_MODEL_INFO,    ///< 模型信息 (查询/切换结果): wire model_info JSON
    AGENTXX_EVT_COMPONENTS, ///< 启动组件 (MCP/Skill/Memory/插件): wire append_component_info JSON
    AGENTXX_EVT_INTERRUPT_REQ,     ///< HIL 中断询问 (权限确认/输入收集): 见注释
    AGENTXX_EVT_INTERRUPT_EXPIRED, ///< 中断已过期/已取消: {"interruptId"}
    AGENTXX_EVT_PLUGIN_DATA,       ///< 插件事件转发: wire plugin_data JSON
    AGENTXX_EVT_ERROR,             ///< 内部错误: {"code","message"}
} AgentxxEventType;

/**
 * EVT_INTERRUPT_REQ payload:
 *   {"interruptId": N, "sessionId": "...", "node": "...", "value": "...",
 *    "argJson": {"name": "...", "inputs": [
 *        {"label": "...", "depict": "...", "type": "bool|int|double|string|enum",
 *         "defaultValue": "...", "enumValues": [...]}, ...]}}
 * 宿主应 UI 展示后调用 agentxx_interrupt_respond() 应答:
 *   values_json 为 JSON 数组, 与 inputs 顺序一一对应:
 *     bool → "true"/"false"; int/double → 数字字符串; enum → 枚举值字符串;
 *     string → 文本; type 为空 → 无需输入 (应答空数组即可)
 */
typedef struct AgentxxCallbacks {
    /// 事件回调 (agent io 线程; payload_json 为 NUL 结尾 UTF-8, 仅回调期间有效)
    void (*on_event)(AgentxxEventType type, const char* payload_json, void* user_data);
    /// 宿主上下文 (原样透传给 on_event)
    void* user_data;
} AgentxxCallbacks;

/* ==================== 生命周期 ==================== */

/**
 * 创建 agent 运行时句柄 (未启动; 构造对象, 不启动线程)
 * @param config_json NULL 或 AgentConfig 覆盖 JSON (未知字段忽略):
 *   {
 *     "dataDir": "~/.agentxx",          // 空=不持久化 (默认)
 *     "enableSessionStore": false,
 *     "sessionStoreDirectory": "",     // 为空时使用 {dataDir}/sqlite/sessions/
 *     "permissionMode": "ask",          // ask|all_ask|pass|deny
 *     "permissionAllowPaths": ["..."],  // 权限白名单
 *     "permissionDenyPaths": ["..."],   // 权限黑名单
 *     "skills": ["..."], "memoryFiles": ["..."],
 *     "mcpServers": {"ns": {"url": "...", "timeoutSec": 120}},
 *     "plugins": [{"path": "...", "enabled": true, "sides": "agent|client|auto", "args": {}}],
 *     "llmMaxRetry": 5,
 *     "agentName": "Agentxx",
 *     "interruptTimeoutSec": 0,         // HIL 等待宿主应答超时, 0=不限 (默认)
 *   }
 * @param model_json 主模型 ModelConfig JSON (建议必填; 可 NULL 但模型必须
 *   isValid: baseUrl 非空 或 apiKey != "EMPTY"):
 *   {
 *     "name": "显示名(默认=modelName)", "type": "openai|anthropic|openai-responses",
 *     "baseUrl": "...", "apiKey": "...", "modelName": "(请求 model 字段)",
 *     "apiPath": "", "connectTimeoutSeconds": 16, "readChunkTimeoutSeconds": 100,
 *     "sslVerify": true|null, "maxConcurrentConnections": 5,
 *     "anthropicVersion": "2023-06-01", "modelContextMaxToken": 0,
 *     "extraHeaders": {"k": "v"}, "extraConfig": {}
 *   }
 * @param cb 事件回调 (可 NULL = 纯 headless; 内部值拷贝, 回调期间必有效)
 * @param log 非 NULL 时失败填入错误详情 (agentxx_free 释放)
 * @return 句柄; 失败返回 NULL
 */
AGENTXX_FFI_EXPORT AgentxxAgent* agentxx_create(
    const char*             config_json,
    const char*             model_json,
    const AgentxxCallbacks* cb,
    char**                  log
);

/// 异步启动 (创建 agent io 线程 + init + 会话驱动循环):
/// - 立即返回 AGENTXX_OK 表示已受理; 就绪后回调 EVT_READY
/// - 启动失败经 EVT_ERROR 上报 (可经 agentxx_drain_logs 取详情)
AGENTXX_FFI_EXPORT int agentxx_start(AgentxxAgent* a, char** log);

/// 同步停止并回收 (阻塞到 agent io 线程退出; 幂等; 不得在回调线程内调用)
AGENTXX_FFI_EXPORT int agentxx_stop(AgentxxAgent* a, char** log);

/// 销毁句柄 (未 stop 时自动 stop; 之后句柄失效; 不得在回调线程内调用)
AGENTXX_FFI_EXPORT int agentxx_destroy(AgentxxAgent* a, char** log);

/* ==================== 会话交互 (异步, 投递 io 线程执行) ==================== */

/// 发送用户输入 (EVT_READY 前发送会缓存, 就绪后按序消费)
AGENTXX_FFI_EXPORT int agentxx_send_input(AgentxxAgent* a, const char* text, char** log);

/// 请求取消当前轮次
AGENTXX_FFI_EXPORT int agentxx_cancel(AgentxxAgent* a, char** log);

/// 切换当前会话所用模型 (结果经 EVT_MODEL_INFO 通知)
AGENTXX_FFI_EXPORT int agentxx_select_model(AgentxxAgent* a, const char* model_name, char** log);

/// 记住权限选择 (服务端注册路径规则, 后续同路径不再询问):
/// op: 0=读取 1=写入; allow: 1=允许 0=拒绝
AGENTXX_FFI_EXPORT int
    agentxx_set_permission(AgentxxAgent* a, const char* path, int allow, int op, char** log);

/// 切换当前连接会话 (sessionId 为空 = 关闭持久化时非法):
/// 重新绑定会话并回推 Sync/ModelInfo/ContextStats (经对应事件通知)
AGENTXX_FFI_EXPORT int agentxx_switch_session(AgentxxAgent* a, const char* sessionId, char** log);

/* ==================== 同步查询 (阻塞等待服务端响应, 最长 10s) ====================
 * 返回值: JSON 字符串 (agentxx_free 释放); 失败返回 NULL (log 含详情)
 * 注意: 同一句柄同一时刻仅允许一个在途 (服务端应答为逐条协议) */

/// 当前模型信息: {"currentModel","models":[...]}
AGENTXX_FFI_EXPORT char* agentxx_get_model_info(AgentxxAgent* a, char** log);

/// 当前会话 LLM 上下文消息: {"messages":[chat message...]}
AGENTXX_FFI_EXPORT char* agentxx_get_context_messages(AgentxxAgent* a, char** log);

/// 持久化会话列表: {"sessions":[{"sessionId","title","lastActiveMs"},...]}
AGENTXX_FFI_EXPORT char* agentxx_list_sessions(AgentxxAgent* a, char** log);

/* ==================== HIL 中断应答 ==================== */

/// 提交 EVT_INTERRUPT_REQ 的应答 (values_json 见该事件注释; 任意线程可调用)
AGENTXX_FFI_EXPORT int agentxx_interrupt_respond(
    AgentxxAgent* a,
    int64_t       interrupt_id,
    const char*   values_json,
    char**        log
);

/* ==================== 日志 ==================== */

/// 取走运行期间积压的日志条目 (JSON 数组 [{"level","message"},...]; 取走后清空;
/// agentxx_free 释放) —— 供宿主在异常后转储排障:
/// {"level": "info|warn|error", "message": "..."}
AGENTXX_FFI_EXPORT char* agentxx_drain_logs(AgentxxAgent* a, char** log);

#ifdef __cplusplus
}
#endif

#endif /* AGENTXX_FFI_API_H */