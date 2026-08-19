# libagentxx FFI C API 导出层

> 版本: 2026-08 · 状态: 已实现并测试通过 (`agentxx_test ffi_c_api`: 57/57)
> 相关文档: [design.md](design.md) (主程序架构) · [plugins.md](plugins.md) (纯 C ABI 插件范式)

## 1. 概述与目标

为 `libagentxx_shared.so` 提供**受控的 C ABI 导出面**, 供其他编程语言
(Python/Rust/Go/C#/Java/Node 等) 经 FFI (ctypes / JNA / jextract / wasm-abi
等) 在**宿主进程内嵌入 agent 会话运行**, 复用 agentxx 全部能力 (ReAct 循环、
工具、MCP、插件、子代理委派、会话持久化、HIL 权限/中断)。

三个核心目标:

1. **导出符号收敛**: 共享库导出面从 ~17 万 C++ 符号 (默认全导出) 收敛为
   仅 21 个顶层 C 符号, 满足 "导出符号必须是 C Api" 的硬约束 (libstdc++
   等运行库依赖仍为 DT_NEEDED, 属正常动态依赖)。
2. **交互参考 client**: FFI 层实现一个自定义 `AgentIOBase` client 端点
   (`FfiClientAgentIO`), 经进程内 `ChannelAgentIOTransport` 与
   `SessionServerAgentIO` (由 BaseAgent 驱动) 通信——与 TUI/CLI client
   **完全同构**, agent 核心零改动。
3. **错误处理双通道**: 同步错误 = 返回值错误码 + 可选的 `char** log` 参数
   (内部填充执行过程日志/错误详情, 宿主 `agentxx_free` 释放); 异步错误 =
   `EVT_ERROR` / `EVT_TURN_END {has_error}` 事件。

## 2. 总体架构

```
┌─────────────────────────────────────────────────────────────┐
│ 宿主程序 (Python/Rust/Go/... 经各自 FFI 绑定)                  │
│   持有 AgentxxAgent* 句柄, 注册 AgentxxCallbacks C 回调        │
└──────────────┬──────────────────────────────────────────────┘
               │ extern "C" API (agentxx/ffi_api.h, 纯 C)
┌──────────────▼──────────────────────────────────────────────┐
│ FFI 层 (编译进 libagentxx_shared, 新增 src/ffi/, 零侵入核心)    │
│  ┌──────────────────────┐  ┌────────────────────────────────┐│
│  │ FfiAgentRuntime      │  │ FfiClientAgentIO               ││
│  │ (句柄实体:            │  │ (AgentIOBase client 端点)        ││
│  │  ioCtx+线程/CodeAgent │◄─┤  onDelta→JSON→on_event 回调      ││
│  │  /AgentHost/日志环/   │  │  WireInterruptRequest→挂起应答    ││
│  │  同步查询 FIFO)       │  │  WireExpired→EVT_INTERRUPT_EXPIRED││
│  └─────────┬────────────┘  └────────┬───────────────────────┘│
│            │ ChannelAgentIOTransport│ (进程内, WireMessage 零拷贝)│
│  ┌─────────▼───────────────────────▼──┐                     │
│  │ SessionServerAgentIO + CodeAgent    │ (现有核心, 完全复用)  │
│  └─────────────────────────────────────┘                     │
└──────────────────────────────────────────────────────────────┘
```

装配时序 (复刻 `agent/client/src/mode_runners.cpp::setupLocalUnifiedDirect`,
单 io 线程模式, clientEx = agentEx):

1. `ChannelAgentIOTransport::makePair(agentEx, agentEx)` → 双端点 `setTransport`
2. **先** `co_spawn(serverIO->runTransportLoop())`: init 期间客户端请求
   (`WireHello`/`WireGetModel`) 有消费方, 不排队积压
3. `co_spawn(runAgentMain)`: `init()` → `AgentHost::create+attachRoot`
   (子代理委派) → `requestAppendComponentInfo` → `EVT_READY` → `serverIO->run()`
   (会话驱动循环)
