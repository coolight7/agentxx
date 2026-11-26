# libagentxx FFI C API 导出层

> 已实现并测试通过 (`agentxx_test ffi_c_api`, 451 条断言)
> 相关文档: [design](index.md) (主程序架构) · [plugins.md](plugins.md) (纯 C ABI 写法)
> 修改了 ffi 源码、设计后，应当更新当前文档，如果 ffi 接口变动，应当同步更新
> [其他编程语言导出接口](/agent/ffi/) 和 [示例](/agent/example/ffi/)

## 1. 概述与目标

为 `libagentxx_shared.so` 提供**受控且规范的 C ABI 导出面**, 供其他编程语言
(Python/Rust/Go/C#/Java/Node/Dart 等) 经 FFI (ctypes / JNA / jextract / ffigen
等) 在**嵌入方程序进程内嵌入 agent 会话运行**, 复用 agentxx 全部能力 (ReAct 循环、
工具、MCP、插件、子代理委派、会话持久化、HIL 权限/中断)。

核心规范与目标:

1. **导出符号收敛**: 共享库导出面只有 **15 个** C 符号 (`agent/lib/ffi_symbols.map`
   白名单), 其余 C++ 符号全部隐藏。**业务能力不加符号** —— 新增能力只加 op 名。
2. **提交 + 回执的调用模型**: 唯一的业务入口是 `agentxx_ffi_call(op, args)` ——
   立即返回 `reqId`, 结果经 `EVT_RESULT` 事件送达; 每个受理的 reqId **恰好一条回执**。
3. **严格 C ABI 规约**:
   - 8 字节结构体对齐: 全局 `#pragma pack(push, 8)`
   - 定长基础数据类型: 统一使用 `int32_t` / `int64_t` / `uint32_t` / `uint64_t`
   - 明确调用约定: 跨边界导出符号统一使用 `AGENTXX_FFI_CALL` (Windows 下为
     `__stdcall`, x64 Unix 下为空)
   - 结构体传参强制指针 (`const Struct*`), 结构体返回值一律改为指针出参
     (`Struct* out`) 并返回 `int32_t` 状态码
   - 字符串边界统一: 只读视图 `AgentxxStringView` 与跨堆分配字符串
     `AgentxxString` 全面替换 `const char*` / `char*`
4. **事件 payload 归回调所有**: `on_event` 收到的 `AgentxxString*` 是库分配的**一整块**
   堆内存 (结构体 + JSON 正文), 回调取完正文后用 `agentxx_ffi_free(payload)` 释放;
   结构体本身也在那块内存上, 因此回调可以安全地投递到自己的线程/事件循环
   (Dart `NativeCallable.listener`、Node threadsafe function、Python queue 等)。
5. **交互参考 client**: FFI 层实现一个自定义 `AgentIOBase` client 端点
   (`FfiClientAgentIO`), 经进程内 `ChannelAgentIOTransport` 与
   `SessionServerAgentIO` (由 BaseAgent 驱动) 通信 —— 与 TUI/CLI client
   **完全同构**, agent 核心零改动。

## 2. 总体架构与线程结构

每个 `FfiAgentRuntime` 实例内部自封闭、强隔离:

```
┌─────────────────────────────────────────────────────────────┐
│ 嵌入方程序 (Python/Rust/Go/Dart/... 经各自 FFI 绑定)              │
│   持有多个 AgentxxFFIAgent* 句柄, 注册 AgentxxFFICallbacks C 回调  │
└──────────────┬──────────────────────────────────────────────┘
               │ extern "C" API (agentxx/ffi_api.h, 纯 C)
┌──────────────▼──────────────────────────────────────────────┐
│ FfiAgentRuntime (client 线程 + server 线程 + 收尾线程)          │
│                                                             │
│  ┌─────────────────────────┐  ┌───────────────────────────┐ │
│  │ Client-IO 线程          │  │ FfiClientAgentIO          │ │
│  │ (专属 clientIoCtx_)     │◄─┤ (AgentIOBase client 端点)  │ │
│  │                         │  │  按 reqId 分流应答         │ │
│  │ - 事件分发 & C 回调     │  │  中断挂起 / 合批冲刷       │ │
│  │ - 超时检查 (共享定时器)  │  │  日志搬运 (1s pump)        │ │
│  └─────────────┬───────────┘  └─────────────┬─────────────┘ │
│                │ ChannelAgentIOTransport    │ (进程内跨线程)│
│  ┌─────────────▼────────────────────────────▼─────────────┐ │
│  │ Server-IO 线程 (专属 serverIoCtx_)                     │ │
│  │ SessionServerAgentIO + CodeAgent + AgentHost           │ │
│  │ - ReAct 循环 / LLM 流式拉取 / 工具调用 / SQLite 会话存储 │ │
│  └────────────────────────────────────────────────────────┘ │
│                                                             │
│  收尾线程 (只在 stop 时起): 等会话停 → 插件 shutdown →         │
│  停两条 io 线程 → 置 Stopped → 发 EVT_STOPPED                  │
└─────────────────────────────────────────────────────────────┘
```

启动时序 (clientEx ↔ agentEx):

1. `ChannelAgentIOTransport::makePair(clientEx, agentEx)` → 双端点 `setTransport`
2. **在 server 线程**: 先 `co_spawn(serverIO->runTransportLoop())`: init 期间客户端
   请求 (`WireHello` / `WireGetModel`) 有处理方, 不排队积压
3. **在 server 线程**: `co_spawn(runAgentMain)`: `init()` → `AgentHost::create` +
   `attachRoot` (子代理委派) → 跨线程请求组件信息 → 就绪 (`EVT_READY`) → `serverIO->run()`
4. **在 client 线程**: `co_spawn(clientIO->runTransportLoop())`: 事件分发 → C 回调
5. `sendToPeer(WireHello)` 触发尾窗同步 (`syncTailCount`) / 增量重放;
   `WireGetModel` 预取模型信息 (`EVT_MODEL_INFO`)

