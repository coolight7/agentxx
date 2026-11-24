# 实施计划：FFI 接口异步化（调用提交 + 事件回执 + 可取消）

## 1. 背景与目标

插件侧已经收敛成"提交 + 回调/协程"的一种形态（见 [coroutine-unify](../coroutine-unify/index.md)），
而 FFI 导出层还留着一条**同步阻塞**的老路：`agentxx_ffi_get_model_info` 一类查询在调用线程
上等 promise/future（最长 10s），`agentxx_ffi_stop` 在调用线程上 join 两条 io 线程（最长
45s），事件必须由嵌入方在回调里**同步拷贝**（否则要经事件队列 + 轮询）。

后果在 Dart 侧最明显：`AgentClient.getModelInfo()` 是普通函数，调一次就把 Dart 主 isolate
卡住（最长 10s），期间事件轮询停摆；事件侧还要一条 4ms 轮询循环顶着（[agent_client.dart]
的 `_pollLoop`）。这与"SDK 全部 async/await"的目标直接冲突。

本任务一次性把 FFI 导出层改成异步：

1. **一次调用 = 提交 + 回执**：所有入口都变成"受理即返回 reqId，结果经 `EVT_RESULT` 送达"；
   调用线程（含事件回调线程）上不再出现任何跨线程等待。
2. **每个调用恰好一个回执**：**没有"只发不等"的暗坑** —— 有服务端副作用的 op 一律以服务端
   回执（typed 应答或通用 `ack`）完成，纯本地 op 完成后立即回执。客户端想等就 `await`，
   想让前一步确实生效再发下一步就"先等回执再提交"（顺序保证）。
3. **可取消**：`agentxx_ffi_call_cancel(reqId)` 让一次**仍在等待**的调用立即以
   `code=-9 canceled` 收尾；底层 SDK 用它给"用户关掉弹窗/放弃查询"提供取消能力
   （Dart `Future` 本身不支持取消，但可以由显式调用触发，再由原调用的 Future 抛取消异常）。
4. **事件自带所有权**：`on_event` 的 payload 改为"归回调所有"的 `AgentxxString`，回调
   返回前释放即可 —— 绑定可以放心用 `NativeCallable.listener` / 线程安全队列，C 侧不再需要
   事件队列桥接与轮询。
5. **导出面收敛**：35 个符号 → 15 个；新增能力/操作只需加 op 名，不再加符号。
6. **生命周期异步**：`stop` 受理即返回，收尾完成时发 `EVT_STOPPED`（最后一个事件）；
   `destroy` 只在已停止后释放句柄，不再做阻塞收尾。
7. **不保留旧版本兼容**：`AGENTXX_FFI_API_VERSION` 3 → 4，旧语义直接删除（最低支持版本
   已抬过、尚未发布，仓库外没有消费者）。

### 为什么不能只在 SDK 层绕

- 把同步查询丢进 isolate（Dart `Isolate.run`）：每个句柄多一条 isolate，参数与结果要跨
  isolate 拷贝；回调仍在 native 线程，事件依旧要队列 + 轮询；`stop` 依旧阻塞收尾。
- "payload 仅在回调期间有效"是 C 层契约：不管 SDK 怎么写，都必须在回调里同步拷贝，
  `NativeCallable.listener` 一类线程安全投递永远用不上。
- "回调内不能发起查询（自己等自己）"也只能在 C 层解掉。
- "取消"必须由 C 层把等待点撤掉（Dart 侧只能把 `Completer` 置错，无法停止 native 等待）。

所以改动必须落在导出层，SDK 只是跟着变简单。

## 2. 现状调查（文件 / 函数一级）

### 2.1 对外 API 面（35 个导出符号）

`agent/lib/include/agentxx/ffi_api.h`（508 行）+ `agent/lib/src/ffi/ffi_api.cpp`（708 行）：

| 分组 | 符号 | 现状 |
|---|---|---|
| 内存 | `agentxx_ffi_malloc` / `_free` / `_string_free` / `_strdup_n` | 同步，本地 |
| 版本 | `agentxx_ffi_api_version` / `_library_version` / `_strerror` | 同步，本地 |
| 能力 | `agentxx_ffi_get_capabilities` | 同步，本地组装 JSON |
| 生命周期 | `_create` / `_start` / `_stop` / `_destroy` | create 同步构造；start 异步（`EVT_READY`）；**stop/destroy 阻塞收尾** |
| 会话交互 | `_send_input` / `_cancel` / `_select_model` / `_switch_session` / `_set_language` / `_get_language` | 前五个"校验 + 投递"即返回；`get_language` 本地 |
| 模型管理 | `_add_model` / `_remove_model` / `_list_models` | **阻塞等回执（最长 10s）** |
| Wire 原样转发 | `_send_wire` | 投递即返回，应答经 `EVT_WIRE` |
| 嵌入方工具 | `_tool_register` / `_tool_unregister` / `_tool_respond` | 投递即返回，**注册无回执**（失败只落日志） |
| 增量合批 | `_set_delta_batch` | 投递即返回 |
| 同步查询 | `_get_model_info` / `_get_context_messages` / `_list_sessions` | **阻塞等应答（最长 10s）** |
| HIL | `_interrupt_respond` | 校验 + 投递即返回 |
| 日志 | `_drain_logs` | 同步，本地环形缓冲 |
| 事件队列 | `_event_queue_create` / `_free` / `_on_event` / `_pop` | 队列桥接 + **阻塞 pop（timeout）** |
| 取消 | —— | **没有**任何"放弃一次等待"的通道，只能等 10s 超时 |

### 2.2 阻塞点清单

| 入口 | 位置 | 阻塞原因 | 最长 | 影响 |
|---|---|---|---|---|
| `getModelInfo` / `listModels` | `ffi_runtime.cpp:667 syncQuery` → `712 getModelInfo` | `promise/future::wait_for(10s)`，应答在 client io 线程完成 | 10s | 调用线程（Dart 主 isolate）停摆；期间的 delta/turn_end 全部积压 |
| `getContextMessages` | 同上 | 同上 | 10s | 同上 |
| `listSessions` | 同上 | 同上（服务端还要扫目录 + SQLite） | 10s | 同上 |
| `addModel` / `removeModel` | `ffi_runtime.cpp:760/812` 走 `syncQuery` | 同上 | 10s | 同上 |
| `stop` / `destroy` | `ffi_runtime.cpp:426 stopInternal` | `runtime_->stop()` + 轮询 `running()`（20s）+ `shutdownAsync` 等 25s + join 两条线程 | 45s | 退出路径卡住界面 |
| `event_queue_pop` | `event_queue.cpp` | `condition_variable::wait_for(timeout_ms)` | 调用方给 | 只能轮询（Dart 4ms 一次），空转 + 事件延迟 |
| `create` | `ffi_runtime.cpp:create` | 解析配置 + `CodeAgent` 构造（含 io_context） | 毫秒级 | 可接受，保持同步 |
| `drain_logs` / `get_capabilities` / 语言 | 本地 | 本地内存操作 | 微秒级 | 保持同步 |

**三个已存在的隐患**（这次一并解决）：

1. **在事件回调里调同步查询 = 自我死锁**：`on_event` 在 client io 线程执行，
   `syncQuery` 把发送动作 `asio::post` 到同一条线程后等 future —— 发送永远排不上，
   只能等满 10s 返回 `AGENTXX_FFI_ERR_TIMEOUT`，期间所有事件停摆。头文件目前只写了
   "回调内可调用本 API（自动投递回 io 线程，不死锁）"，对同步查询并不成立。
2. **`stop`/`destroy` 与回调的生命周期靠口头约定**：`stop` 会 join 回调线程，头文件只写了
   "不得在回调线程内调用"；没有"收尾完成"的显式信号，嵌入方只能靠 `stop` 返回推断
   "不会再有回调"，而 `destroy` 又可能被提前调用。
3. **命令类操作没有回执**：`select_model` / `switch_session` / `compact_context` /
   `interrupt_respond` / `host_tool_register` 等只"投递出去"，服务端是否受理、是否失败
   嵌入方完全不知道（`select_model` 选错名字被静默忽略；`tool_register` 冲突只打日志），
   因此**无法表达"前一步完成后再做下一步"**。

### 2.3 事件通路与线程模型

- 每个 `FfiAgentRuntime` 两条 io 线程：Server-IO（`CodeAgent` / `SessionServerAgentIO` / 工具 /
  LLM）与 Client-IO（`FfiClientAgentIO`：wire 编解码、C 回调、中断等待、合批）；另有 1 条日志
  sink 线程（`FfiLogSink` 派生自 `ThreadedLogSink`，见 §9.1）。
- 事件全部在 client io 线程发出：`ffi_client_io.cpp:232 emitEvent`，
  payload 是 `const AgentxxStringView*`（**借用，回调返回即失效**）。