4. `co_spawn(clientIO->runTransportLoop())`: 事件分发 → C 回调
5. `sendToPeer(WireHello)` 触发全量 Sync/增量重放; `WireGetModel` 预取模型信息

安全停止 (`stopInternal`, 调用方线程执行):

1. `clientIO->failAllPendingInterrupts()` (失败挂起中断, 等待协程结束)
2. `clientIO->transport()->close()` (client 接收循环结束)
3. `serverIO->stop()` (dispatch 到 io 线程 `stopImpl`: 关输入 channel/
   取消轮次/失败 server 侧 pending) → `run()` 循环退出
4. 轮询 `serverIO->running()==false` (最长 20s)
5. `removeSink` → `workGuard.reset()` → `ioCtx->stop()` → `join` io 线程

## 3. 线程模型

- **单 agent io 线程**: `FfiAgentRuntime` 持有独立 `io_context` +
  专用线程 (即 `CodeAgent::ioCtx`); 所有 agent 状态、端点、事件分发均在该
  线程访问 (与 TUI 一致的协作式单线程), 内部无锁。
- **事件回调**在 agent io 线程同步调用, **必须快速返回** (与插件回调同约定);
  宿主长任务自行投递到自己的线程池。
- **C API 任意线程可调用**: 会话交互类经 `asio::post` 投递 io 线程串行执行;
  同步查询类经 promise/future 等待 (最长 10s)。
- **约束**: `agentxx_stop`/`agentxx_destroy` 不得在回调 (agent io 线程) 内
  调用, 返回 `AGENTXX_ERR_STATE` (避免自 join 死锁); 宿主需从自己的线程调用。
- 回调内调用其他 API (send_input 等) 安全 (自动 post, 不死锁)。

## 4. C API 设计 (`agent/lib/include/agentxx/ffi_api.h`)

### 4.1 错误码与 `char** log`

```c
#define AGENTXX_OK              0
#define AGENTXX_ERR_INVALID    -1   /* 参数非法 (NULL 句柄/空串等) */
#define AGENTXX_ERR_STATE      -2   /* 状态错误 (未 start / 已 stop / io 线程内 stop) */
#define AGENTXX_ERR_JSON       -3   /* JSON 解析失败 */
#define AGENTXX_ERR_CONFIG     -4   /* 配置非法 (模型缺失/字段类型错误) */
#define AGENTXX_ERR_INIT       -5   /* agent init 失败 */
#define AGENTXX_ERR_INTERRUPT  -6   /* 中断 id 无效/已应答/已过期 */
#define AGENTXX_ERR_TIMEOUT    -7   /* 同步查询超时 (10s) */
#define AGENTXX_ERR_OOM        -8
#define AGENTXX_ERR_INTERNAL  -99
```

规范:

- 每个函数可选的 `char** log`; 失败时填入 NUL 结尾 UTF-8 详情 (错误信息 +
  内部异常经 `catchError` 转换, 含编码自动归一), **宿主用后 `agentxx_free`**;
  成功时置 NULL。
- `agentxx_create/start` 失败详情尾部拼接 `agentxx_drain_logs()` 积压的
  启动日志 (如 MCP 连接失败重试记录), 提升排障体验。
- 运行时日志: 每句柄一个 `ThreadedLogSink`, 收集 Info/Warn/Error 进 512 条
  环形缓冲, `agentxx_drain_logs()` 取出 (JSON 数组) 后清空。

### 4.2 句柄与生命周期

```c
AgentxxAgent* agentxx_create(const char* config_json, const char* model_json,
                             const AgentxxCallbacks* cb, char** log); // NULL=失败
int agentxx_start(AgentxxAgent*, char** log);   // 异步; 就绪经 EVT_READY
int agentxx_stop(AgentxxAgent*, char** log);    // 同步阻塞; 幂等
int agentxx_destroy(AgentxxAgent*, char** log); // 未 stop 自动 stop
```

`config_json` (AgentConfig 覆盖, 未知字段忽略):