停止时序 (`stopInternal`, 收尾线程执行):

1. `runtime_->stop()`: 失败挂起中断 + 冲刷合批 + 关传输 + 停服务端点
2. client io 屏障 (5s 内): 把仍在等待的调用按 `canceled` 收尾
   —— 这些回执一定排在 `EVT_STOPPED` 之前
3. 等 `serverIO->running()==false` (最长 20s)
4. 摘日志 sink → `agent_->shutdownAsync` (插件关闭, 25s) → 停 server io + join
5. 停 client io + join
6. 显式释放 `clientIO_` / `runtime_` / `agent_` / `calls_`, 确保 channel 先于
   `io_context` 销毁
7. 置 `Stopped` → 发 `EVT_STOPPED` (最后一个事件)

## 3. 线程模型特性

- **双 IO 线程完全解耦**: 每个 `FfiAgentRuntime` 持有一条专属 server-io 线程与一条
  专属 client-io 线程; 收尾线程只在 stop 期间存在。
- **保护 Agent 核心**: 嵌入方程序在 `on_event` 回调里的耗时逻辑只发生在 client-io
  线程, 不会阻塞 server-io 线程的 ReAct 循环、LLM 数据接收和工具调用。
- **实例强隔离**: 多 Runtime 并发创建时, 每个 Runtime 的生命周期、事件处理和会话
  状态完全正交, 无全局锁竞争。
- **调用线程上没有任何跨线程等待**: `agentxx_ffi_call` / `call_cancel` / `stop`
  都是"受理即返回", 因此**事件回调里可以放心继续调它们** (v3 的同步查询会在这里死锁)。
- **任意线程可调用**: `call` / `call_cancel` / `stop` / `drain_logs` /
  `get_capabilities` 都可在任意线程调用; 只有 `destroy` 要求不在内部线程上
  (事件回调 / 收尾线程) 且状态是 Created / Stopped。

## 4. C API 契约 (`agent/lib/include/agentxx/ffi_api.h`)

### 4.1 核心数据结构与调用约定

```c
#pragma pack(push, 8)

// 基础字符串视图 (借用所有权, 8 字节对齐)
typedef struct AgentxxStringView {
    const char* data;
    uint64_t    size;
} AgentxxStringView;

// 跨 CRT 堆分配字符串 (显式所有权, 用后 agentxx_ffi_string_free 释放)
typedef struct AgentxxString {
    char*    data;
    uint64_t size;
} AgentxxString;

// 错误码 (int32_t)
#define AGENTXX_FFI_OK              0
#define AGENTXX_FFI_ERR_INVALID    -1   /* 参数非法 (NULL 句柄/空串/未知 op/字段类型错) */
#define AGENTXX_FFI_ERR_STATE      -2   /* 状态不允许 (未 start / 已 stop / 运行中 destroy) */
#define AGENTXX_FFI_ERR_JSON       -3   /* JSON 解析失败 */
#define AGENTXX_FFI_ERR_CONFIG     -4   /* 配置非法 (模型缺失/字段类型错误) */
#define AGENTXX_FFI_ERR_INIT       -5   /* agent init 失败 */
#define AGENTXX_FFI_ERR_INTERRUPT  -6   /* 中断 id 无效/已应答/已过期 */
#define AGENTXX_FFI_ERR_TIMEOUT    -7   /* 调用超时 (也可出现在 EVT_RESULT 里) */
#define AGENTXX_FFI_ERR_OOM        -8
#define AGENTXX_FFI_ERR_CANCELED   -9   /* 被 call_cancel 取消 / 收尾时仍未完成 */
#define AGENTXX_FFI_ERR_REJECTED  -10   /* 服务端拒绝 (error.name 给结构化原因) */
#define AGENTXX_FFI_ERR_INTERNAL  -99

// 事件回调 (struct_size 用于跨版本扩展时判定字段可见性)
typedef struct AgentxxFFICallbacks {
    uint32_t struct_size;   /* = sizeof(AgentxxFFICallbacks) */
    uint32_t reserved;      /* 必须为 0 */
    void(AGENTXX_FFI_CALL* on_event)(int32_t type, AgentxxString* payload, void* user_data);
    void* user_data;
} AgentxxFFICallbacks;
```

- `AgentxxFFICallbacks` 按值复制, 之后可以立刻释放; `struct_size` 须填
  `sizeof(AgentxxFFICallbacks)`, `reserved` 须为 0 (否则 `create` 返回 NULL)
- `on_event` 传 **NULL** = headless: 完全不做事件序列化 (也不分配 payload)

### 4.2 调用模型 (提交 + 回执 + 取消)

```c
uint64_t reqId = 0;
AgentxxString log = {NULL, 0};
AgentxxStringView req = {"{\"op\":\"model.list\",\"args\":{}}", 27};
int32_t rc = agentxx_ffi_call(agent, &req, &reqId, &log);   /* 受理即返回 */
/* ... 稍后 on_event 收到 EVT_RESULT {reqId, op, ok, result?|error?} ... */
```

- `rc == AGENTXX_FFI_OK` = **已受理**, 结果一定经 `EVT_RESULT` 到达
  (`reqId` 与之对应)
- `rc != 0` = 同步失败 (参数/状态), **不会有回执**, `log` 里是原因
- 与 `agentxx_ffi_stop` 并发时: 停止受理之后 (`stop()` 已把状态推进到 Stopping /
  收尾已清表) 提交的调用一律返回 `AGENTXX_FFI_ERR_STATE` 且无回执; 已受理的全部
  以 `canceled` 收尾 —— 受不受理只看 `rc`, 没有"受理了但没有回执"的中间态
- 同一句柄可以同时有任意多个未完成调用; 各自的回执按 `reqId` 匹配
- 需要"前一步真的生效再发下一步"时, 等前一步回执再提交下一步;
  同时提交的多个调用**不保证完成顺序**
- `timeoutMs` 缺省用 op 表里的默认值 (`0` = 不限); 超时的回执是
  `ok:false, error:{code:-7, name:"timeout"}`