- 应答路由靠"按种类 FIFO"：`FfiClientAgentIO::SyncKind`（5 种）+ `onSyncReply` +
  `FfiAgentRuntime::onSyncReplyOnClientThread` + `syncWaits_`（`map<SyncKind, deque<SyncWait>>`）。
  因此头文件里有一条硬约束："同一句柄同一时刻仅允许一个未完成的同步查询"。
- 请求与应答在 wire 上**没有相关性字段**（见 2.4），FIFO 是唯一可行的匹配方式。

### 2.4 请求 / 应答相关性（wire 侧）

| 请求 | 应答 | 现在有 reqId？ |
|---|---|---|
| `WireGetModel` | `WireModelInfo` | 否 |
| `WireGetContext` | `WireContextMessages` | 否 |
| `WireListSessions` | `WireSessionList` | 否 |
| `WireAddModel` | `WireAddModelResult` | 否 |
| `WireRemoveModel` | `WireRemoveModelResult` | 否 |
| `WireRenameSession` | `WireRenameSessionResult` | 否 |
| `WireGetViewMessages` | `WireViewMessagesPage` | 否 |
| `WireGetAppendComponentInfo` | `WireAppendComponentInfo` | 否 |
| `WireGetPermissionState` / `WireSetFullAuth` | `WirePermissionState` | 否 |
| `WireListDir` | `WireListDirResult` | **是**（`reqId`，TUI 附件选择在用） |
| `WireUserInput` | `WireInputAck` | 是（`requestId`，仅 > 0 才回执） |
| `WireSelectModel` / `WireSwitchSession` / `WireCompactContext` / `WireCancel` / `WireClearMessageQueue` / `WireRemoveQueueItem` / `WireInterruptAndRunNext` / `WireInterruptResponse` / `WireHostToolRegister` / `Unregister` / `WireHostToolResult` / `WirePluginDataUp` | **无** | — |

结论：

- 要做到"任意多个并发调用 + 各自回执"，必须先给这些请求/应答补 `reqId` 回显；
- 要做到"无结果操作也能确认完成"，必须给"现在没有应答"的那批命令加一条**通用回执**
  （`WireAck`），否则客户端只能靠"再查一次"猜是否生效。

### 2.5 消费方清单

| 位置 | 用途 | 受影响 |
|---|---|---|
| `agent/test/test_ffi_c_api.cpp`（1380 行） | 唯一 FFI 测试模块（静态链接调用导出符号 + mock LLM） | 同步查询/事件队列相关用例要重写 |
| `agent/benchmark/bench_resource.cpp:1607 benchResourceFfi` | 动态 `dlopen` + 按名解析符号的内存基准 | 符号与调用方式要改 |
| `agent/ffi/dart/` | ffigen 生成的绑定包（`ffigen.yaml` + `lib/agentxx_ffi_bindings.dart` 949 行） | 重新生成 |
| `agent/example/ffi/dart/` | Dart CLI 示例（`AgentClient` 封装 + REPL + smoke_check） | 全面异步化 |
| `docs/zh-cn/design/ffi.md`（497 行）/ `docs/en/design/ffi.md`（509 行） | 契约文档 | 重写 |
| `agent/lib/ffi_symbols.map` | ELF/MSVC 导出白名单 | 收敛到 15 个 |

### 2.6 顺带发现的问题（本次一并修）

1. `ffi_runtime.cpp:742 getCapabilities()` 的 `libraryVersion` 写死 `"0.4.0"`，且能力清单
   少了 `AGENTXX_FFI_CAP_VIEW_REPLICA`（`ffi_api.cpp` 的 NULL 句柄分支反而有）
   —— 新形态改为 `ops` + `features`（§3.2），两处不再各自拼一份。
2. `ffi_api.h:208` 的注释还写着 `"apiVersion":2`。
3. `agent/ffi/dart/ffigen.yaml` 里列了不存在的符号 `agentxx_ffi_set_permission`。
4. `FfiClientAgentIO::notifyServerReady`（`ffi_client_io.cpp:25`）是死代码：`EVT_READY`
   实际由基类 `onServerReady` 覆写发出。
5. `agentxx_ffi_tool_register` 无回执：注册失败（同名冲突、schema 非法）只有服务端日志，
   嵌入方以为注册成功，模型永远看不到这些工具。
6. `WireSelectModel` 选到不存在的模型名被服务端**静默忽略**（`BaseAgent::selectModel`
   只在 registry 里存在时才写），客户端看到的状态栏与实际模型不一致也无从发现。
7. `ffi_api.h:74` 的 API 3 版本说明写着"直接调 `agentxx_ffi_get_view_messages` 拉取历史"，
   但这个符号并不存在（v3 已删）：嵌入方要么自己按 `EVT_DELTA` 的消息级 op 拼副本，要么用
   `get_context_messages`（那给的是 LLM 上下文，不是展示历史）。新形态用 `context.view_page`
   （§3.3）补上这个缺口。
8. 事件枚举里 `AGENTXX_FFI_EVT_HOST_TOOL_Canceled` 的大小写与同族不一致（其余全大写）；
   v4 顺带规范为 `AGENTXX_FFI_EVT_HOST_TOOL_CANCELED`。
9. `docs/zh-cn/design/ffi.md` 自己不一致：§1 写"仅 26 个顶层 C 符号"，§4.3 写"35 个 FFI
   C API"（实际 35，`ffi_symbols.map` 也是 35）；第 6 步重写时统一成 15。

## 3. 目标形态

### 3.1 原则

1. **受理即校验，执行异步**：参数错误（JSON 非法、op 未知、字段类型错）同步返回错误码 +
   `log`；执行结果（超时、被拒、服务端失败）经 `EVT_RESULT` 送达。同步失败时**不写
   `out_req_id`、也不会有回执**（调用方不要为它登记 Future）。
2. **一个入口**：`agentxx_ffi_call(json)`；op 名是稳定字符串，能力清单可枚举。
3. **每个 reqId 恰好一个 `EVT_RESULT`**：包括被取消、超时、收尾中断的情况；不会出现
   "提交后永远不知道结果"的 op。
4. **有服务端副作用的 op 以服务端回执完成**：客户端要顺序保证时"先 await A，再提交 B"；
   FFI 不会对这一步做额外承诺（见 §3.5 的顺序说明）。
5. **取消只停止等待，不撤回已提交的操作**：`call_cancel` 不保证服务端不做那件事（见 §3.6）。
6. **事件不并发**：同一时刻只有一个线程在执行事件回调 —— 运行期固定是 client io 线程，
   `stop` 收尾的 `EVT_STOPPED` 在收尾线程（`Created` 状态下则由 `stop` 的调用线程直接发），
   那时其它线程已 join 或尚未启动。`EVT_STOPPED` 是最后一个事件（见 §3.7）。
7. **导出面与绑定解耦**：新增功能不动 C ABI。
8. **不保留旧形态**：同步查询、事件队列、`send_wire`、按种类 FIFO 全部删除。

### 3.2 头文件骨架（`agentxx/ffi_api.h` v4）