```jsonc
{
  "dataDir": "~/.agentxx",            // 空=不持久化
  "enableSessionPersistence": true,
  "sessionPersistenceRoot": "",
  "permissionMode": "ask",            // ask|all_ask|pass|deny
  "permissionAllowPaths": ["..."], "permissionDenyPaths": ["..."],
  "skills": ["..."], "memoryFiles": ["..."],
  "mcpServers": {"ns": {"url": "...", "timeoutSec": 120}},
  "plugins": [{"path": "...", "enabled": true, "sides": "agent|client|auto", "args": {}}],
  "llmMaxRetry": 5, "agentName": "Agentxx",
  "interruptTimeoutSec": 0            // HIL 等待宿主应答超时, 0=不限
}
```

`model_json` (必填或经 config_json.model; 需 `isValid`: baseUrl 非空或
apiKey != "EMPTY"): `name/type/baseUrl/apiKey/modelName/apiPath/
connectTimeoutSeconds/readChunkTimeoutSeconds/sslVerify/
maxConcurrentConnections/anthropicVersion/modelContextMaxToken/
extraHeaders/extraConfig`。

### 4.3 事件回调

```c
typedef enum AgentxxEventType { EVT_READY, EVT_SYNC, EVT_DELTA, EVT_TURN_END,
    EVT_CONTEXT_STATS, EVT_MODEL_INFO, EVT_COMPONENTS, EVT_INTERRUPT_REQ,
    EVT_INTERRUPT_EXPIRED, EVT_PLUGIN_DATA, EVT_ERROR } AgentxxEventType;
typedef void (*AgentxxEventFn)(AgentxxEventType type, const char* payload_json, void* ud);
```

payload 全部为 JSON 字符串, 字段与 wire 协议 JSON 一致 (参照
`agentxx/agent/io/wire_protocol.h` / WS 客户端收到的格式, 语言绑定零新协议):

| 事件 | payload |
|---|---|
| `EVT_READY` | `{"threadId"}` |
| `EVT_SYNC` | `{"from_index","tail_hash","messages":[ViewMessage...]}` |
| `EVT_DELTA` | `{"type":"delta","kind":"text_token\|thinking_token\|tool_start\|tool_end\|turn_start\|turn_end\|node_start\|node_end\|message_tip\|system_message","seq",...}` |
| `EVT_TURN_END` | `{"has_error","error_message","interrupted","start_time_ms","duration_ms"}` |
| `EVT_CONTEXT_STATS` | `{"context_tokens","max_context_tokens","tps"}` |
| `EVT_MODEL_INFO` | `{"current_model","models":[...]}` |
| `EVT_COMPONENTS` | `{"notifications":[{"type","name","success","error_message"}]}` |
| `EVT_INTERRUPT_REQ` | `{"interruptId","threadId","node","value","argJson":{...,"inputs":[{"label","depict","type","defaultValue","enumValues"}]}}` |
| `EVT_INTERRUPT_EXPIRED` | `{"interruptId"}` |
| `EVT_PLUGIN_DATA` | `{"plugin","event","data"}` |
| `EVT_ERROR` | `{"code","message"}` |

### 4.4 会话交互 (异步) 与同步查询

```c
int    agentxx_send_input(a, text, log);         // WireUserInput (READY 前可发, 排队)
int    agentxx_cancel(a, log);                   // WireCancel (取消当前轮次)
int    agentxx_select_model(a, model_name, log); // WireSelectModel
int    agentxx_set_permission(a, path, allow, op, log); // op: 0=读 1=写
int    agentxx_switch_session(a, thread_id, log);// WireSwitchSession
char*  agentxx_get_model_info(a, log);           // 同步 (10s); 返回 JSON, free
char*  agentxx_get_context_messages(a, log);     // {"messages":[...]}
char*  agentxx_list_sessions(a, log);            // {"sessions":[...]}
```

同步查询实现: 逐类型 FIFO 等待队列 (`syncWaits_[kind]`) + promise/future;
服务端应答 (WireModelInfo/ContextMessages/SessionList) 经 `onSyncReply` 路由
完成队首等待方; 超时移除本等待器防错配。同一句柄同一时刻仅一个在途
(服务端应答为逐条协议)。