取消:

```c
agentxx_ffi_call_cancel(agent, reqId, &log);   /* 任意线程, 幂等 */
```

- 只停止"这次等待", **不撤回**已经提交给服务端的操作 (要撤回输入用 `input.cancel`)
- 该调用立刻以 `code=-9 canceled` 收尾; 迟到的服务端应答被丢弃 (不会产生第二条回执)
- `reqId` 不存在 (已完成/从未发出) 返回 `OK` 并在 `log` 里说明 "already completed"

### 4.3 导出符号清单 (15 个, 白名单见 `agent/lib/ffi_symbols.map`)

| 分组 | 符号 | 说明 |
|------|------|------|
| 内存 | `agentxx_ffi_malloc` / `agentxx_ffi_free` / `agentxx_ffi_string_free` / `agentxx_ffi_strdup_n` | 跨 CRT 堆边界唯一分配与释放通道 |
| 版本 | `agentxx_ffi_api_version` / `agentxx_ffi_library_version` | API 版本校验 / 库版本字符串视图出参 |
| 错误 | `agentxx_ffi_strerror` | 错误码 → 静态字符串视图出参 |
| 能力清单 | `agentxx_ffi_get_capabilities` | `{"apiVersion","libraryVersion","ops":[...],"features":[...]}`; 传 NULL 句柄也可查询 |
| 生命周期 | `agentxx_ffi_create` / `agentxx_ffi_start` / `agentxx_ffi_stop` / `agentxx_ffi_destroy` | 创建 (不启动线程) / 异步启动 (EVT_READY) / 异步停止 (EVT_STOPPED) / 销毁 (纯释放) |
| 调用 | `agentxx_ffi_call` / `agentxx_ffi_call_cancel` | 唯一的业务入口与取消入口 |
| 日志 | `agentxx_ffi_drain_logs` | 取走积压日志 `[{"level","message"},...]` |

版本策略: 当前 `AGENTXX_FFI_API_VERSION` 为 **4**

- 1 → 2: `agentxx_ffi_send_input` 的文本参数改为输入 JSON (支持附件)
- 2 → 3: 展示历史单写者 (wire 协议 v4), delta 改为按消息 id 寻址的操作集合
- 3 → 4: **导出面收敛为"提交 + 回执"**: 35 个符号 → 15 个; 删除全部同步阻塞查询、
  `send_wire` 与 `event_queue_*`; 新增 `call` / `call_cancel` 与 `EVT_RESULT` /
  `EVT_STOPPED`; `on_event` 的 payload 改为归回调所有; `stop` 改异步、`destroy` 变纯释放

绑定加载时应当校验 `agentxx_ffi_api_version() >= AGENTXX_FFI_API_VERSION`。

### 4.4 op 表 (全部经 `agentxx_ffi_call`)

"完成"列: `ack` = 服务端通用回执; `resp:<类型>` = 对应 wire 应答; `本地` = 不进 wire。
"默认超时"是该 op 未给 `timeoutMs` 时的取值 (`0` = 不限)。会话作用域由 FFI 自动填
(args 里不写 `sessionId`, 一律用当前会话; `session.switch` 例外)。

| op | args | 完成 | 默认超时 | result / 说明 |
|---|---|---|---|---|
| `input.send` | `text` / `attachments` / `model` / `delivery` | `input_ack` | 10s | `{status, delivery, itemId?}`; `status=rejected` → `ok:false`, `error.name=<InputRejectReason>` |
| `input.cancel` | – | ack | 10s | 回执表示服务端已处理该取消请求 (`WireCancel` 只置取消标记) |
| `input.queue.clear` | – | ack | 10s | – |
| `input.queue.remove` | `itemId` | ack | 10s | 条目不存在 → `ok:false, error.name:"item_not_found"` |
| `input.queue.run_next` | – | ack | 10s | 中断当前轮次并开始队列里的下一条 |
| `model.list` | – | resp:`model_info` | 10s | `{currentModel, models[], capabilities[]}` |
| `model.select` | `name` (空 = 沿用当前) | ack | 10s | result `{model}`: 生效后的模型名; 名字不在注册表 → `error.name:"model_not_found"` |
| `model.add` | 模型配置对象 (同 `create` 的 `model_json`) | resp:`add_model_result` | 30s | 失败 `code=-4` (要写 yaml 落盘) |
| `model.remove` | `name` | resp:`remove_model_result` | 30s | 删除当前模型被拒 |
| `session.list` | `beforeMs?` / `beforeId?` / `limit?` (默认 50) / `keyword?` | resp:`session_list` | 10s | `{sessions[], totalCount, hasMore, error?}`; 取数出错 (库损坏/目录读不了) 时 `sessions` 为空且 `error` 带原因 —— **不会没有回执**, 也不会把失败当成“没有会话” |
| `session.switch` | `sessionId` | ack | 60s | 在切换**提交后**回执 (result `{sessionId}`); 历史 `EVT_SYNC` / `EVT_MODEL_INFO` / `EVT_CONTEXT_STATS` 已先于它发出 |
| `session.rename` | `title` | resp:`rename_session_result` | 10s | 空标题被拒 |
| `session.compact` | – | ack | 0 (不限) | 压缩**完成后**回执 (result `{ok}`) |
| `context.get` | `tail?` (只取最近 N 条) | resp:`context_messages` | 10s | `{messages[]}`; 不给 `tail` 为整份上下文 |
| `context.view_page` | `beforeIndex?` / `count?` | resp:`view_messages_page` | 10s | `{startIndex, totalCount, messages[]}` |
| `components.get` | – | resp:`append_component_info` | 10s | `{notifications[]}`; 启动时仍会自动拉一次 (→ `EVT_COMPONENTS`) |
| `permission.get_state` | – | resp:`permission_state` | 10s | `{fullAuth}` |
| `permission.set_full_auth` | `fullAuth` | resp:`permission_state` | 10s | 回执里是**生效后**的状态; 权限中间件缺失 → `error.name:"rejected"` |
| `fs.list_dir` | `path?` / `allowedExtensions?` | resp:`list_dir_result` | 30s | 跨设备附件选择用 |
| `interrupt.respond` | `interruptId` / `values` (对象) | ack | 10s | 中断不存在/已应答/已过期 → 同步失败 `code=-6`; 服务端也说没有 → `error.name:"interrupt_not_found"` |
| `hosttool.register` | `tools[]` (同 `tool_register` 的条目) | ack | 10s | result `{registered[], failed[{name,error}]}` |
| `hosttool.unregister` | `names[]` (或单个 `name`) | ack | 10s | result `{removed[]}` |
| `hosttool.respond` | `callId` / `ok` / `result` 或 `error` | ack | 10s | callId 未知 → `error.name:"call_not_found"` |
| `plugin.data_up` | `topic` / `data` | ack | 10s | 回执表示已发布到 agent 总线 |
| `delta.set_batch` | `maxDelayMs` | 本地 | – | `{maxDelayMs}`; `0` 关闭合批 |
| `language.get` | – | 本地 | – | `{language}` |
| `language.set` | `language` | 本地 | – | `{language}` (规范化后); 投递到 server io 设会话语言 |