```c
#define AGENTXX_FFI_API_VERSION 4

/* 错误码：新增 CANCELED / REJECTED */
#define AGENTXX_FFI_OK             0
#define AGENTXX_FFI_ERR_INVALID   -1   /* 参数/内容非法 */
#define AGENTXX_FFI_ERR_STATE     -2   /* 状态不允许（未 start、已 stop、运行中 destroy） */
#define AGENTXX_FFI_ERR_JSON      -3
#define AGENTXX_FFI_ERR_CONFIG    -4
#define AGENTXX_FFI_ERR_INIT      -5
#define AGENTXX_FFI_ERR_INTERRUPT -6
#define AGENTXX_FFI_ERR_TIMEOUT   -7   /* 本次调用超时（也可出现在 EVT_RESULT 里） */
#define AGENTXX_FFI_ERR_OOM       -8
#define AGENTXX_FFI_ERR_CANCELED  -9   /* 调用被 call_cancel 取消 / 收尾时未完成 */
#define AGENTXX_FFI_ERR_REJECTED -10   /* 服务端拒绝（reason 给结构化原因） */
#define AGENTXX_FFI_ERR_INTERNAL -99

typedef enum AgentxxFFIEventType {
    AGENTXX_FFI_EVT_READY = 0,              /* {sessionId} */
    AGENTXX_FFI_EVT_STOPPED,                /* {} 最后一个事件 */
    AGENTXX_FFI_EVT_RESULT,                 /* {reqId, op, ok, result?, error?} */
    AGENTXX_FFI_EVT_ERROR,                  /* {code, name, message} 与调用无关的异步错误 */
    AGENTXX_FFI_EVT_SYNC,                   /* wire sync */
    AGENTXX_FFI_EVT_DELTA,                  /* wire delta（消息级 op） */
    AGENTXX_FFI_EVT_DELTA_TEXT_BATCH,       /* {msgId, kind, text} */
    AGENTXX_FFI_EVT_TURN_END,
    AGENTXX_FFI_EVT_CONTEXT_STATS,
    AGENTXX_FFI_EVT_MODEL_INFO,             /* 模型状态广播（select_model/切换会话后） */
    AGENTXX_FFI_EVT_COMPONENTS,
    AGENTXX_FFI_EVT_MESSAGE_QUEUE,          /* {sessionId, items[], state} */
    AGENTXX_FFI_EVT_PERMISSION_STATE,       /* {fullAuth} */
    AGENTXX_FFI_EVT_INTERRUPT_REQ,          /* {interruptId, sessionId, node, value, argJson} */
    AGENTXX_FFI_EVT_INTERRUPT_EXPIRED,
    AGENTXX_FFI_EVT_HOST_TOOL_CALL,         /* {callId, name, argsJson, sessionId, timeoutSec} */
    AGENTXX_FFI_EVT_HOST_TOOL_CANCELED,     /* {callId, reason} */
    AGENTXX_FFI_EVT_PLUGIN_DATA,            /* {topic, data} */
} AgentxxFFIEventType;

typedef struct AgentxxFFICallbacks {
    uint32_t struct_size;                   /* = sizeof(AgentxxFFICallbacks) */
    uint32_t reserved;                      /* 必须 0 */
    /* 事件回调。payload 所有权交给回调：返回前必须 agentxx_ffi_string_free(payload)。 */
    void (AGENTXX_FFI_CALL* on_event)(int32_t type, AgentxxString* payload, void* user_data);
    void* user_data;
} AgentxxFFICallbacks;

/* 生命周期 */
AgentxxFFIAgent* agentxx_ffi_create(const AgentxxStringView* config_json,
                                    const AgentxxStringView* model_json,
                                    const AgentxxFFICallbacks* cb,
                                    AgentxxString* log);
int32_t agentxx_ffi_start   (AgentxxFFIAgent*, AgentxxString* log);   /* → EVT_READY */
int32_t agentxx_ffi_stop    (AgentxxFFIAgent*, AgentxxString* log);   /* → EVT_STOPPED */
int32_t agentxx_ffi_destroy (AgentxxFFIAgent*, AgentxxString* log);   /* 仅 Created/Stopped */

/* 调用（唯一异步入口） */
int32_t agentxx_ffi_call(AgentxxFFIAgent* a,
                         const AgentxxStringView* request_json,
                         uint64_t* out_req_id,
                         AgentxxString* log);

/* 取消一次仍在等待的调用（不撤回已提交的服务端操作） */
int32_t agentxx_ffi_call_cancel(AgentxxFFIAgent* a, uint64_t req_id, AgentxxString* log);

/* 本地同步（不跨线程等待） */
int32_t agentxx_ffi_get_capabilities(AgentxxFFIAgent*, AgentxxString* out, AgentxxString* log);
int32_t agentxx_ffi_drain_logs     (AgentxxFFIAgent*, AgentxxString* out, AgentxxString* log);
/* 内存 / 版本 / strerror / library_version 保持原样 */
```

事件名尽量沿用 v3 的现有拼写（`INTERRUPT_REQ` 不改名），改动只有三类：删 `EVT_WIRE`、
新增 `EVT_STOPPED` / `EVT_RESULT` / `EVT_MESSAGE_QUEUE` / `EVT_PERMISSION_STATE`、把
`EVT_HOST_TOOL_Canceled` 规范为 `EVT_HOST_TOOL_CANCELED`。枚举值顺序会随新增项重排，但
v4 本来就不兼容 v3，绑定重新生成即可。`AgentxxFFICallbacks` 新增 `struct_size`（须填
`sizeof`）与保留字段 `reserved`；`create` 仍在调用线程同步执行（毫秒级），其余生命周期入口
都异步（唯一的同步回调是 `Created` 状态下 `stop` 发出的 `EVT_STOPPED`，见 §3.7）。

请求 JSON：

```json
{ "op": "model.list", "args": { }, "timeoutMs": 10000 }
```

回执事件：

```json
{ "reqId": 7, "op": "model.list", "ok": true,
  "result": { "currentModel": "m1", "models": ["m1"], "capabilities": [] } }

{ "reqId": 8, "op": "model.add", "ok": false,
  "error": { "code": -4, "name": "bad_config", "message": "模型名重复: m1" } }

{ "reqId": 9, "op": "session.list", "ok": false,
  "error": { "code": -9, "name": "canceled", "message": "canceled by caller" } }
```

`error.name` 是稳定字符串（`invalid_argument` / `invalid_state` / `bad_json` / `bad_config` /
`init_failed` / `interrupt_invalid` / `timeout` / `out_of_memory` / `canceled` / `rejected` /
`internal`；服务端 ack 的失败名来自 `WireAck.reason`，见 §3.4），SDK 按名字判定，不看数字。

**result 的形态**：`resp:*` 类 op 的 `result` = 该 wire 应答 JSON **去掉 `type` 与 `reqId`**
（其余字段原样，包括可选的 `totalCount` / `hasMore`）；`ack` 类 op 的 `result` = `WireAck.data`
（没有数据时为 `{}`）；`本地` 类 op 的 `result` 按 op 表给出。

能力清单（`get_capabilities`）随之为：

```json
{ "apiVersion": 4, "libraryVersion": "<kVersion>",
  "ops": ["input.send", "model.list", "..."],
  "features": ["call_api", "call_cancel", "event_owned_payload", "async_stop", "op_ack"] }
```

旧的 `AGENTXX_FFI_CAP_*` 常量删除（`VIEW_REPLICA` 的语义已由 `EVT_RESULT` + `EVT_DELTA`
固定，不再需要能力位）；SDK 用 `ops` / `features` 做特性探测。

### 3.3 op 表（v1 全集）

"完成"列：`ack` = 服务端通用回执 `WireAck`；`resp:<类型>` = 对应 wire 应答；`本地` = 不进 wire。
"默认超时"是该 op 未给 `timeoutMs` 时的取值（`0` = 不限）。

| op | args | 完成 | 默认超时 | result / 说明 |
|---|---|---|---|---|
| `input.send` | `text` / `attachments` / `model` / `delivery` | `input_ack` | 10s | `{status, delivery, itemId?}`；`status=rejected` → `ok:false, code=-10`, `error.reason=<InputRejectReason>`, `error.message=<detail>` |
| `input.cancel` | – | ack | 10s | 回执表示服务端已处理该取消请求（`WireCancel` 只置取消标记） |
| `input.queue.clear` | – | ack | 10s | – |
| `input.queue.remove` | `itemId` | ack | 10s | 条目不存在 → `ok:false, reason:"item_not_found"`（同时保留给 TUI 的 `WireError`） |
| `input.queue.run_next` | – | ack | 10s | – |
| `model.list` | – | resp:`WireModelInfo` | 10s | `{currentModel, models[], capabilities[]}` |
| `model.select` | `name`（空 = 沿用当前） | ack | 10s | data `{model}`：生效后的模型名（空名字时回当前模型名）；名字不在注册表 → `ok:false, reason:"model_not_found"` |
| `model.add` | 模型配置对象（字段同 `create` 的 `model_json`） | resp:`WireAddModelResult` | 30s | `ok:false` → `code=-4`（要写 yaml 落盘） |
| `model.remove` | `name` | resp:`WireRemoveModelResult` | 30s | 删除当前模型被拒 |
| `session.list` | `beforeMs?` / `beforeId?` / `limit?`（默认 50）/ `keyword?` | resp:`WireSessionList` | 10s | `{sessions[], totalCount, hasMore}` |
| `session.switch` | `sessionId` | ack | 60s | 在切换**提交后**回执（data `{sessionId}`），历史 `EVT_SYNC` / `EVT_MODEL_INFO` / `EVT_CONTEXT_STATS` 已先于它发出 |
| `session.rename` | `title` | resp:`WireRenameSessionResult` | 10s | 空标题被拒 |
| `session.compact` | – | ack | 0（不限） | 压缩**完成后**回执（data `{ok}`），见 §3.4 |
| `context.get` | `tail?`（只取最近 N 条） | resp:`WireContextMessages` | 10s | `{messages[]}`；不给 `tail` 为整份上下文 |
| `context.view_page` | `beforeIndex?` / `count?` | resp:`WireViewMessagesPage` | 10s | `{startIndex, totalCount, messages[]}` |
| `components.get` | – | resp:`WireAppendComponentInfo` | 10s | `{notifications[]}`；启动时仍会自动拉一次（→ `EVT_COMPONENTS`） |
| `permission.get_state` | – | resp:`WirePermissionState` | 10s | `{fullAuth}` |
| `permission.set_full_auth` | `fullAuth` | resp:`WirePermissionState` | 10s | 回执里是**生效后**的状态；权限中间件缺失 → `ok:false, reason:"no_permission_middleware"` |
| `fs.list_dir` | `path?` / `allowedExtensions?` | resp:`WireListDirResult` | 30s | 跨设备附件选择用 |
| `interrupt.respond` | `interruptId` / `values` | ack | 10s | 中断不存在/已应答 → `ok:false, reason:"interrupt_not_found"`（`code=-6`） |
| `hosttool.register` | `tools[]`（同 `tool_register` 的条目） | ack | 10s | data `{registered[], failed[{name,error}]}` |
| `hosttool.unregister` | `names[]` | ack | 10s | data `{removed[]}` |
| `hosttool.respond` | `callId` / `ok` / `result` 或 `error` | ack | 10s | callId 未知 → `ok:false, reason:"call_not_found"` |
| `plugin.data_up` | `topic` / `data` | ack | 10s | 服务端已发布到 agent 总线 |
| `delta.set_batch` | `maxDelayMs` | 本地 | – | `{maxDelayMs}`；<=0 关闭合批 |
| `language.get` | – | 本地 | – | `{language}` |
| `language.set` | `language` | 本地 | – | `{language}`（规范化后）；投递到 server io 设会话语言，回执 = 已受理（没有服务端失败分支） |