### 4.5 HIL 中断应答 (权限询问/输入收集)

链路与 TUI/CLI 完全一致:

```
服务端点 handleInterrupt → sendToPeer(WireInterruptRequest) ──▼
FfiClientAgentIO::onPeerMessage ── 记录 id, 注册 pending_, 发 EVT_INTERRUPT_REQ
        └── co_spawn(waitHostInterrupt): 挂起等待 channel
宿主 (任意线程) agentxx_interrupt_respond(a, id, values_json) ── post io 线程
        └── submitInterruptResponse(id, values) → channel 完成 → 回送
            WireInterruptResponse → 服务端点 resolveInterrupt → 轮次恢复
超时/取消: WireInterruptExpired → 关闭 channel → EVT_INTERRUPT_EXPIRED (不回送)
```

`values_json` 为 JSON 数组, 与 `inputs` 顺序对应 (bool → "true"/"false",
enum → 枚举值, string → 文本; 语义同 StdIOClientAgentIO)。应答前
`hasPendingInterrupt(id)` 校验 (id 快照互斥锁保护), 已应答/过期返回
`AGENTXX_ERR_INTERRUPT`。

## 5. 导出符号控制

### 5.1 机制 (三平台)

| 平台 | 机制 |
|---|---|
| Linux/Android (ELF) | `-Wl,--version-script=lib/ffi_symbols.map` (global 白名单 + `local: *`, 同插件构建) |
| macOS | `-exported_symbols_list` (符号带 `_` 前缀, 生成于 `lib/CMakeLists.txt`) |
| Windows/MSVC | 默认不自动导出, 仅 `AGENTXX_FFI_EXPORT` (`__declspec(dllexport)`) 标记符号导出, 天然白名单 |

`lib/CMakeLists.txt` 中 `target_link_options(agentxx_shared PRIVATE ...)` **仅
作用于共享库**, `agentxx_static` (cli/test/benchmark 与 C++ 嵌入方) 不受影响;
不设编译期 `-fvisibility=hidden` (object 库被 static/shared 复用, 保持最小
改动, version script 已足够收敛导出面)。

### 5.2 导出清单 (21 个)

```
agentxx_malloc / agentxx_free / agentxx_strdup_n     # 内存 (跨 CRT 堆边界)
agentxx_ffi_api_version / agentxx_ffi_library_version / agentxx_ffi_strerror
agentxx_create / start / stop / destroy
agentxx_send_input / cancel / select_model / set_permission / switch_session
agentxx_get_model_info / get_context_messages / list_sessions
agentxx_interrupt_respond / agentxx_drain_logs
agentxx_get_builtin_plugins                            # 既有入口 (PluginManager)
```

实测 (`nm -D --defined-only`: 173977 → **21**)。新增导出符号时同步维护
`ffi_symbols.map`、`lib/CMakeLists.txt` (darwin 列表)、`ffi_api.h`。

### 5.3 版本策略

`AGENTXX_FFI_API_VERSION` 起 1; 宿主使用前以 `agentxx_ffi_api_version()` 校验。
新增事件值/新增 JSON 字段/新增函数为非破坏性, 不递增; 删除/重命名符号或
修改参数语义时递增。

## 6. 文件清单

| 文件 | 说明 |
|---|---|
| `lib/include/agentxx/ffi_api.h` | 纯 C 头 (唯一跨版本稳定接口, 随包安装) |
| `lib/src/ffi/ffi_client_io.h/.cpp` | `FfiClientAgentIO` (client 端点: 事件→JSON 回调, 中断挂起应答, 同步应答路由) |
| `lib/src/ffi/ffi_runtime.h/.cpp` | `FfiAgentRuntime` (句柄实体: 装配/生命周期/同步查询/日志环/config 解析) |
| `lib/src/ffi/ffi_api.cpp` | extern "C" 入口 (参数校验 + catchError 兜底 + `char** log` 填充) |
| `lib/ffi_symbols.map` | ELF 导出白名单 |
| `lib/CMakeLists.txt` | `target_link_options(agentxx_shared PRIVATE ...)` + darwin 列表 |
| `test/test_ffi_c_api.h/.cpp` | 测试模块 (见 §7); `test.cpp` 注册 `ffi_c_api` |

