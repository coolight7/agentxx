# libagentxx FFI C API 导出层

> 已实现并测试通过 (`agentxx_test ffi_c_api`: 78/78)
> 相关文档: [design.md](design.md) (主程序架构) · [plugins.md](plugins.md) (纯 C ABI 插件范式)
- 修改了 ffi 源码、设计后，应当更新当前文档，如果ffi接口变动，应当同步更新 [其他编程语言导出接口](/agent/ffi/) 和 [示例](/agent/example/ffi/)

## 1. 概述与目标

为 `libagentxx_shared.so` 提供**受控的 C ABI 导出面**, 供其他编程语言
(Python/Rust/Go/C#/Java/Node 等) 经 FFI (ctypes / JNA / jextract / wasm-abi
等) 在**宿主进程内嵌入 agent 会话运行**, 复用 agentxx 全部能力 (ReAct 循环、
工具、MCP、插件、子代理委派、会话持久化、HIL 权限/中断)。

三个核心目标:

1. **导出符号收敛**: 共享库导出面从 ~17 万 C++ 符号 (默认全导出) 收敛为
   仅 25 个顶层 C 符号, 满足 "导出符号必须是 C Api" 的硬约束 (libstdc++
   等运行库依赖仍为 DT_NEEDED, 属正常动态依赖)。
2. **交互参考 client**: FFI 层实现一个自定义 `AgentIOBase` client 端点
   (`FfiClientAgentIO`), 经进程内 `ChannelAgentIOTransport` 与
   `SessionServerAgentIO` (由 BaseAgent 驱动) 通信——与 TUI/CLI client
   **完全同构**, agent 核心零改动。
3. **错误处理双通道**: 同步错误 = 返回值错误码 + 可选的 `char** log` 参数
   (内部填充执行过程日志/错误详情, 宿主 `agentxx_free` 释放); 异步错误 =
   `EVT_ERROR` / `EVT_TURN_END {has_error}` 事件。

## 2. 总体架构与线程拓扑 (方案 A: 独立双线程模型)

每个 `FfiAgentRuntime` 实例内部完全自封闭、强隔离，配备独立的两个 IO 线程：