两点补充：

- `resp:*` / `ack` 类 op 的回执**只发一条 `EVT_RESULT`**，不再另外发 `EVT_MODEL_INFO` 之类的
  广播事件；只有 `reqId == 0` 的下行应答（TUI 等其它客户端的请求、或服务端主动广播）才走
  广播事件。
- `input.send` 的 wire `requestId` 直接用自己的 reqId（D9），因此 ack 不需要第二张映射表。
- 会话作用域由 FFI 自动填：args 里不写 `sessionId`，一律用当前会话（`session.switch` 例外，
  它给的是目标会话）。

### 3.4 通用回执 `WireAck`（新增 wire 消息）

```cpp
/// 命令类请求的通用回执 (Client -> Server 的请求带 reqId 时由服务端发回)
struct WireAck {
    uint64_t           reqId = 0;
    bool               ok    = false;
    std::string        reason;   ///< 机器可读的失败原因 (model_not_found / interrupt_not_found / ...)
    std::string        error;    ///< 给人看的失败原因
    utilxx_base::Json data;      ///< 可选结果 (默认 null, 按 op 约定)
};
```

`MsgType` 增加 `ack`；服务端统一用一个小助手回：

```cpp
/// 请求带 reqId 时回一条通用回执 (reqId == 0 直接跳过: TUI/老客户端行为不变)
void sendAck(const std::shared_ptr<AgentIOTransportBase>& sender,
             uint64_t reqId, bool ok, std::string_view reason = {},
             std::string error = {}, utilxx_base::Json data = {});
```

FFI 侧把 `WireAck{reqId}` 映射成 `EVT_RESULT`：`ok ? result = data : {code = op 表给的失败码
（默认 -10）, name = reason 或 "rejected", message = error}`。

**未匹配的应答**：`reqId` 非 0 但在 `CallRegistry` 里找不到（已超时 / 已取消 / 已收尾）→ 丢弃并记
debug 日志；`reqId == 0` 的 typed 应答或 `WireAck`（别的客户端的请求、老客户端）→ FFI 不发
事件，只记 debug 日志 —— v3 那种"原样转 `EVT_WIRE`"的做法随 `EVT_WIRE` 一起删除。

**哪些请求补 `reqId`、怎么回执**：

| 请求 | 现在 | 改后 |
|---|---|---|
| `WireGetModel` / `GetContext` / `ListSessions` / `AddModel` / `RemoveModel` / `RenameSession` / `GetViewMessages` / `GetAppendComponentInfo` | 无 reqId | 请求 + 应答都带 `reqId`（`0` = 不写字段） |
| `WireGetPermissionState` / `WireSetFullAuth` | 无 | 请求 + `WirePermissionState` 带 `reqId`；`set_full_auth` 对发起方回带 reqId 的状态，同时照旧广播状态变化（见下） |
| `WireSelectModel` | 无应答 | 加 `reqId` → `WireAck{data:{model}}` |
| `WireSwitchSession` | 无应答 | 加 `reqId` → 切换提交后 `WireAck{data:{sessionId}}` |
| `WireCompactContext` | 无应答 | 加 `reqId` → 压缩完成后 `WireAck{data:{ok}}` |
| `WireCancel` / `ClearMessageQueue` / `InterruptAndRunNext` | 无应答 | 加 `reqId` → `WireAck` |
| `WireRemoveQueueItem` | 失败时 `WireError` | 加 `reqId` → `WireAck`（`WireError` 保留给 TUI） |
| `WireInterruptResponse` | 无应答 | 加 `reqId` → `WireAck`（未知 id → `reason:"interrupt_not_found"`） |
| `WireHostToolRegister` / `Unregister` / `HostToolResult` | 无应答 | 加 `reqId` → `WireAck`（`register` 带 `{registered,failed}`） |
| `WirePluginDataUp` | 无应答 | 加 `reqId` → `WireAck` |
| `WireUserInput` | `requestId` | 保持（`input_ack` 已够用） |
| `WireListDir` | `reqId` | 保持 |

**`session.compact` 的完成信号**（唯一需要动中间件的一处）：`SummarizationMiddlewareHandle::
compactSessionContext`（`agent/lib/src/middlewares/summarization.cpp:1183`）返回 `bool` 但
没有对外通知。新增一条总线事件 `Topic::SummarizationCompactDone`，载荷
`{sessionId, ok}`；`SessionServerAgentIO` 订阅它，把该会话上排队的 compact 请求按 FIFO
依次回 ack。中间件里**每条提前退出路径**（没有 agentContext / 没有会话 / 上下文为空）
也要发布 `ok=false`，否则该请求会等到超时。若想缩范围，可退回"发布即 ack（accepted）"，
文档里写明不保证压缩完成。

**`set_full_auth` 的回执与广播**：发起方经 `sendToClient(sender, WirePermissionState{state, reqId})`
拿回执；状态确实变化时再 `sendToPeer(WirePermissionState{state})` 广播给其他客户端。
`sendToPeer` 会发给所有客户端（含发起方），因此要给 `SessionServerAgentIO` 加一个
`broadcastToClientsExcept(sender, msg)` 小助手，避免发起方收两条（收到的幂等，但会让 FFI
多一条 `EVT_PERMISSION_STATE`）。

**会话不匹配**：`acceptSessionScope`（`session_server_agent_io.cpp:2668`）现在发 `WireError`；
增加一个可选 `reqId` 参数，`reqId != 0` 时改发 `WireAck{ok:false, reason:"session_mismatch"}`，
让调用方拿到确定的失败而不是等超时（TUI 传 0，行为不变）。

### 3.5 顺序与"等待无结果操作"

- **保证的顺序**：收到 A 的回执之后才提交 B（即 `await A; await B` 的写法），则 B 到达服务端时
  A 已经处理完成。这是表达"前置操作已完成"的唯一可靠写法。
- **不保证的顺序**：同时提交 A、B（不等 A 的回执）时，若 A 的实现是异步完成
  （`session.list` / `session.switch` / `session.compact` / `fs.list_dir` 走 `co_spawn`），
  B 可能在 A 完成之前就被处理。文档明写这一条，SDK 需要顺序就"先 await"。
- `session.switch` 的回执在历史 `Sync` / 模型信息 / 上下文统计**发出之后**才发，因此
  `await switch()` 返回时，切换造成的状态事件已经排在它前面（同一条连接按序投递）。
- **合批的增量不在这个顺序里**：`EVT_DELTA_TEXT_BATCH` 由 FFI 本地按 `maxDelayMs` 定时 flush，
  可能与后续的 `EVT_RESULT` 交错到达。需要"回执之前的增量都已到齐"这种强顺序时，关掉合批
  （`delta.set_batch(0)`）或由绑定按 `msgId` 缓存消息级 op。

### 3.6 取消语义（`agentxx_ffi_call_cancel`）

- **作用范围**：只撤掉"这次等待"。已投递到服务端的操作照常执行（`input.send` 的文本不会因为
  取消而撤回；要撤回输入请用 `input.cancel` 取消当前轮次）。
- **实现**：`CallRegistry` 的每条待完成记录带 `std::atomic<bool> settled`；
  `settle()` 先 CAS，赢的一方负责 `erase` + 取消超时定时器 + 投递 `EVT_RESULT`，
  因此"服务端应答"和"本地取消"赛跑时恰好一个回执。
- **回执**：`EVT_RESULT{ok:false, error:{code:-9, name:"canceled", message:"canceled by caller"}}`。
- **幂等**：`reqId` 不存在（已完成/从未发出）返回 `AGENTXX_FFI_OK` 并在 `log` 里写明
  "already completed"，避免 SDK 在竞态里收到伪错误；`reqId == 0` 返回 `ERR_INVALID`。