`resp:*` / `ack` 类 op 的回执**只发一条 `EVT_RESULT`**, 不再另外发 `EVT_MODEL_INFO`
之类的广播事件; 只有 `reqId == 0` 的下行应答 (TUI 等其它客户端的请求、服务端主动
广播) 才走广播事件。

### 4.5 事件类型 (`AgentxxFFIEventType`)

payload 均为 `AgentxxString*` JSON, **归回调所有**: 结构体与正文是一块堆内存,
取完正文后用 `agentxx_ffi_free(payload)` 释放 (不是 `agentxx_ffi_string_free`)。

| 事件 | payload | 说明 |
|------|---------|------|
| `EVT_READY` | `{"sessionId"}` | 服务端就绪, 可开始提交调用与输入 |
| `EVT_STOPPED` | `{}` | 收尾完成 —— **最后一个事件**, 之后不会再有回调 |
| `EVT_RESULT` | `{reqId, op, ok, result?}` 或 `{reqId, op, ok:false, error:{code,name,message}}` | 调用回执 (每个受理的 reqId 恰好一条) |
| `EVT_ERROR` | `{code, name?, message}` | 与具体调用无关的内部错误 |
| `EVT_SYNC` | wire sync JSON | 完整/尾窗历史同步 (`fromIndex` / `totalMessages`) |
| `EVT_DELTA` | wire delta JSON | 流式增量: 消息级 op (stream_begin/text_token/thinking_token/insert_message/update_message/stream_end, 均带 `msgId`) + 界面状态类 op; 工具就是普通历史行 |
| `EVT_DELTA_TEXT_BATCH` | `{"msgId","kind","text"}` | 合批后的文本/思考增量 (`delta.set_batch(>0)` 时才产出) |
| `EVT_TURN_END` | wire turn_result JSON | 轮次结束 (`hasError` 报告异步错误) |
| `EVT_CONTEXT_STATS` | wire context_stats JSON | 上下文 token 统计 + 会话累计用量 (`promptTokens`/`completionTokens`/`totalTokens`/`cachedPromptTokens`) + 会话标题 (`sessionTitle`) |
| `EVT_MODEL_INFO` | wire model_info JSON | 模型状态**广播** (切换会话/增删模型后); 广播的 `capabilities` 给出各模型多模态输入能力 |
| `EVT_COMPONENTS` | wire append_component_info JSON | 启动组件 (MCP/Skill/Memory/插件) 加载信息 |
| `EVT_MESSAGE_QUEUE` | wire message_queue_update JSON | 待发队列变化 `{sessionId, items[], state?}` |
| `EVT_PERMISSION_STATE` | `{"fullAuth"}` | 完全授权状态**广播** |
| `EVT_INTERRUPT_REQ` | `{"interruptId","sessionId","node","value","argJson"}` | HIL 中断询问; `argJson` 为 `InterruptHandleArg` (`{name,arg,resultId,ui}`), 渲染指引见 4.7 |
| `EVT_INTERRUPT_EXPIRED` | `{"interruptId"}` | 中断已过期/取消, 不再可应答 |
| `EVT_HOST_TOOL_CALL` | `{"callId","name","argsJson","sessionId","timeoutSec"}` | 服务端请求嵌入方程序执行已注册的嵌入方工具 (见 4.8) |
| `EVT_HOST_TOOL_CANCELED` | `{"callId","reason"}` | 嵌入方工具调用被取消 (停止/销毁时未应答的调用统一上报) |
| `EVT_PLUGIN_DATA` | wire plugin_data JSON | agent 侧插件事件转发 (`{topic,data}`) |

### 4.6 配置与模型 JSON (`agentxx_ffi_create` 参数)