```
┌─────────────────────────────────────────────────────────────┐
│ 宿主程序 (Python/Rust/Go/... 经各自 FFI 绑定)                  │
│   持有多个 AgentxxAgent* 句柄, 注册 AgentxxCallbacks C 回调    │
└──────────────┬──────────────────────────────────────────────┘
               │ extern "C" API (agentxx/ffi_api.h, 纯 C)
┌──────────────▼──────────────────────────────────────────────┐
│ FfiAgentRuntime (每个实例独占 1 个 Client 线程 + 1 个 Agent 线程)│
│                                                             │
│  ┌─────────────────────────┐  ┌───────────────────────────┐ │
│  │ Client-IO 线程          │  │ FfiClientAgentIO          │ │
│  │ (专属 clientIoCtx_)     │◄─┤ (AgentIOBase client 端点)  │ │
│  │                         │  │  onDelta→JSON→on_event    │ │
│  │ - 运行 client 接收循环  │  │  WireInterruptRequest 挂起 │ │
│  │ - 事件分发 & C 回调     │  │  同步查询 promise 路由    │ │
│  └─────────────┬───────────┘  └─────────────┬─────────────┘ │
│                │ ChannelAgentIOTransport    │ (进程内跨线程)│
│  ┌─────────────▼────────────────────────────▼─────────────┐ │
│  │ Agent-IO 线程 (专属 agentIoCtx_)                        │ │
│  │ SessionServerAgentIO + CodeAgent + AgentHost           │ │
│  │ - ReAct 循环 / LLM 流式拉取 / 工具调用 / SQLite 会话存储 │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

装配时序 (双线程协同, clientEx ↔ agentEx):

1. `ChannelAgentIOTransport::makePair(clientEx, agentEx)` → 双端点 `setTransport`
2. **在 agent 线程**: 先 `co_spawn(serverIO->runTransportLoop())`: init 期间客户端请求
   (`WireHello`/`WireGetModel`) 有消费方, 不排队积压
3. **在 agent 线程**: `co_spawn(runAgentMain)`: `init()` → `AgentHost::create+attachRoot`
   (子代理委派) → 跨线程请求组件信息 → `notifyServerReady` (触发 `EVT_READY`) → `serverIO->run()`
   (会话驱动循环)
4. **在 client 线程**: `co_spawn(clientIO->runTransportLoop())`: 事件分发 → C 回调
5. `sendToPeer(WireHello)` 触发全量 Sync/增量重放; `WireGetModel` 预取模型信息

安全停止 (`stopInternal`, 调用方线程执行):

1. `clientIO->failAllPendingInterrupts()` (失败挂起中断, 等待协程结束)
2. `clientIO->transport()->close()` (client 接收循环结束)
3. `serverIO->stop()` (dispatch 到 agent io 线程 `stopImpl`: 关输入 channel/
   取消轮次/失败 server 侧 pending) → `run()` 循环退出
4. 轮询 `serverIO->running()==false` (最长 20s)
5. `removeSink` → 停止并 join `agentThread_` → 停止并 join `clientThread_`
6. 显式释放 `clientIO_`, `serverIO_`, `agent_` 等对象，确保 channel 先于 `io_context` 销毁

## 3. 线程模型特性

- **双线程完全解耦**: 每个 `FfiAgentRuntime` 持有一条专属的 `agent-io` 线程和一条专属的 `client-io` 线程。
- **保护 Agent 核心**: 宿主在 `on_event` 事件回调中的任何耗时逻辑仅发生在 `client-io` 线程，绝对不会阻塞 `agent-io` 线程的 ReAct 循环、LLM 数据接收和工具调用。
- **实例强隔离**: 多 Runtime 并发创建时，每个 Runtime 的生命周期、事件处理和会话状态完全正交，无全局锁竞争。
- **C API 任意线程可调用**: 会话交互类经 `asio::post` 投递 `client-io` 线程串行执行; 同步查询类经 promise/future 等待 (最长 10s)。
- **约束**: `agentxx_stop`/`agentxx_destroy` 不得在内部 io 线程 (即 client/agent 回调线程) 内调用, 返回 `AGENTXX_ERR_STATE` (避免自 join 死锁); 宿主需从自己的线程调用。

## 4. C API 契约 (`agent/lib/include/agentxx/ffi_api.h`)

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

### 4.2 异步安全事件队列 (供 Dart/ctypes 等无法同步拷贝 payload 的宿主)

`on_event` 在内部 client-io 线程同步回调, payload 仅回调期间有效 —— 宿主若
把回调转发到其他线程/事件循环 (如 Dart `NativeCallable.listener`、Python
queue 转发), 回调返回后 payload 即失效。此类宿主改用内置队列桥接:

```c
AgentxxEventQueue* q = agentxx_event_queue_create();
AgentxxCallbacks cb;
cb.on_event  = agentxx_event_queue_on_event; /* 内置桥接: 同步拷贝入队 */
cb.user_data = q;
/* ... agentxx_create/start 后, 宿主线程序列化取事件: */
int32_t type; char* json;
while ((rc = agentxx_event_queue_pop(q, &type, &json, 50)) == AGENTXX_OK) {
    /* 消费 type/json; 用后 agentxx_free(json) */
}
agentxx_event_queue_free(q);
```

- `agentxx_event_queue_pop`: 阻塞至多 timeout_ms; 空/超时返回
  `AGENTXX_ERR_TIMEOUT`; 队列已销毁返回 `AGENTXX_ERR_STATE`
- 队列有界 (16384): 宿主停轮询时丢最旧并补发一条 EVT_ERROR 提示
- 实现: `agent/lib/src/ffi/event_queue.cpp`

### 4.3 导出符号清单 (25 个, 白名单见 `agent/lib/ffi_symbols.map`)

| 分组 | 符号 | 说明 |
|------|------|------|
| 内存 | `agentxx_malloc` / `agentxx_free` / `agentxx_strdup_n` | 跨 CRT 堆边界唯一分配通道 |
| 版本 | `agentxx_ffi_api_version` / `agentxx_ffi_library_version` | API 版本校验 / 库版本字符串 |
| 错误 | `agentxx_ffi_strerror` | 错误码 → 静态字符串 |
| 生命周期 | `agentxx_create` / `agentxx_start` / `agentxx_stop` / `agentxx_destroy` | 创建(不启动线程)/异步启动(EVT_READY)/同步停止(幂等)/销毁(未 stop 自动 stop) |
| 会话交互 (异步) | `agentxx_send_input` / `agentxx_cancel` / `agentxx_select_model` / `agentxx_set_permission` / `agentxx_switch_session` | 投递 io 线程串行执行; READY 前发送的输入自动缓存 |
| 同步查询 | `agentxx_get_model_info` / `agentxx_get_context_messages` / `agentxx_list_sessions` | 阻塞等待服务端响应 (最长 10s), 返回 JSON (`agentxx_free`); 同一句柄同一时刻仅允许一个在途 |
| HIL 应答 | `agentxx_interrupt_respond` | 提交 EVT_INTERRUPT_REQ 的应答 (values_json 数组与 inputs 顺序一一对应) |
| 日志 | `agentxx_drain_logs` | 取走积压日志 `[{"level","message"},...]` (异常后排障) |
| 事件队列 | `agentxx_event_queue_create` / `agentxx_free`(队列) / `..._on_event` / `..._pop` | 见 4.2 |
| 内置插件 | `agentxx_get_builtin_plugins` | 内置合并编译模式插件清单入口 (PluginManager 使用; 白名单第 25 个符号, 隐藏 17 万 C++ 符号) |

版本策略: 修改契约递增 `AGENTXX_FFI_API_VERSION` (当前为 1);
新增符号/字段为非破坏性不递增, 删除/重命名或修改参数语义时递增。

### 4.4 事件类型 (`AgentxxEventType`, payload 均为 NUL 结尾 UTF-8 JSON)

| 事件 | payload | 说明 |
|------|---------|------|
| `EVT_READY` | `{"sessionId"}` | 服务端就绪, 可开始发送输入 |
| `EVT_SYNC` | wire sync JSON | 全量/部分历史同步 (含 fromIndex 尾窗语义) |
| `EVT_DELTA` | wire delta JSON | 流式增量 (kind=text_token/thinking_token/tool_start/tool_end/turn_end/...) |
| `EVT_TURN_END` | wire turn_result JSON | 轮次结束 (`has_error` 字段报告异步错误) |
| `EVT_CONTEXT_STATS` | wire context_stats JSON | 上下文 token 统计 (含 tps) |
| `EVT_MODEL_INFO` | wire model_info JSON | 当前模型信息 (查询/切换结果) |
| `EVT_COMPONENTS` | wire append_component_info JSON | 启动组件 (MCP/Skill/Memory/插件) 加载信息 |
| `EVT_INTERRUPT_REQ` | `{"interruptId","sessionId","node","value","argJson"}` | HIL 中断询问 (权限确认/输入收集); argJson.inputs 描述输入项 (bool/int/double/string/enum + defaultValue/enumValues) |
| `EVT_INTERRUPT_EXPIRED` | `{"interruptId"}` | 中断已过期/取消, 不再可应答 |
| `EVT_PLUGIN_DATA` | wire plugin_data JSON | agent 侧插件事件转发 (`{plugin,event,data}`) |
| `EVT_ERROR` | `{"code","message"}` | 内部错误 |

### 4.5 配置与模型 JSON (`agentxx_create` 参数)

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
  "permissionMode": "ask",            // ask|all_ask|pass|deny (yaml permission.mode 同值; ask=工作目录内 ALLOW 其余 INTERRUPT)
  "permissionAllowPaths": ["..."],    // 权限白名单
  "permissionDenyPaths": ["..."],     // 权限黑名单
  "skills": ["..."],                  // 技能目录列表
  "memoryFiles": ["..."],             // 上下文文件列表
  "mcpServers": {"ns": {"url": "...", "timeoutSec": 120}},
  "plugins": [{"path":"...", "enabled":true, "sides":"agent|client|auto", "args":{}}],
  "llmMaxRetry": 5,
  "agentName": "Agentxx",
  "interruptTimeoutSec": 0 }          // HIL 等待宿主应答超时, 0=不限 (默认)

// model_json (建议必填; isValid: baseUrl 非空 或 apiKey != "EMPTY"):
{ "name": "显示名", "type": "openai|anthropic|openai-responses",
  "baseUrl": "...", "apiKey": "...", "modelName": "(请求 model 字段)",
  "apiPath": "", "connectTimeoutSeconds": 16, "readChunkTimeoutSeconds": 100,
  "sslVerify": true|null, "maxConcurrentConnections": 5,
  "anthropicVersion": "2023-06-01", "modelContextMaxToken": 0,
  "extraHeaders": {"k":"v"}, "extraConfig": {} }
```