- **迟到应答**：`reqId` 已结算的应答直接丢弃（debug 日志），不产生第二条回执。
- **未发送的调用**：已经 `asio::post` 但尚未组包发送的调用，取消**不**阻止发送
  （保持"操作一定会发生"的可预测性）。

### 3.7 生命周期状态机

```
Created --start--> Starting --EVT_READY--> Ready
                        \--init 失败--> Failed --(内部收尾)--> Stopped
Ready/Starting --stop--> Stopping --join 后发 EVT_STOPPED--> Stopped
Created --stop--> Stopped (直接发 EVT_STOPPED)
Created/Stopped --destroy--> (句柄释放)
```

- `stop`：任意线程（含回调内）可调，受理即返回；重复调用幂等（不再发事件）。`Created` 状态
  没有收尾动作，直接在 `stop()` 的调用线程把状态置 Stopped 并发 `EVT_STOPPED`（此时还没启动
  任何内部线程，不可能与其它事件并发）；`Starting` / `Ready` 由收尾线程在 join 之后发（那时
  client io 已停，也不会有并发）—— 无论哪个状态，一次成功的 `stop` 之后**恰好一条**
  `EVT_STOPPED`，所以 SDK 的 `dispose()` 可以无脑 "stop → 等 `EVT_STOPPED` → destroy"，
  不必先判断有没有 start 过。
- `EVT_STOPPED`：**最后一个事件**，之后不再有任何回调；发它之前状态已经是 Stopped。`destroy`
  只在 Created / Stopped 状态成功，其余返回 `ERR_STATE` 并提示"先 stop 并等 EVT_STOPPED"。
  **不要在事件回调里 `destroy`**：回调可能就跑在收尾线程上，会被 `isOnAnyIoThread()` 拦下
  （转 `ERR_STATE`）；请在等到 `EVT_STOPPED` 的那个线程上调 `destroy`（Dart/Java 的 listener
  型回调本身就在别的线程，天然满足）。
- 收尾线程只做收尾：等 `AgentSessionRuntime::running()==false` → 插件
  `shutdownAsync`（25s 上限）→ 停 client/server io → join → 置 Stopped → 发 `EVT_STOPPED`。
  事件回调因此可能发生在收尾线程上（而不是 client io），但那时其它线程已 join，仍然不会并发。
- 收尾时所有未完成调用在 `onRuntimeStopping`（client io 线程）里以 `code=-9 canceled`
  回执（与 `call_cancel` 同一个 `settle` 路径）；它们在 join 之前完成，所以一定排在
  `EVT_STOPPED` 前面，保证"每个 reqId 恰好一个回执"。
- `Stopping` 期间新提交的 `call` 直接返回 `ERR_STATE`（不入表、也不会有回执）—— "恰好一个
  回执"只针对已受理（拿到 reqId）的调用。`call_cancel` 在 `Stopping` / `Stopped` 仍可调（幂等）。

### 3.8 内部结构（新增两个文件）

| 文件 | 内容 |
|---|---|
| `agent/lib/src/ffi/ffi_call.h/.cpp` | `CallRegistry`：reqId 分配（原子自增）、**紧凑待完成记录**（`unordered_map<reqId, PendingCall>`，mutex 保护；只存值，不放 `std::function`）、**共享一个 `steady_timer` + 最早到期小顶堆**（client io 执行器；`timeoutMs == 0` 与本地 op 不登记）、`settle()`（CAS 去重）、`cancel()`、`failAll()`（收尾） |
| `agent/lib/src/ffi/ffi_ops.h/.cpp` | op 表：`name → {args 校验, 完成方式, 默认超时, 失败错误码, 发送函数}`；参数解析与 wire 消息组装（复用 `parseUserInputJson` / `io::addModelFromJson` 等既有实现） |

`FfiAgentRuntime` 只保留：状态机、线程、`sessionId_`、日志缓冲、`call()` / `callCancel()`
转发、生命周期。`FfiClientAgentIO` 只保留：事件序列化（改成 owned payload）+ 应答分流
（`reqId > 0` → 回执；`reqId == 0` → 广播事件；未匹配 → 丢弃并记 debug 日志）。`EVT_WIRE`
随 `send_wire` 一起删除：原来原样转发的 `WireInputAck` / `WireMessageQueueUpdate` /
`WirePermissionState` 等改成专用事件（`EVT_RESULT` / `EVT_MESSAGE_QUEUE` / `EVT_PERMISSION_STATE`）。

### 3.9 SDK 形态（Dart 为例）

```dart
final client = await AgentxxClient.connect(configJson: ..., modelJson: ...); // create + start + 等 READY
// 需要降低增量事件频率时自己打开（默认关闭，建议 16~33ms）: client.enableDeltaBatch();

// 1. 普通 await
final models = await client.getModelInfo();            // Future<Map<String, dynamic>>
await client.selectModel('m2');                        // 等到服务端生效（ack）

// 2. 顺序保证: 先等前一步的回执再发下一步
await client.setFullAuth(true);
await client.sendInput('现在按完全授权跑');

// 3. 取消: 显式取消令牌 (Dart Future 本身不可取消)
final token = AgentxxCancelToken();
final f = client.listSessions(cancel: token);
token.cancel();                                        // → agentxx_ffi_call_cancel(reqId)
try {
  await f;
} on AgentxxCanceledException {
  // 用户放弃, 忽略
}

// 4. 不要结果: 显式声明, 不占 Completer (回执仍会到达, 直接丢弃)
client.callNoWait('input.cancel');

await client.dispose();                                // stop → 等 EVT_STOPPED → destroy
```

实现要点：

- `on_event = NativeCallable.listener(...)`（静态函数 + 全局 `handle → 实例` 表），
  回调里 `utf8.decode` 后 `agentxx_ffi_string_free`，**不再有 `_pollLoop`**；
- `reqId → Completer` 表；`EVT_RESULT` 的 `ok:false` → `completeError(AgentxxException)`；
  `code=-9` 用 `AgentxxCanceledException` 子类，方便调用方区分；
- `AgentxxCancelToken.cancel()` → `agentxx_ffi_call_cancel`；令牌在调用完成时自动解绑；
- `callNoWait(op,args)`：提交后立刻丢弃 Completer（仍在 `reqId` 表里登记，收到回执时忽略）；
- `dispose` 期间的未完成 Future 统一 `completeError`；`EVT_STOPPED` 之后关闭 `NativeCallable`。

## 4. 关键决策

| 编号 | 决策 | 说明 |
|---|---|---|
| D1 | 导出面收敛成 **一个 `agentxx_ffi_call`** + `call_cancel` + 2 个本地同步入口（`get_capabilities` / `drain_logs`） | 新增功能只加 op 名；绑定不需要为新符号重新生成 |
| D2 | 回执走**事件**（`EVT_RESULT` + `reqId`），不加"每调用一个完成回调" | 绑定侧一个回调 + 一张 Future 表；per-call 回调在 Dart/Java/C# 都要额外保持函数指针存活 |
| D3 | `EVT_RESULT` 里 `error.name` 为稳定字符串，`code` 为 `AGENTXX_FFI_ERR_*` | SDK 按名字判定，不受错误码调整影响 |
| D4 | 事件 payload **所有权交给回调**（`AgentxxString*`） | 让 listener 型绑定（Dart `NativeCallable.listener`、Node threadsafe function）可以直接用，C 侧不再需要事件队列 |
| D5 | 删除 `agentxx_ffi_event_queue_*` 4 个符号 | 有了 D4 就没有存在意义；避免"两套事件入口" |
| D6 | wire 请求/应答补 `reqId` 回显 | 并发调用 + 精确匹配；`WireListDir` 已有，照抄同一写法 |
| D7 | 新增通用 `WireAck`，**所有命令类请求**都有回执 | 一个消息类型覆盖 12 个命令（含 `hosttool.register`），不给每个命令加 result 类型 |
| D8 | 删除 `send_wire` 与 `EVT_WIRE` | FFI 与 agent 同进程同版本，所有操作都能建模；少一层白名单 |
| D9 | `input.send` 用自己的 reqId 当 wire `requestId` | ack 直接按 reqId 回执，不需要第二张映射表 |
| D10 | `stop` 异步 + `EVT_STOPPED`；`destroy` 只在 Created/Stopped 成功 | 收尾有显式完成信号；`destroy` 变成纯释放（O(1)） |
| D11 | `get_capabilities` / `drain_logs` 保持同步 | 纯本地、不跨线程等待；顺手修 `libraryVersion` 与能力清单 |
| D12 | 不保留旧接口，`AGENTXX_FFI_API_VERSION` = 4 | 最低支持版本已抬、未发布；旧符号/旧语义直接删 |
| D13 | **提供 `agentxx_ffi_call_cancel`**（取消只停止等待） | Dart 没有 Future 取消，但显式令牌 + 一次 C 调用即可；C 层用 `settled` CAS 保证"恰好一个回执" |
| D14 | **每个 op 都有回执**，命令类走 `WireAck` | 支持"等前置操作完成再继续"；`select_model` 选错模型不再静默 |
| D15 | 每个 op 有**默认超时**，`timeoutMs` 可覆盖 | `compact` 不限、`switch` 60s、`model.add` / `model.remove` / `fs.list_dir` 30s、其余 10s |
| D16 | `session.compact` 的完成由中间件新增的 `SummarizationCompactDone` 事件驱动 | 唯一需要动中间件的地方（约 15 行）；不这么做就只能回"已发起" |
| D17 | 性能默认值：首屏只同步尾窗（默认 200 条）、headless 不序列化事件；**增量合批默认关闭**，SDK 只给开关与建议值（~24ms） | 尾窗与 headless 不改变语义；合批会推迟增量到达，是否开启交给嵌入方按渲染节奏决定，基准给出开/关对比；详见 §9 |