`lib/src/ffi/` 经 `GLOB_RECURSE` 自动编入 libagentxx (shared + static), 其他
源文件无需改动; agent 核心 (BaseAgent/CodeAgent/SessionServerAgentIO/
AgentHost/wire_protocol) **零修改**。

## 7. 测试 (`agentxx_test ffi_c_api`)

| 用例 | 覆盖 |
|---|---|
| 版本/内存/错误串 | `agentxx_ffi_api_version`、strerror 映射、`strdup_n/free` 往返 (含内嵌 NUL) |
| create 错误路径 | 缺模型 / 非法 JSON / 模型 isValid 失败 → NULL + `char** log` 详情; NULL 句柄校验 |
| 生命周期+对话 | mock LLM (SSE) → create/start/EVT_READY → send_input → delta 流 (turn_start→llm→text_token→turn_end) → TURN_END 无错; 幂等 stop |
| 同步查询 | `get_model_info`/`get_context_messages` (消息数>0)/`list_sessions` (空数组) |
| HIL 权限中断 | mock 首轮返回 tool_call (`agentxx_filesystem_read` /etc/hostname) → EVT_INTERRUPT_REQ (含 permission argJson) → **独立线程**延迟应答 `["true"]` → 轮次恢复完成; 重复应答报 ERR_INTERRUPT; 无 EXPIRED |
| 取消 | 慢 LLM (1.5s) 中 `agentxx_cancel` → TURN_END `has_error=true, error_message="Cancelled by user"` |
| 日志 | `drain_logs` 返回合法 JSON 数组 (含启动/错误日志) |
| 泄漏 | ASan/LSan 全程 0 泄漏 |

## 8. 语言绑定示例 (Python ctypes)

```python
import ctypes, json

lib = ctypes.CDLL("./libagentxx.so")

# 类型/回调
PEVT = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p)
class CB(ctypes.Structure):
    _fields_ = [("on_event", PEVT), ("user_data", ctypes.c_void_p)]

events = []
def on_event(t, payload, ud):
    events.append((t, json.loads(payload.decode("utf-8")) if payload else None))
cb = CB(PEVT(on_event), None)

P = ctypes.POINTER(ctypes.c_void_p)
lib.agentxx_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(CB),
                               ctypes.POINTER(ctypes.c_char_p)]
lib.agentxx_create.restype = ctypes.c_void_p
lib.agentxx_start.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p)]
lib.agentxx_send_input.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p)]

log = ctypes.c_char_p()
h = lib.agentxx_create(None, model_json.encode(), ctypes.byref(cb), ctypes.byref(log))
assert h, log.value
assert lib.agentxx_start(h, ctypes.byref(log)) == 0
lib.agentxx_send_input(h, b"分析这个仓库", ctypes.byref(log))
time.sleep(10)  # events 里收到 EVT_DELTA(text_token) → EVT_TURN_END
lib.agentxx_stop(h, None); lib.agentxx_destroy(h, None)
```

## 9. 边界与限制

- **回调内 stop/destroy 被拒绝** (`AGENTXX_ERR_STATE`): 宿主须从自己的线程
  停止; 回调内其他 API 安全。
- **同步查询同一句柄同一时刻仅一个在途** (一问一答协议); 用后必须
  `agentxx_free` 返回值与 `*log`。
- **payload 仅回调期间有效**: 回调须同步消费或拷贝。
- 单句柄单会话驱动; 需要多个并发会话由宿主创建多个句柄 (每个独立 io 线程
  与 CodeAgent) 或使用 `agentxx_switch_session` 切换。
- 会话交互 API 在 `Starting` 态可投递 (输入先入 `inputChannel` 缓冲, `run()`
  启动后消费), 与 CLI/TUI 行为一致。
- `agentxx_get_context_messages` 返回的是 LLM 上下文 (可压缩), 非完整展示
  历史; 完整历史经 `EVT_SYNC`/`EVT_DELTA` 流式维护。