```c
// config_json (可 NULL; 未知字段忽略):
{ "dataDir": "~/.agentxx",            // 空=不持久化 (默认)
  "workDir": "/abs/project/dir",      // 会话工作目录; 空=进程 cwd (默认)。
                                      // 生效范围: permission Ask 放行范围 /
                                      // filesystem 与权限校验相对路径基准 /
                                      // 命令执行子进程初始目录 / 插件 projectRoot。
                                      // 支持 ~/相对路径 (按进程 cwd 展开为绝对);
                                      // 嵌入多实例 (多句柄) 各自绑定独立项目目录
  "enableSessionStore": false,
  "sessionStoreDirectory": "",        // 为空时使用 {dataDir}/sqlite/sessions/
  "syncTailCount": 200,               // 首屏只同步最近 N 条展示消息 (0 = 完整同步;
                                      // 更早的历史用 context.view_page 按页拉)
  "permissionMode": "ask",            // ask|all_ask|pass|deny (yaml permission.mode 同值;
                                      // ask=工作目录内 ALLOW 其余 INTERRUPT)
  "permissionAllowPaths": ["..."],    // 权限白名单
  "permissionDenyPaths": ["..."],     // 权限黑名单
  "skills": ["..."],                  // 技能目录列表
  "memoryFiles": ["..."],             // 上下文文件列表
  "mcpServers": {"ns": {"url": "...", "timeoutSec": 120}},
  "plugins": [{"path":"...", "enabled":true, "sides":"agent|client|auto", "args":{}}],
  "llmMaxRetry": 5,
  "agentName": "Agentxx",
  "language": "zh-cn",                // 界面/会话语言 (缺省 en)
  "devMode": false,
  "interruptTimeoutSec": 0 }          // HIL 等待嵌入方程序应答超时, 0=不限 (默认)

// model_json (建议必填; isValid: baseUrl 非空 或 apiKey != "EMPTY"):
{ "name": "显示名", "type": "openai|anthropic|openai-responses",
  "baseUrl": "...", "apiKey": "...", "modelName": "(请求 model 字段)",
  "apiPath": "", "connectTimeoutSeconds": 16, "readChunkTimeoutSeconds": 100,
  "sslVerify": true|null, "maxConcurrentConnections": 5,
  "anthropicVersion": "2023-06-01", "modelContextMaxToken": 0,
  "imageInput": false, "audioInput": false, "videoInput": false,
  // ↑ 多模态输入能力 (对齐 yaml 的 image_input/audio_input/video_input,
  //   字段名兼容下划线写法): 经 model.list / EVT_MODEL_INFO 的 capabilities
  //   下发给嵌入方程序, 供其决定是否展示图片/音频/视频输入入口
  "extraHeaders": {"k":"v"}, "extraConfig": {} }
```

### 4.7 中断 UI 描述渲染指引 (嵌入方程序 GUI 零语义渲染)

`EVT_INTERRUPT_REQ.argJson.ui` 是**自包含的表单描述**: 嵌入方程序不需要了解任何具体
询问类型 (权限确认/repeat 提醒/未来新增), 按下面的块类型表依次渲染即可;
应答经 `interrupt.respond` 回传 `{"interruptId":N,"values":{"<控件 id>": 值}}`
(FFI 侧负责包成服务端要的 `{"values": {...}}`)。

> `version` 当前为 1 (schema 首版)。**不保留旧版本兼容**: 描述缺失/非法按契约
> 错误处理 (输出提示且不可交互); 未识别的块类型/字段忽略或降级为 `fallback`
> 文本 (向前兼容, 不要报错)。

**结构**

```
ui = { "version": 1,
       "header": { "segments": [ {"text","labelKey","color","bold","dim"}, ... ] },  // 可空 = 默认前缀
       "blocks": [ ... ] }                                    // 按顺序渲染 (内容块与控件块混排)
```

**内容块 (`blocks[].kind`)**

| kind | 字段 | 渲染 |
|------|------|------|
| `text` | `text`/`textKey`, `color`, `bold`, `dim`, `wrap`, `indent` | 文本行 (wrap=按宽度硬折行; 空文本不渲染) |
| `markdown` | `text`, `indent` | markdown 富文本 (无 markdown 能力的嵌入方程序可直接打印原文) |
| `diff` | `path`, `oldStr`, `newStr` | 差异对比 (可降级为统一 diff 文本) |
| `image` | `dataUrl` 或 `path`, `alt`, `maxHeight` | 图片预览 (无图嵌入方程序降级为 `alt` 文本行) |
| `progress` | `value`, `total`, `label`/`labelKey`, `text`/`textKey` | 进度条 (`total` 为 0 = 不确定态; 可降级为 `value/total` 文本) |
| `separator` | `indent` | 分隔线 |
| `gap` | `lines` | 空行 |
| `custom` | `component`, `props`, `fallback` | **预留字段**: 客户端自定义渲染组件; 无该组件时渲染 `fallback` 文本 |

**控件块 (`blocks[].kind == "control"`)**

| `control` | 字段 | 渲染 | 结果值 |
|-----------|------|------|--------|
| `buttons` | `options[{value,label,labelKey,color}]`, `defaultValue`, `commitOnPick` | 横排按钮; `commitOnPick=true` 点击即选中并提交整份表单 | 选中项 `value` (原始 JSON) |
| `select` | `options`, `defaultValue` | 竖排单选列表 | 选中项 `value` |
| `checkbox` | `label`/`labelKey`, `defaultValue`(bool) | 勾选行 | 布尔 |
| `text` | `label`/`labelKey`, `help`/`helpKey`, `defaultValue`, `multiline`(预留) | 文本输入框 | 字符串 |
| `number` | `defaultValue`, `integer`, `min`, `max`, `step` | 数值控件 | 数值 (`integer=true` 时整数) |
| `path` | `label`/`labelKey`, `mode`(`file`/`dir`/`multi`), `filter`, `defaultValue` | 路径选择 (无选择器时降级为文本输入) | 路径字符串 (多选为数组) |

通用控件字段: `id` (结果键, 同一描述内唯一; 缺省 "value")、`label`/`labelKey`、
`help`/`helpKey`、`indent`。