## 5. 分步实施

> 每步结束都要求：顶层 Debug 构建通过 + 相关测试绿。第 2 步是唯一"实现 + 测试同一批"
> 的步骤（C API 换代后旧测试无法编译），其余步骤都可单独验收。

### 第 1 步：wire 相关性 + 通用回执

改动：

- `agent/lib/include/agentxx/agent/io/agent_io_transport.h`：
  - 给 9 对"typed 应答"消息（`WireGetModel`/`WireModelInfo`、`WireGetContext`/
    `WireContextMessages`、`WireListSessions`/`WireSessionList`、`WireAddModel`/
    `WireAddModelResult`、`WireRemoveModel`/`WireRemoveModelResult`、`WireRenameSession`/
    `WireRenameSessionResult`、`WireGetViewMessages`/`WireViewMessagesPage`、
    `WireGetPermissionState`/`WirePermissionState`、`WireGetAppendComponentInfo`/
    `WireAppendComponentInfo`）的请求与应答各加 `uint64_t reqId = 0;`（`WireSetFullAuth` 也带，
    应答复用 `WirePermissionState`）；
    顺带给 `WireGetContext` 加 `uint32_t tail = 0`（0 = 整份，>0 = 只回最近 N 条，
    `context.get` 的大载荷优化，§9.2-8）；
  - 给"命令类"请求加 `uint64_t reqId = 0;`：`WireSelectModel`、`WireSwitchSession`、
    `WireCompactContext`、`WireCancel`、`WireClearMessageQueue`、`WireRemoveQueueItem`、
    `WireInterruptAndRunNext`、`WireInterruptResponse`、`WireHostToolRegister`、
    `WireHostToolUnregister`、`WireHostToolResult`、`WirePluginDataUp`；
  - 新增 `WireAck { uint64_t reqId; bool ok; std::string reason; std::string error;
    utilxx_base::Json data; }`，**追加到** `WireMessage` variant 末尾（`serialize` 是通函式
    `std::visit`，只要补 `toJson` 重载）。
- `agent/lib/include/agentxx/agent/io/wire_protocol.h` + `agent/lib/src/agent/wire_protocol.cpp`：
  `make*` 只在 `reqId != 0` 时写字段；`*FromJson` 读回；`MsgType` 加 `ack`；
  `getDeserializerMap()`（`wire_protocol.cpp:512`）加一条映射，否则对端收到的 `ack` 会被丢掉。
- `agent/lib/src/agent/io/session_server_agent_io.cpp`：
  - 新增 `sendAck(sender, reqId, ok, reason, error, data)` 与
    `broadcastToClientsExcept(sender, msg)`；
  - 9 对新应答前 `resp.reqId = m.reqId`（`buildModelInfo` 加 `reqId` 参数）；`WireGetContext`
    按 `tail` 切片（与 `get_view_messages` 同样的尾部切片写法）；
  - 命令分支在完成处回 ack：`cancel` / `queue.*` / `select_model`（加"模型存在"校验，
    空名字 = 沿用当前）/ `switch_session`（`co_spawn` 末尾，在 `switchSession()` 之后）/
    `compact_context` / `interrupt_response`（未知 id → `interrupt_not_found`）/
    `host_tool_register|unregister|result` / `plugin_data_up` / `set_full_auth`；
  - `acceptSessionScope` 增加可选 `reqId`：非 0 时回 `session_mismatch` ack。
- `agent/lib/include/agentxx/event/events.h`（`Topic::SummarizationCompact` 旁边，105 行附近）
  新增 `SummarizationCompactDone` 主题与 `EventCompactContextDone{sessionId, ok}` 载荷；
  `agent/lib/src/middlewares/summarization.cpp` 的 `compactSessionContext` 在**每条退出路径**
  发布（含提前 `co_return false` 的分支）；`SessionServerAgentIO` 新增订阅，按会话 FIFO 回 ack。
- 测试：`agent/test/core/test_wire_roundtrip.cpp` 加 `reqId` / `WireAck` 往返断言；
  `agent/test/core/test_wire_schema.cpp` 用 `AGENTXX_UPDATE_WIRE_SCHEMA=1` 重新生成
  `agent/schema/wire-schema.json` 与 `docs/zh-cn/design/wire-protocol-fields.md`。

验证：`wire_roundtrip` / `wire_schema` / 会话/权限相关模块；顶层 Debug 构建绿。

### 第 2 步：FFI 内部换代（调用表 + op 分发 + C API v4 + 测试重写）

新增：

- `agent/lib/src/ffi/ffi_call.h/.cpp`：`CallRegistry`
  - `uint64_t begin(op, timeoutMs, context)`（分配 reqId + 登记；有超时才入到期堆并重臂共享定时器）；
  - `settle(reqId, outcome)`（`settled` CAS，赢者 `erase` + 重臂定时器 + 投递 `EVT_RESULT`）；
  - `cancel(reqId)`（调 `settle(canceled)`；不存在 → false，调用方按幂等处理）；
  - `failAll(code, name)`（收尾用）。
- `agent/lib/src/ffi/ffi_ops.h/.cpp`：op 表（§3.3 全集）。每个 op 一个处理函数：
  `int32_t (FfiAgentRuntime&, const Json& args, CallContext&, std::string& err)`；
  `CallContext` 携带 reqId / 完成方式 / 默认超时 / 失败错误码 / 发送动作。

重写：

- `agent/lib/include/agentxx/ffi_api.h`（v4，§3.2）
- `agent/lib/src/ffi/ffi_api.cpp`：只剩参数校验 + 转发（`create`/`start`/`stop`/`destroy`/
  `call`/`call_cancel`/`drain_logs`/`get_capabilities`/内存/版本）
- `agent/lib/src/ffi/ffi_runtime.h/.cpp`：
  - 删 `SyncWait` / `syncWaits_` / `syncQuery` / `onSyncReplyOnClientThread` /
    `getModelInfo` / `getContextMessages` / `listSessions` / `addModel` / `removeModel` /
    `listModels` / `sendWire` / `toolRegister` / `toolUnregister` / `toolRespond` /
    `sendInput` / `cancel` / `selectModel` / `switchSession` / `setLanguage` /
    `interruptRespond`（全部变成 op 处理函数）；
  - 新增 `int call(std::string_view requestJson, uint64_t* outReqId, std::string& err)` 与
    `int callCancel(uint64_t reqId, std::string& err)`；
  - `getCapabilities` 补 `ops` / `features`，`libraryVersion = agentxx::kVersion`；
  - 首屏尾窗：`config_json` 新增 `syncTailCount`（默认 200），传给
    `rtCfg.initialSyncTailCount`（§9.2-2）；
  - 日志：`FfiLogSink` 从 `ThreadedLogSink` 改为普通 `LogSink`（client io 上 1s `pump()`，
    `drain_logs` 前再 pump），少一条常驻线程（§9.2-9）；
  - 大载荷按 `std::move` 取字段组装 wire 消息（§9.2-6）。
- `agent/lib/src/ffi/ffi_client_io.h/.cpp`：
  - 删 `SyncKind` / `onSyncReply`；
  - `emitEvent` 改成"构造 `AgentxxString`（`agentxx_ffi_malloc` 分配）→ 调用回调 → 不回收
    （所有权已交）"；**先判 `on_event == nullptr` 再构 payload**（headless 零序列化，§9.2-3）；
  - 应答分流：`reqId > 0` → `runtime.onCallResult(reqId, json)`（不再发事件）；`reqId == 0` →
    按 §3.2 的清单发广播事件（`EVT_MODEL_INFO` / `EVT_PERMISSION_STATE` /
    `EVT_MESSAGE_QUEUE` / `EVT_COMPONENTS` 等）；新增 `WireAck` 分支；
  - 删 `notifyServerReady`（死代码）；
  - `onRuntimeStopping` 里 `failAll(canceled)`。