## 5. 语言绑定与示例

| 目录 | 说明 |
|------|------|
| [`agent/ffi/dart/`](/agent/ffi/dart/) | Dart FFI 绑定包: `ffigen.yaml` 由 `ffi_api.h` 自动生成符号定义 (`dart run ffigen --config ffigen.yaml`, 输出 `lib/agentxx_ffi_bindings.dart`) |
| [`agent/example/ffi/dart/`](/agent/example/ffi/dart/) | Dart CLI 示例 (`agentxx_dart_cli`): 流式渲染/HIL 权限与会话切换/`/model` `/sessions` `/logs` 等命令/Ctrl+C 优雅退出; 含 mock LLM 冒烟检查 (`example/smoke_check.dart`); 详见其 README |

其他语言按同样模式接入: 经 dlopen/dlsym (或平台等价物) 查找白名单符号,
注册 `AgentxxCallbacks` 回调即可。payload 生命周期敏感的宿主优先使用 4.2
事件队列桥接。

## 6. 测试

- `agent/test/test_ffi_c_api.cpp` (模块名 `ffi_c_api`): 生命周期/交互/HIL/
  事件队列/同步查询全覆盖, 内置 mock LLM Server 端到端验证

---

## 7. 实现细节补充 (2026-08 刷新)

- **Channel 直连**：FFI 层复用 `ChannelAgentIOTransport::makePair` (零序列化 concurrent_channel), 非 WS；`SessionServerAgentIO` 视为服务端端点，`FfiClientAgentIO` 为 client 端点，二者完全同构于 TUI/CLI 的 transport 抽象
- **工作目录回退**：`config_json.workDir` 支持 `~`/`\${VAR}` 展开与相对路径 (按进程 cwd 解析为绝对)；未配置时回退进程 `cwd`，与 `AgentConfig::resolvedWorkDir()` 语义一致；会话级 worktree 绑定 (`Session::WorktreeBinding`) 与 `AgentContext::getSessionWorkDir` 的多源回退对 FFI 句柄同样生效 (会话内所有相对路径自动切换)
- **权限 sides**：`plugins[].sides` 取值 `auto` (默认, 按导出符号 `agentxx_client_create` 自动决定) / `agent` (仅 agent 侧加载) / `client` (仅 client 侧，FFI 场景通常为 agent)
- **同步查询约束**：`get_model_info/get_context_messages/list_sessions` 同一句柄同一时刻仅允许一个在途 (服务端逐条协议)；超时 10s 返回 `AGENTXX_ERR_TIMEOUT`，payload 为 `{"code","message"}` 的 `EVT_ERROR` 也会并发上报
- **HIL 输入描述**：`EVT_INTERRUPT_REQ` 的 `argJson` 为 `InterruptHandleArg` 序列化，`inputs[]` 含 `label/depict/type (bool/int/double/string/enum)/defaultValue/enumValues`；空 `type` 表示无需输入 (应答空数组 `[]` 即可)
- **跨 CRT 堆**：所有 `char*` 返回值与 `char** log` 均经 `agentxx_malloc` 分配，宿主必须 `agentxx_free` 释放；`agentxx_strdup_n` 为统一拷贝入口