**提交行 (`blocks[].kind == "submit"`**

`label`/`labelKey` = 确认按钮 (缺省 "确认"), `cancelLabel`/`cancelLabelKey` = 取消。
点击确认 = 提交整份表单; 点击取消 = 取消整个中断请求。

- `color` 取值 `error`/`accent`/`hint`/`normal`/`thinking`/`tool`, 嵌入方程序按主题映射
- 结果恒为对象 `{"values": {"<控件 id>": 值}}`; **空对象 = 未提交/取消** (按未应答处理)
- 内容块与控件块可任意混排; 未知块类型忽略

**示例 (权限询问的应答, 含勾选项)**

```jsonc
// EVT_INTERRUPT_REQ.argJson.ui (节选; 由 preset::permissionCard 生成)
// 固定文案只带 i18n 键 (text/label/help 为空, 见下"文案约定"), 故示例中无字面文本
{ "version": 1,
  "header": { "segments": [ {"labelKey":"interrupt.permissionBadge",
                             "color":"error","bold":true},
                            {"text":"read_file","color":"accent","bold":true},
                            {"text":" filesystem_read","color":"hint"} ] },
  "blocks": [ {"kind":"text","text":"• /workspace/data/x.txt","color":"hint","wrap":true,"indent":2},
              {"kind":"gap"},
              {"kind":"control","id":"remember","control":"checkbox",
               "labelKey":"interrupt.remember","defaultValue":false},
              {"kind":"control","id":"fullAuth","control":"checkbox",
               "labelKey":"interrupt.fullAuth","defaultValue":false},
              {"kind":"gap"},
              {"kind":"control","id":"decision","control":"buttons",
               "options":[{"value":"true","labelKey":"interrupt.allow"},
                          {"value":"false","labelKey":"interrupt.deny",
                           "color":"error"}],
               "defaultValue":"false","commitOnPick":true} ] }

// 嵌入方程序应答 (允许 + 记住本次选择):
{"op":"interrupt.respond","args":{"interruptId":1,
  "values":{"decision":true,"remember":true}}}
```

> **文案约定**: agent 侧内置预设模板生成的固定文案**只声明 i18n 键**
> (`labelKey`/`helpKey`/`textKey`), 字面文本留空 —— 文案由客户端词表提供;
> 生产者自定义的文本没有对应键, 仍按字面文本渲染 (客户端缺键时回退字面文本)。
> 嵌入方程序未实现词表时可直接用键名或自备映射, 键名为稳定契约。

> `remember: true` 表示"记住本次选择": agent 侧权限中间件按本次目标路径注册
> 允许/拒绝规则, 后续同目标访问直接按规则处理, 不再询问; 目标为目录时
> (描述内目标路径带尾斜杠) 点击授权或完全授权均表示同时授权其全部子目录与文件
> —— 此时在目标描述行后附加 `textKey = "interrupt.rememberDir"` 的生效范围提示行

**示例 (多控件表单 + 提交行)**

```jsonc
// 控件 id: path / count / force
{"values":{"path":"/tmp/a.txt","count":4,"force":true}}
```

伪代码 (任意语言): 遍历 `blocks`, 内容块直接渲染, `control` 块按 `control` 字段渲染
控件并维护 `id → 值` map; 点击 `submit` 的确认 (或 `commitOnPick` 按钮) 时把 map 拼成
`{"interruptId":N,"values":{...}}` 交给 `agentxx_ffi_call`; 取消时传 `{"values":{}}`。

> 参考实现: 控制台嵌入方程序见 `agent/example/ffi/dart/` (按 `control` 块逐项问答的最小形态);
> 完整描述驱动渲染见 TUI 的 `InterruptView` (`agent/client/.../components/interrupt_view.cpp`)。

### 4.8 生命周期与嵌入方工具

**生命周期状态机**

```
Created ──start()──► Starting ──就绪──► Ready ──stop()──► Stopping ──收尾完──► Stopped
                        └──init 失败──► Failed ──stop()──► Stopping
```

- `create`: 任意线程, 同步完成 (解析配置 + 构造 agent, 不启动线程)
- `start`: 受理即返回; 就绪发 `EVT_READY`, 失败发 `EVT_ERROR`
- `stop`: 受理即返回 (任意线程, 含事件回调内), 收尾完成发 `EVT_STOPPED`;
  **幂等** (重复调用只发一次); Starting / Ready 期间提交的 `call` 照常受理,
  Stopping 之后返回 `ERR_STATE`
- `destroy`: **只在 Created / Stopped 成功**, 纯释放不再阻塞收尾; 其余状态返回
  `ERR_STATE` 并提示"先 stop 并等 EVT_STOPPED"; 在内部线程 (事件回调 / 收尾线程)
  上调用同样被拒绝。收尾期间未完成的调用会以 `canceled` 收尾 (排在 `EVT_STOPPED` 之前)

**嵌入方程序提供的工具**

```
hosttool.register {tools:[...]}        → 服务端注册进 ToolRegistry
input.send(...)                        → 模型决定调用嵌入方工具
EVT_HOST_TOOL_CALL {callId,name,argsJson,sessionId,timeoutSec}
hosttool.respond {callId,ok,result}    → 回送结果 (服务端协程继续本轮)
```

- 每个工具需 `name` (必填), 可选 `description` / `inputSchema` (JSON Schema 对象) /
  `timeoutSec` (0 = 不限) / `maxConcurrent`; 回执 `{registered[], failed[{name,error}]}`
- 同名工具重复注册会先注销旧的再注册
- 超过 `timeoutSec` 由服务端判定超时并让本轮继续 (工具结果记为失败), 不会永久挂住会话
- 客户端断开时该客户端注册的嵌入方工具自动注销; `stop` / `destroy` 时未应答的调用统一
  上报 `EVT_HOST_TOOL_CANCELED {callId, reason}`
- 嵌入方程序工具在 `ToolRegistry` 里带 `HostTool` 包装, 调用时经 `callHostToolAsync`
  向注册该工具的客户端发 `host_tool_call` 并等应答

### 4.9 文本增量合批 (`delta.set_batch`)

本地路径唯一高频跨语言边界的是逐 token 的文本增量。`delta.set_batch(maxDelayMs)`
把同一轮次同一流里的文本/思考 token 累积到窗口结束再发一条
`EVT_DELTA_TEXT_BATCH {msgId, kind, text}`:

```
delta.set_batch {maxDelayMs: 24}      → 打开合批 (建议 16~33ms)
EVT_DELTA_TEXT_BATCH {kind,text} × N  → 嵌入方程序直接追加文本
delta.set_batch {maxDelayMs: 0}       → 关闭, 逐条事件 (默认关闭)
```

- **默认关闭**: 合批会减少跨边界事件数 (SDK 只在推送大流量时打开),
  但文本会按窗口成块到达 —— 需要"回执之前的增量都已到齐"这种强顺序时不要打开
- 合批**不改变事件顺序语义**: 工具开始/结束、轮次结束、中断必须单独成条且严格有序
  —— 这些事件到达时先冲刷积压文本, 再按原顺序发出
- 思考内容与正文分开合批 (`kind: "thinking"` / `"text"`)
- 轮次结束/停止/销毁时冲刷积压文本, 不丢尾部内容

### 4.10 用户输入 (`input.send` 的 args)

```json
{ "text": "看一下这张图",                 // 与 attachments 至少一个非空
  "attachments": [                        // 可选; 单条消息 ≤ 5 个
    { "type": "image",                    // image|audio|video (别名 kind)
      "display_name": "diagram.png",      // 别名 name
      "mime_type": "image/png",           // 别名 mimeType
      "data_url": "data:image/png;base64,...",  // 别名 dataUrl; 小文件直接内嵌
      "path_or_url": "/abs/photo.png",    // 别名 path; 与 data_url 二选一
      "size_bytes": 68 }                  // 别名 size
  ],
  "model": "m2",                          // 可选; 空 = 不切换模型
  "delivery": "next-turn" }               // 可选; 空 = next-turn
```

限额与拒绝 (与 wire 侧同一套约定; 超限是**同步失败** `AGENTXX_FFI_ERR_INVALID`,
`log` 写明是哪一项):

| 项 | 上限 | 说明 |
| --- | --- | --- |
| 单条消息附件数 | 5 | 超限即拒绝, 不进入轮次 |
| 图片 | 10 MB | 按 `size_bytes` 判定; 缺失时按 `data_url` 的 Base64 长度估算, 再退到本地文件大小 |
| 音频 | 25 MB | 同上 |
| 视频 | 50 MB | 同上 |

- `data_url` 为空且 `path_or_url` 为本机路径时, **服务端读取并编码**, 嵌入方程序不必
  自己读文件 (跨设备场景先经 `fs.list_dir` 选服务端文件, 只传路径即可)
- 附件最终按类型汇入 `ChatMessage` 的 `image_urls` / `audio_urls` / `video_urls`
- 受理结果即 `input.send` 的回执: `{status, delivery, itemId?}`, `status` 取
  `started|queued|steered|rejected`; 拒绝原因见 `InputRejectReason`
  (`empty_content` / `attachment_too_large` / `too_many_attachments` /
  `session_mismatch` / `server_stopped` / `bad_delivery` / `queue_cleared`)
- 带附件的输入按 **next-turn 排队**(附件需要服务端加载编码, 只在轮次起始处理)
- 模型能力: 先从 `model.list` / `EVT_MODEL_INFO` 的 `capabilities` 读到当前模型是否
  支持图片/音频/视频输入, 再决定是否给出附件入口; **服务端还有一道兜底**: 模型未声明
  的类型不会投给模型 (正文里补一行「[附件未传给模型] …」并保留文件名/路径)

### 4.11 顺序、超时与取消语义

- **顺序**: 同时提交的多个调用完成顺序不确定 (服务端按各自处理速度回执)。
  需要顺序就 `await` 前一条回执再发下一条 —— 这是唯一可靠的顺序手段。
  `input.send` 的文本按提交顺序入队, 与回执到达顺序无关。
- **超时**: 每条调用的初始时限 = `timeoutMs` (缺省用 op 表); 到点后 FFI 自己回一条
  `code=-7 timeout` 回执并忘掉该 reqId, 之后到达的服务端应答被丢弃。超时**不撤回**
  已提交的操作 (输入仍会执行, 只是没人等它的结果)。
- **取消**: `call_cancel` 只结束等待, 与超时同一条路径; 因此"先超时后取消"或
  "先取消后超时"都只会产生一条回执。
- **收尾**: `stop` 期间仍未完成的调用由 FFI 统一以 `canceled` 收尾 (排在
  `EVT_STOPPED` 之前), 因此 `EVT_STOPPED` 之后一定没有待完成的调用。
- **回调内调用**: 回调 (client io 线程) 里 `call` / `call_cancel` / `stop` /
  `drain_logs` 都是安全的 (没有任何跨线程等待); 但**不要**在回调里长时间阻塞,
  它挡着所有事件的分发。

### 4.12 性能默认值与建议

- `syncTailCount` 默认 200: 首屏只同步最近 200 条展示消息, 更早的用
  `context.view_page` 按页拉 (设为 0 = 完整同步, 长会话会显著变慢/变大)
- 合批默认关闭: 逐 token 事件在跨语言边界上有固定成本, 高频流式输出时用
  `delta.set_batch {maxDelayMs: 24}` 打开
- headless (`callbacks == NULL`) 完全不做事件序列化; 只需要会话结果、不需要界面的
  嵌入方程序应当用这种形态
- 每个实例的常驻线程: client io + server io (收尾期间多一条); 日志经 client io
  每秒 `pump()` 一次搬运, 不再单独起线程

## 5. 语言绑定与示例

| 目录 | 说明 |
|------|------|
| [`agent/ffi/dart/`](/agent/ffi/dart/) | Dart FFI 绑定包: `ffigen.yaml` 由 `ffi_api.h` 自动生成符号定义 (`dart run ffigen --config ffigen.yaml`, 输出 `lib/agentxx_ffi_bindings.dart`) |
| [`agent/ffi/dart_wire/`](/agent/ffi/dart_wire/) | 纯 Dart wire 客户端包: 全消息模型与 JSON 编解码 + WebSocket 连接管理 (握手/心跳/事件流/嵌入方工具应答), 不依赖 FFI 与原生库 |
| [`agent/example/ffi/dart/`](/agent/example/ffi/dart/) | Dart CLI 示例 (`agentxx_dart_cli`): 提交 + `Future` 回执封装 / 取消令牌 / 事件流渲染 / HIL 权限交互 / `/model` `/sessions` `/compact` `/batch` 等命令; 含 mock LLM 基本功能检查 (`example/smoke_check.dart`); 详见其 README |

Dart SDK 的用法 (v4 形态):

```dart
final client = AgentClient();
client.events.listen((e) { /* ReadyEvent / ResultEvent / DeltaEvent / ... */ });
await client.start(configJson: cfg, modelJson: model, syncTailCount: 200);
await client.ready;

await client.sendInput('你好');                       // 提交 + 等受理回执
final models = await client.listModels();              // 提交 + 等结果
await client.enableDeltaBatch(maxDelayMs: 24);         // 本地 op
final token = AgentxxCancelToken();
final pending = client.call('session.compact', cancel: token);
token.cancel();                                        // → AgentxxCanceledException

await client.stop();                                   // 等 EVT_STOPPED
await client.dispose();                                // destroy + 释放回调
```

其他语言按同样模式接入: 经 dlopen/dlsym (或平台等价物) 查找白名单符号, 注册
`AgentxxFFICallbacks` 回调; 回调内立刻释放 payload (或把内容拷走), 再按 reqId 把
`EVT_RESULT` 还原成各自的 Future/Promise。**同步等待原生调用返回结果是不可能的** ——
所有等待都在嵌入方程序自己的事件循环里完成。

## 6. 测试

- `agent/test/test_ffi_c_api.cpp` (模块名 `ffi_c_api`, 451 条断言): 生命周期 / 17 个
  用例, 含并发 20 调用、取消与迟到应答、超时、顺序、回调内调用、事件所有权、
  多句柄并发、headless; 内置 mock LLM Server 全流程验证
- 基准: `agentxx_benchmark resource_ffi` 场景 (调用吞吐 / 合批对比 / 首屏尾窗)

---

## 7. 实现细节补充

- **Channel 直连**: FFI 层复用 `ChannelAgentIOTransport::makePair` (零序列化
  concurrent_channel), 非 WS; `SessionServerAgentIO` 视为服务端端点,
  `FfiClientAgentIO` 为 client 端点, 二者完全同构于 TUI/CLI 的 transport 抽象
- **工作目录回退**: `config_json.workDir` 支持 `~`/`${VAR}` 展开与相对路径;
  未配置时回退进程 `cwd`; 会话级 worktree 绑定 (`Session::WorktreeBinding`) 与
  `AgentContext::getSessionWorkDir` 的多源回退对 FFI 句柄同样生效
- **权限 sides**: `plugins[].sides` 取值 `auto` (默认, 按导出符号
  `agentxx_client_plugin_create` 自动决定) / `agent` / `client`
  (FFI 场景通常为 agent)
- **调用表**: `CallRegistry` 记 `reqId → (op, 到期时刻)`; 超时靠**一个共享
  `steady_timer`** + 按到期时刻排序的索引 (不是每调用一条定时器);
  `settle` 用 CAS 保证"恰好一条回执", 谁先到谁赢
- **回执投递**: 已在 client io 线程 (服务端应答 / 超时 / 收尾) 就地发;
  其余线程 (`call_cancel` / 本地 op) `asio::post` 过去 —— 这样"取消/收尾产生的
  回执排在 `EVT_STOPPED` 之前"不需要额外同步
- **`input.send` 的 wire `requestId` 直接用 FFI 自己的 reqId**, 服务端 `input_ack`
  按它匹配, 不需要第二张映射表
- **HIL 输入描述**: `EVT_INTERRUPT_REQ` 的 `argJson` 为 `InterruptHandleArg`
  序列化 (`{name,arg,resultId,ui}`); `ui` 为必填的中断 UI 描述
  (schema 见 `agent/middlewares/interrupt_ui.h`), 渲染指引见 4.7
- **跨 CRT 堆**: 所有 `AgentxxString` 出参与事件 payload 均经 `agentxx_ffi_malloc`
  分配, 用后 `agentxx_ffi_string_free`; `agentxx_ffi_strdup_n` 为统一拷贝入口

---

## 8. 与 `musicxx_extern_plugin` 共用的口径

四条约定由 musicxx 仓库的 `docs/history/plugin-framework-compare/plan.md` 定稿，两边文档互相引用；
通用实现下沉在子模块 `cxx_pluginxx`（`host/ffi_bridge.h`、`api/ui.h`、`host/ui_content.h`、
`host/ui_slots.h`、`host/kind_registry.h`、`host/manifest_targets.h`）。

### 8.1 事件 `type` 与 `seq`

`AgentxxFFIEventType` 枚举保留（现有绑定不破），但**新事件的判定以 payload 里的 `type` 字符串为准**：
宿主按名字路由，不认识的忽略；`seq` 单调递增，用来发现丢事件。老宿主不认识新事件也不会崩。

### 8.2 超时与取消

- 超时归调用方：`agentxx_ffi_call(..., timeoutMs <= 0)` = 不限，op 表的默认值只是默认；
  到点由 `CallRegistry` 产生一条 `timeout` 回执。
- `agentxx_ffi_call_cancel(reqId)` 立刻以 `canceled` 收尾，晚到应答丢弃；`reqId` 不存在返回 OK（幂等）。
- 停机时先结算全部未完成请求（`canceled`），**然后**才发 `EVT_STOPPED`。
- 不变量：受理成功 ⇒ 恰好一条回执；不受理 ⇒ 不推回执；事件队列满要显式回 `busy`，不静默丢。

### 8.3 日志英文

运行期字符串（日志、错误原因、事件里的 `message` / `reason`）一律英文；中文只在代码注释与本地化表里。
检查脚本 `agent/script/check_log_language.sh`。

### 8.4 界面

client 侧界面改成“注册声明项 + 注册内容生成函数”，渲染线程只读缓存；块树 / 槽位契约与 musicxx 共用
（`cxx_pluginxx/api/ui.h`、`host/ui_content.h`、`host/ui_slots.h`）。渲染器各写各的（FTXUI / Flutter）。