- 删除 `agent/lib/src/ffi/event_queue.cpp`（GLOB 构建，删完要重跑 cmake 配置）。
- `agent/lib/ffi_symbols.map`：15 个符号。
- 测试 `agent/test/test_ffi_c_api.cpp`（重写）：
  - 通用 `waitResult(reqId, timeout)`；
  - 每个 op 一条用例（含 `ok:false` 分支：非法 op / 非法 args / 超时 / 拒绝 / ack 失败）；
  - 并发：同一句柄 20 个 `model.list` 同时在飞，各自拿到自己的回执；
  - 回调内调 `call` / `call_cancel`（原来会 10s 死锁的场景，现在应毫秒级返回）；
  - 取消：长请求（mock 服务端不回）→ `call_cancel` → 立刻 `canceled` 回执，且只有一个回执；
    迟到应答不再产生第二条；取消已完成的 reqId → `OK`；
  - 顺序：`select_model` 生效后再 `model.list` 能看到新模型（服务端 ack 后才继续）；
  - 事件 payload 所有权：回调 free，ASan 下无泄漏/无 UAF；
  - 删掉事件队列用例。

验证：`ffi_c_api` 全绿；`command` / `filesystem` 等无关模块抽查；顶层 Debug 构建绿。

### 第 3 步：生命周期异步化（`stop` → `EVT_STOPPED`）

- `ffi_runtime.cpp`：`stop()` 只做 CAS（Created→Stopped：置状态后在调用线程同步发
  `EVT_STOPPED`；Running→Stopping：启动收尾线程），收尾线程 = `std::thread`（存成员，
  `destroy`/析构时 join；析构里判断"当前线程就是它" 时 detach，避免自我 join）。
- `stopInternal` 保持原顺序（`runtime_->stop()` → 等 `running()==false` →
  `shutdownAsync` → 停 io → join），末尾发 `EVT_STOPPED`。
- `destroy()`：仅 Created/Stopped 成功；否则 `ERR_STATE`。
- `isOnAnyIoThread()` 加入收尾线程 id（收尾线程上调 API 也要拦）。
- 测试：`EVT_STOPPED` 是最后一条记录、之后无事件；运行中 `destroy` → `ERR_STATE`；
  stop 期间提交的调用收到 `canceled`；重复 stop 只发一次 `EVT_STOPPED`。

### 第 4 步：benchmark FFI 场景

- `agent/benchmark/bench_resource.cpp:1607`：符号解析列表改成 15 个；
  `agentxx_ffi_get_context_messages` → `agentxx_ffi_call("context.get")` + 等 `EVT_RESULT`；
  事件 tracker 释放 payload；`destroy` 前先 `stop` 并等 `EVT_STOPPED`。
- 新增三个 FFI 子场景（§9.4）：
  1. 调用吞吐：空转 `model.list` 1k 次（C 边界 + 回执 + 事件回调的固定开销）；
  2. 事件吞吐：mock 流式固定 token 数，`delta.set_batch(0)` 与 `24ms` 各跑一遍；
  3. 首屏体积/耗时：长会话 `syncTailCount = 0` 与 `200` 各跑一遍（同步字节数 + 接入延迟）。
- 只在 Release 构建编译，验证方式：Release 构建通过 + `agentxx_benchmark resource_ffi` 跑通。

### 第 5 步：Dart 绑定与 SDK 异步化

- `agent/ffi/dart/ffigen.yaml`：函数/结构体/枚举清单按 v4 重写（去掉事件队列与
  不存在的 `agentxx_ffi_set_permission`），`dart run ffigen` 重新生成
  `lib/agentxx_ffi_bindings.dart`。
- `agent/example/ffi/dart/lib/src/agent_client.dart`：重写为 `Future`/`Stream`
  （§3.9）；删除 `_pollLoop` 与 `_syncQuery`；新增 `call(op,args,{cancel,timeout})`、
  `callNoWait`、`reqId → Completer` 表、`AgentxxCancelToken`、`AgentxxCanceledException`；
  **合批默认关闭**：提供 `enableDeltaBatch({maxDelayMs = 24})` / `disableDeltaBatch()` 两个
  便捷方法（内部就是 `delta.set_batch`），文档与示例给建议值但不替调用方打开；
  `connect({syncTailCount = 200})` 传给 `config_json`（§9.2-1/2）。
- `agent/example/ffi/dart/lib/src/events.dart`：事件类型与解析（`EVT_RESULT` /
  `EVT_STOPPED` / `EVT_MESSAGE_QUEUE` / `EVT_PERMISSION_STATE`；删 `EVT_WIRE`）。
- `agent/example/ffi/dart/lib/src/repl.dart`、`bin/agentxx_cli.dart`、
  `example/smoke_check.dart`：全部改 `await`；`/model`、`/sessions`、`/context`、`/status`
  不再阻塞事件循环；给 `/sessions` 之类的长查询加一个"取消"演示（按 ESC 取消）。
- `agent/ffi/dart/README.md`、`agent/example/ffi/dart/README.md`：更新用法（含取消与顺序示例）。

验证：`dart analyze`；`example/smoke_check.dart`（mock LLM）；手工跑 `agentxx_cli`。

### 第 6 步：文档与规则收尾

- `docs/zh-cn/design/ffi.md`：§2 线程模型、§4 契约（符号表 → 15 个 + 调用/取消/事件/op 表）、
  §1 的"仅 26 个顶层 C 符号"改成 15（与 §4.3 对齐）、
  §4.2 事件队列 → 删除、§4.3 符号清单、§4.4 事件表（加 `EVT_RESULT` / `EVT_STOPPED` /
  `EVT_MESSAGE_QUEUE` / `EVT_PERMISSION_STATE`，删 `EVT_WIRE`）、新增"顺序与取消"
  一节（给其他语言绑定作者）；配置段补 `syncTailCount`，性能段写合批/尾窗/headless/大载荷
  四个默认值（与 plan §9 对齐）。
- `docs/en/design/ffi.md` 同步。
- `docs/zh-cn/design/wire-protocol-fields.md` + `agent/schema/wire-schema.json`（第 1 步已生成，
  这里确认无遗漏；补 `WireAck` 与各 `reqId` 字段）。
- `AGENTS.md`：把"FFI API 抬到 3"改成 4，并补一句"FFI 只有 `agentxx_ffi_call` /
  `call_cancel` + 事件回执"。
- `docs/zh-cn/design/index.md`：若提到 FFI 同步查询，改为异步调用。
- 头文件导出集与 `ffi_symbols.map` 的一致性自查（`boundaries` 不覆盖 FFI，用脚本/测试比对）。

### 第 7 步：全量回归与收尾

- 全量 `agentxx_test --log-level warn`；重点模块：`ffi_c_api` / `wire_roundtrip` /
  `wire_schema` / 会话与权限相关模块 / `plugin_*` 抽查。
- Release 构建（含 benchmark、LTO）通过。
- `work.md` 补验证清单与偏差；`index.md` 补结论。

## 6. 兼容与边界

- **不保留兼容**：`AGENTXX_FFI_API_VERSION` = 4；同步查询、事件队列、`send_wire`、
  按种类 FIFO 全部删除；Dart 绑定重新生成后不保留旧方法名。
- **wire 版本不动**：`reqId` / `WireAck` 都是可选能力（`reqId == 0` 不写字段、不回 ack），
  `kVersion`/`kMinVersion` 仍为 4；TUI/WS 客户端不设置即保持原行为。
- **TUI 不受影响**：所有新增 ack 都要求 `reqId != 0`；`remove_queue_item` 的 `WireError` 保留。
- **不是本任务范围**：插件 C ABI、UI 描述层、WSS 远程客户端、其他语言绑定（只提供文档与
  Dart 参考实现）。
- **性能**：每个事件多一次 `malloc` + 一次 `memcpy`（原来借用视图）；`select_model` 之类的
  命令多一次 `ack` 往返（本地 channel，可忽略）。流式文本量大时，嵌入方可以自行开启合批
  （`delta.set_batch(16~33ms)`；默认关闭）。
  优化项、默认值与度量见 §9。

## 7. 验证

| 阶段 | 验证 |
|---|---|
| 第 1 步 | 顶层 Debug 构建；`wire_roundtrip` / `wire_schema` 绿；TUI 交互抽查（select_model / compact / queue） |
| 第 2 步 | 顶层 Debug 构建；`ffi_c_api` 全绿（每个 op、并发、超时、取消、顺序、回调内调用、所有权） |
| 第 3 步 | `ffi_c_api` 生命周期用例；`EVT_STOPPED` 顺序断言；取消与收尾竞态 |
| 第 4 步 | Release 构建 + `agentxx_benchmark` 的 `resource_ffi` 跑通；三个新子场景有数（调用吞吐 / 合批开与关的事件吞吐 / 首屏尾窗 0 vs 200） |
| 第 5 步 | `dart analyze` 无告警；`smoke_check.dart` 全绿；CLI 手工验收（含取消） |
| 第 6 步 | 文档交叉引用无死链；导出集与 `ffi_symbols.map` 一致 |
| 第 7 步 | 全量 `agentxx_test --log-level warn` 0 失败；Release 构建通过 |

## 8. 风险与未决项

| 风险 | 影响 | 应对 |
|---|---|---|
| 某条应答忘记回填 `reqId` / 忘记 `sendAck` | 对应 op 固定超时 | 每个 op 一条用例；FFI 侧"只有 0/1 两种结局"，漏回即超时，容易发现 |
| 取消与服务端应答赛跑 | 可能两条回执 | `settled` CAS + 只在赢家 `erase`/发事件；用例覆盖 |
| 取消被误认为"撤回操作" | 调用方以为消息没发出去 | 文档 + SDK 注释明确"只停止等待"；需要撤回用 `input.cancel` |
| 异步 op 的回执顺序 | "同时提交 A、B"不保证 A 先落地 | 文档明写"要顺序就先 await"；`session.switch` 的回执排在它引发的状态事件之后 |
| 回调忘记释放 payload | 内存泄漏（不崩溃） | 头文件/文档/reference SDK 三处强调；测试里 ASan 检查 |
| 收尾线程自我 join | `std::terminate` | 析构里判断 `std::this_thread::get_id()` 与收尾线程相同则 detach |
| 一次性事件队列删除 | C 嵌入方需要自己排队 | 文档给 10 行 C 示例（mutex + deque） |
| 中间件新增 compact 完成事件 | 多一处跨模块改动 | 约 15 行；若想缩范围可退回"发布即 ack"并在文档写明不保证完成 |

已定案（原未决项）：

1. `agentxx_ffi_call_cancel` **做**（D13）：显式取消令牌 → C 层 `settle(canceled)` →
   原调用 Future 抛 `AgentxxCanceledException`。
2. `model.select` **要服务端回执**（D14）：服务端校验模型存在，回 `WireAck{data:{model}}`；
   选错模型不再静默。
3. `permission.set_full_auth` **要服务端回执**（D14）：回 `WirePermissionState{state, reqId}`；
   权限中间件缺失时 `ok:false`，不再"看起来成功"。

## 9. 性能：预算、默认值与可选项

### 9.1 值得优化的几处（改造前的现状，均可在代码里确认）

| 项 | 现状 | 影响 |
|---|---|---|
| 逐 token 事件 | FFI 的 `deltaBatch` 默认 0，Dart SDK 也从未调用 `set_delta_batch` → 每个 token 一次 Json 构树 + dump + malloc + C 回调 + isolate 消息 + Dart `jsonDecode` | 本地快速模型（100+ tok/s）下每秒上百条跨边界事件，Dart 侧 GC 压力最大的一项（新 SDK 提供开关但仍默认关闭，见 §9.2-1） |
| 首屏完整同步 | `ffi_runtime.cpp:388 initialSyncTailCount = 0`（完整同步），且 FFI 没有分页入口 | 长会话首屏是 MB 级 JSON：服务端构 Json、dump 成 string、再拷进 malloc 块，Dart 侧再整棵解析 |
| 同步查询阻塞 | 10s 上限 | 不是 CPU，是延迟：阻塞期间事件全部停摆（新设计已解决） |
| 每实例 3 条线程 | server io / client io / `FfiLogSink`（`ThreadedLogSink` 自带后台线程） | 多实例嵌入时线性叠加；每条日志一次线程唤醒 |
| 每调用一个定时器（新设计若按最直白实现） | 100 个并发调用 = 100 个 `steady_timer` + `std::map` 节点 | 突发调用下多出百次分配 |

传输层不需要优化：`ChannelAgentIOTransport` 经 `concurrent_channel` **直接传 `WireMessage`
对象**，进程内零序列化；FFI 侧唯一的 JSON 工作就是 C 边界的入参与事件载荷。

### 9.2 设计里直接落地（默认值与实现细节，不改变语义）

1. **合批默认关，只提供开关**：`delta.set_batch` 保留为 op（`maxDelayMs<=0` 逐条，>0 才合批），
   SDK 提供 `enableDeltaBatch({maxDelayMs = 24})` / `disableDeltaBatch()`，但**建连后不自动
   打开**。理由：合批会把增量推迟最多 `maxDelayMs`，这是可感知的行为变化，应由嵌入方按自己的
   渲染节奏决定。基准里给出开（24ms）与关的事件数 / CPU 对比，文档给建议值（16~33ms）；
   打开后事件数约降到 1/5~1/10。
2. **首屏只发尾窗**：`rtCfg.initialSyncTailCount` 改为配置项 `syncTailCount`（默认 200），
   更早历史由新增的 `context.view_page` 按页拉。长会话首屏从 MB 级降到几十 KB。
3. **headless 零序列化**：`emitEvent` 先判 `on_event == nullptr` **再**构 payload
   （现在是先 `dump()` 再判空，纯 headless 嵌入方白花一次序列化 + 一次分配）。
4. **超时定时器共享**：所有待完成调用共用一个 `asio::steady_timer` + 最早到期小顶堆，
   而不是每调用一个 timer；`timeoutMs == 0` 与"本地 op"不登记定时器。
5. **待完成记录保持紧凑**：`PendingCall` 存 op 索引 + reqId + 截止时间 + `settled`，
   不放 `std::function`；容器用 `unordered_map`（reqId 稠密自增）。
6. **大载荷按 move 组装**：解析请求后 `std::move` 取字段（`input.send` 的 base64、
   `hosttool.respond` 的结果正文、`plugin.data_up` 的 data），避免"解析一次拷贝 + 组包一次拷贝"。
7. **附件优先传路径**：同机场景让嵌入方传 `path_or_url`，由服务端读文件编码，避免 base64
   经 FFI 边界（10MB 图 ≈ 13MB base64 字符串，且要在 Json 树里再存一份）。
8. **大结果限量**：`session.list` 的 `limit` 默认 50；`context.get` 增加可选 `tail`
   （只取最近 N 条消息），避免"看最后 20 条却传了整份 400KB 上下文"。
9. **去掉日志后台线程**：改用 `utilxx_base::LogSink`（已有 4096 条有界队列）+
   client io 上 1s `pump()`，`drain_logs` 前再 pump 一次。每实例少一条线程，每条日志少
   一次线程唤醒；环形缓冲仍保留最近 512 条。**注意**：`LogSink` 队列满时丢**新**条目
   （不是丢最旧），所以必须低频 pump，不能等到 `drain_logs` 才排。

### 9.3 实测后再决定（不写进第一版）

| 可选项 | 代价 |
|---|---|
| `EVT_DELTA_TEXT_BATCH` 改二进制载荷（msgId/kind 长度 + 文本字节） | 破坏"payload 恒为 JSON"契约，SDK 要分流两种载荷 |
| 给 `Json` 加 `dumpTo(char*, size_t)`，直接写进 malloc 块 | 要动 `cxx_utilxx_base`；只省掉一次 memcpy（大载荷才有意义） |
| `agentxx_ffi_malloc` 加尺寸分级无锁缓存 | 事件在 SDK 线程释放（跨线程回收），实现复杂度高，ASan 下噪声大 |
| 请求里加 `"ack": false` 跳过服务端回执 | 只对"高频命令类 op"有意义；目前 FFI 没有这种 op |
| 事件合并（多个 `EVT_RESULT` 合一条） | 收益小（每次调用才一条），不值得增加协议复杂度 |

### 9.4 度量与基准

- **FFI 侧轻量计数**（默认零开销，只在 `devMode` / 基准打开）：调用总数、按结局分布
  （result / timeout / canceled / stopped）、事件数、事件字节数、序列化累计耗时。
  沿用 `AgentConfigStatic::enableBenchmark` 的做法（编译期不关、运行期一个原子判断）。
- **基准场景**（`agentxx_benchmark`，第 4 步加）：
  1. 调用吞吐：空转 `model.list` 1k 次（测 C 边界 + 回执 + 事件回调的固定开销）；
  2. 事件吞吐：mock 流式输出固定 token 数，合批开/关各跑一遍（对比事件数、CPU、RSS）；
  3. 首屏体积/耗时：长会话 `syncTailCount = 0` vs `200`（对比同步字节数与接入延迟）。
- 口径与现有 resource 基准一致：同机空载、固定 token 数、记录 RSS/PSS + 用户/内核 CPU，
  报告写进 `resource/benchmark/`。

## 附录 A：备选方案（不加 wire `reqId` 时）

FFI 侧按 op 种类串行：每个种类维护 `deque<PendingCall>`，一次只发一条，收到应答再发下一条；
超时的调用必须把该种类"挂起"到迟到的应答到达为止，否则后一条会拿到前一条的结果。
实现更复杂、并发受限、超时语义不精确，且**无法让命令类操作产生回执**（服务端根本不答）——
只有在"服务端不允许改 wire"时才选它。本计划不采用。
