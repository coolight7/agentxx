# 实施记录：FFI 接口异步化

## 进度

- [x] 现状调查（阻塞点 / 事件通路 / 相关性 / 消费方）与方案定稿：见 [plan.md](plan.md)
- [x] 规划通读核对（2026-11-24）：合批改默认关闭（D17 / §9.2）、D15 超时表补 `fs.list_dir`、
      事件枚举名与 `AgentxxFFICallbacks.struct_size`、D1 本地同步入口数、日志 sink 线程数、
      `ffi_api.h` 引用了不存在的 `agentxx_ffi_get_view_messages` — 均已改入 plan
- [x] 第 1 步：wire 相关性 + 通用回执（`reqId` + `WireAck` + 服务端 ack + schema）
- [x] 第 2 步：FFI 内部换代（`ffi_call` / `call_cancel` / `ffi_ops` + C API v4 + 事件所有权 + 测试重写）
- [x] 第 3 步：生命周期异步化（`stop` → `EVT_STOPPED`、`destroy` 约束）—— 与第 2 步同一批文件，合并实施
- [x] 第 4 步：benchmark FFI 场景更新（含调用吞吐 / 合批对比 / 首屏尾窗三个子场景）
- [x] 第 5 步：Dart 绑定重新生成 + SDK/示例异步化（含取消令牌与顺序用法）
- [x] 第 6 步：文档与导出白名单收尾
- [x] 第 7 步：全量回归（44016/0）+ Release 构建 `EXIT=0` + 冒烟收尾

## 开工前的现场（2026-11-24 调查，可直接引用）

### 文件与规模

| 文件 | 行数 | 说明 |
|---|---|---|
| `agent/lib/include/agentxx/ffi_api.h` | 508 | 35 个导出符号 + 事件枚举 + 回调结构 |
| `agent/lib/src/ffi/ffi_api.cpp` | 708 | C 边界：参数校验、`log` 填充、转发 |
| `agent/lib/src/ffi/ffi_runtime.cpp` | 1122 | 状态机 / 线程 / `syncQuery` / 生命周期 |
| `agent/lib/src/ffi/ffi_runtime.h` | 294 | 声明 + 成员 |
| `agent/lib/src/ffi/ffi_client_io.cpp` | 251 | 事件序列化（借用 payload）+ 应答分流 |
| `agent/lib/src/ffi/ffi_client_io.h` | 108 | `SyncKind` 5 种 + `onSyncReply` |
| `agent/lib/src/ffi/event_queue.cpp` | 178 | 阻塞 pop + 有界队列（本任务删除） |
| `agent/test/test_ffi_c_api.cpp` | 1380 | 唯一 FFI 测试模块（mock LLM） |
| `agent/ffi/dart/lib/agentxx_ffi_bindings.dart` | 949 | ffigen 生成物 |
| `agent/example/ffi/dart/lib/src/agent_client.dart` | 468 | 含 4ms 轮询循环与同步查询封装 |
| `docs/zh-cn/design/ffi.md` / `docs/en/design/ffi.md` | 497 / 509 | 契约文档 |

关键位置（行号为当前快照）：`ffi_runtime.cpp:426 stopInternal`、`:516 stop`、`:525 destroy`、
`:649 onSyncReplyOnClientThread`、`:667 syncQuery`、`:712 getModelInfo`；
`ffi_client_io.cpp:126 onPeerMessage`、`:232 emitEvent`。

### 已确认的事实（省去重复调查）

1. **同步查询不能从事件回调里调用**：`syncQuery` 把发送动作 `asio::post` 到 client io 线程后
   等 future；回调正占着那条线程 → 必然等满 10s 返回 `ERR_TIMEOUT`，期间事件全部停摆。
2. **应答匹配靠"按种类 FIFO"**，所以头文件里写了"同一句柄同一时刻仅允许一个未完成同步查询"。
3. **wire 侧只有两处带相关性字段**：`WireListDir.reqId`（已回显）与
   `WireUserInput.requestId`（仅 > 0 回 `input_ack`，服务端所有拒绝分支都会回）。
4. **命令类请求完全没有应答**：`WireSelectModel` / `WireSwitchSession` / `WireCompactContext` /
   `WireCancel` / `WireClearMessageQueue` / `WireRemoveQueueItem` / `WireInterruptAndRunNext` /
   `WireInterruptResponse` / `WireHostToolRegister|Unregister` / `WireHostToolResult` /
   `WirePluginDataUp`；`WireRemoveQueueItem` 只在失败时发 `WireError`（无 reqId）。
5. **`select_model` 服务端不回 ack 且会静默忽略未知模型**：`BaseAgent::selectModel`
   （`base_agent.cpp:1491`）只在 registry 里存在时才写会话模型名，空名字表示"沿用当前"
   （TUI 有这种用法，`session_server_agent_io.cpp:1588` 的注释）。
6. **`set_full_auth` 走广播**：`sendToPeer(WirePermissionState)`，且只在状态变化时发；
   `setFullAuthorized` 在权限中间件缺失时返回 false（只打日志）。
7. **`host_tool_register` 无回执**：注册冲突只打 `XX_LOGW`；`handleHostToolResult` 收到未知
   `callId` 也只打日志。
8. **`compact_context` 没有完成信号**：`SummarizationMiddlewareHandle::compactSessionContext`
   （`summarization.cpp:1183`）返回 `bool` 但只有内部订阅者知道；`Topic::SummarizationCompact`
   只是"发起"。
9. **`sendToClient(transport, msg)` 只发给该连接**（transport 失效时退回广播），
   `sendToPeer` / `broadcastToClients` 发给所有客户端 —— 给 `set_full_auth` 回执时要避免
   发起方收到两条（需要 `broadcastToClientsExcept`）。
10. **`acceptSessionScope` 失败时发 `WireError`**（无 reqId），FFI 侧只能当通用错误事件，
    没法让对应调用失败。
11. **`agent/lib/CMakeLists.txt` 用 `GLOB_RECURSE`** 收集源文件（无 `CONFIGURE_DEPENDS`）：
    删除/新增 `.cpp` 后要重新跑一次 cmake 配置，否则构建系统看不到。
12. **导出白名单双份维护**：ELF/macOS 用 `agent/lib/ffi_symbols.map`，MSVC 从同一份 map
    解析 `agentxx_ffi_*` 生成 `ffi_symbols.exp`，删符号只改这一处即可。
13. **`test_ffi_c_api` 直接静态链接调用导出符号**（不经 dlopen），改了签名后测试编译期就会报错，
    不会漏改。
14. **benchmark 的 FFI 场景（`bench_resource.cpp:1607`）走 `dlopen` + `decltype(&::符号)`
    按名解析**：符号删掉后运行期才失败，改的时候要连解析列表一起改。
15. 顺带发现（plan §2.6）：`libraryVersion` 写死 `"0.4.0"`、能力清单缺 `VIEW_REPLICA`、
    `ffi_api.h` 注释写 `apiVersion:2`、`ffigen.yaml` 列了不存在的 `agentxx_ffi_set_permission`、
    `FfiClientAgentIO::notifyServerReady` 是死代码、`ffi_api.h:74` 引用了不存在的
    `agentxx_ffi_get_view_messages`、事件枚举名 `EVT_HOST_TOOL_Canceled` 大小写与同族不一致、
    `docs/zh-cn/design/ffi.md` 自相矛盾（§1 写 26 个符号、§4.3 写 35 个，实际 35）。
16. **性能现场**（plan §9.1）：FFI 的 delta 合批默认 0 且 Dart SDK 从未调 `set_delta_batch`
    → 现在逐 token 跨边界（新 SDK 只提供开关、仍默认关闭，见 plan §9.2-1 / D17）；`rtCfg.initialSyncTailCount = 0` → 首屏完整同步；每实例 3 条线程
    （含 `ThreadedLogSink` 后台线程）；`ChannelAgentIOTransport` 是零序列化的（`WireMessage`
    直接传），所以 JSON 只在 C 边界出现。
17. `utilxx_base::LogSink` 的队列上限 4096，**满了丢新条目**（不是丢最旧）：若改成
    "drain 时才 pump" 会保留最旧的日志，所以必须低频定时 pump（plan §9.2-9）。
18. `WireHello.historyTail`（-1 = 用服务端默认）与 `WireGetViewMessages` 已支持尾窗 + 分页，
    FFI 只是没声明；`context.get`（`WireGetContext`）目前无任何参数，需加可选 `tail`。

## 各步记录

### 第 1 步：wire 相关性 + 通用回执 (2026-11-24 完成)

改动文件与内容:

| 文件 | 改动 |
|---|---|
| `agent/lib/include/agentxx/agent/io/agent_io_transport.h` | 31 个请求/应答结构体末尾加 `uint64_t reqId = 0`（`WireGetContext` 另加 `uint32_t tail = 0`）；新增 `WireAck` 并追加到 `WireMessage` 变体末尾 |
| `agent/lib/include/agentxx/agent/io/wire_protocol.h` | `MsgType::Ack`；`putReqId` / `readReqId` / `withReqId` / `assignReqId` 四个轻量工具；`makeAck` / `ackFromJson`；`makeGetContext` 加可选 `tail`；声明 `toJson(WireAck)` / `ackMsgFromJson` |
| `agent/lib/src/agent/wire_protocol.cpp` | 各 `toJson` 统一走 `withReqId`（`reqId == 0` 不写字段）；`deserialize` 入口统一 `assignReqId`（各 `*FromJson` 不必各写一遍）；补 `ack` 反序列化映射；`getContextFromJson` 读 `tail` |
| `agent/lib/include/agentxx/agent/io/session_server_agent_io.h/.cpp` | 新增 `sendAck` / `broadcastToClientsExcept` / `subscribeSummarizationDone` / `pendingCompactAcks_`；`acceptSessionScope` 加 `reqId`（非 0 改回 `session_mismatch` ack）；`resolveInterrupt` 改为返回 bool；`handleHostToolResult` 加 sender 参数；各命令分发处回 ack |
| `agent/lib/include/agentxx/event/events.h` | 新增 `Topic::SummarizationCompactDone` 与 `EventCompactContextDone{sessionId, ok}` |
| `agent/lib/src/middlewares/summarization.cpp` | `compactSessionContext` 在**每条**退出路径发布完成事件（含三条提前 `co_return false`） |
| `agent/test/core/test_wire_roundtrip.cpp` | 新增 reqId 往返（含 `reqId == 0` 不写字段）、`tail`、`WireAck` 成功/失败与缺 reason 的兜底断言 |
| `agent/test/core/test_wire_schema.cpp` | 补 `ack` 示例（变体最后一个），`select_model` / `get_model` / `model_info` / `get_context` / `host_tool_result` 示例带 reqId 以便生成物里出现该字段 |
| `agent/schema/wire-schema.json`、`docs/zh-cn/design/wire-protocol-fields.md` | 用 `AGENTXX_UPDATE_WIRE_SCHEMA=1` 重新生成 |

关键实现选择:

1. **写侧 `withReqId` / 读侧 `assignReqId` 各只一处** —— 读侧放在 `deserialize()` 入口（`std::visit` + `requires { m.reqId; }`），
   以后新增带 `reqId` 的消息不必逐改 30 多处 `*FromJson`；写侧仍在各 `toJson` 里显式调用，
   这样 schema 生成物能看到该字段。
2. **`reqId == 0` 一律不写字段、不回 ack** —— TUI/旧客户端行为不变（TUI 不填 reqId，也不会收到它不认识的 `ack`）。
3. **`set_full_auth` 的回执与广播分开** —— 发起方拿带 reqId 的 `WirePermissionState`，
   其余客户端经新增 `broadcastToClientsExcept` 收广播，避免发起方收两条。
4. **`session.compact` 的回执等真的结束** —— `pendingCompactAcks_` 按会话 FIFO 排，
   `SummarizationCompactDone` 到达时逐个回（`ok=false` 用 `compaction_failed`）。
5. **`select_model` 补模型存在性校验** —— 空模型名仍表示“沿用当前”（TUI 切换会话后重发用），
   名字不在注册表则回 `model_not_found`（以前是静默忽略）。
6. **`plugin_data_up` 的 ack 在 `bus->publish` 之后** —— 语义是“已发布到 agent 总线”，
   不是在“已投递到总线 executor”时。

验证:

- 顶层 Debug 构建通过（首次构建 MSBuild 子节点异常退出，原样重跑即成功，与本次代码无关）。
- `wire_roundtrip` 285 passed / 0 failed；`wire_schema` 1377 passed / 0 failed（schema 已重新生成）。
- `agent`（223）/ `session_admin`（38）/ `input_delivery`（140）/ `remote_agent`（602）全绿。

### 第 2 步：FFI 内部换代 (2026-11-24 完成)

改动文件与内容:

| 文件 | 改动 |
|---|---|
| `agent/lib/src/ffi/ffi_call.h/.cpp`（新增）| `CallRegistry`：`begin` / `abort` / `settle` / `cancel` / `failAll` / `pendingCount` / `stop`；共享一条 `asio::steady_timer` + 按到期时刻排序的 `multimap` 索引（超时不再每个调用一条定时器）；`settle` 用 CAS 保证**恰好一条回执** |
| `agent/lib/src/ffi/ffi_ops.h/.cpp`（新增）| `CallContext`（reqId / 会话 / client 端点 / 投递函数 / 本地完成回调）、`FfiCompletion`（`TypedResponse` / `Ack` / `Local`）、`FfiOpDef`（名称 / 完成方式 / 默认超时 / 默认失败码 / 处理函数）与 **27 个 op**；`findOp` / `opsDefinition` |
| `agent/lib/include/agentxx/ffi_api.h` | v4：`AGENTXX_FFI_API_VERSION` 3→4；新增 `AGENTXX_FFI_ERR_CANCELED(-9)` / `AGENTXX_FFI_ERR_REJECTED(-10)`；新回调结构 `AgentxxFFICallbacks{struct_size, reserved, on_event, user_data}`（payload 归回调所有）；事件枚举新增 `EVT_STOPPED` / `EVT_RESULT` / `EVT_MESSAGE_QUEUE` / `EVT_PERMISSION_STATE` / `EVT_HOST_TOOL_CANCELED`，删除 `EVT_WIRE`；导出面 35 → **15** 个符号（去掉全部同步查询、`send_wire` 与 `event_queue_*`，新增 `call` / `call_cancel`）| 
| `agent/lib/src/ffi/ffi_api.cpp` | 只剩内存/版本/`strerror`/`get_capabilities`/`create`/`start`/`stop`/`destroy`/`call`/`call_cancel`/`drain_logs`；`create` 校验 `callbacks.struct_size` 与 `reserved` |
| `agent/lib/src/ffi/ffi_client_io.h/.cpp` | `emitOwnedEvent`（`agentxx_ffi_malloc` 分配、不回收）；`emitCallResult` / `emitEvent`；`onPeerMessage` 全部按 `reqId` 分流（`reqId == 0` 才走广播事件）；`WireInputAck` 按 `requestId` 当 reqId 结算；删除 `SyncKind` / `onSyncReply` / `EVT_WIRE` |
| `agent/lib/src/ffi/ffi_runtime.h/.cpp` | 状态机 `Created/Starting/Ready/Stopping/Stopped/Failed` + `calls_`；`call()` 同步校验后 `begin` 再 `run`（同步失败 `abort` 且不写 outReqId）；`callCancel()` 幂等；日志 `LogSink` + 每秒 pump；`sessionMutex_` 保护会话绑定 |
| `agent/lib/src/ffi/event_queue.cpp` | **删除**（阻塞 pop + 有界队列全部退役）| 
| `agent/lib/include/agentxx/agent/io/user_input_json.h/.cpp` | 新增 `parseUserInputJsonValue(const Json&)`，`parseUserInputJson` 改为 parse 后委托（FFI 的 `input.send` 复用同一套校验）| 
| `agent/lib/ffi_symbols.map` | 白名单改为 15 个符号 |
| `agent/test/test_ffi_c_api.cpp` | 重写为 17 个用例（451 条断言）：事件记录器（payload 立刻 free）/ 提交-回执封装；并发 20 调用、取消与迟到应答、超时、顺序、回调内调用、生命周期顺序、本地 op、hosttool、能力清单、fs/plugin_data、HIL、轮次取消、多句柄并发、headless |

关键实现选择:

1. **超时只用一个定时器**：`deadlines_` 是 `multimap<到期时刻, reqId>`，重臂时取最小到期；回调带代次号，旧代次直接退出。
2. **恰好一条回执**：服务端应答 / 超时 / 取消 / 收尾都汇到 `settle`（`atomic<bool>` CAS），赢家负责 `erase` + 投递，后到者返回 false 被丢弃并记 debug 日志。
3. **回执投递就地或投递**：已在 client io 线程（服务端应答 / 超时 / 收尾）就同步发，其它线程（`call_cancel` / 本地 op）`asio::post` 过去 —— 这样“取消/收尾产生的回执排在 `EVT_STOPPED` 之前”不需要额外同步。
4. **`reqId == 0` 的应答一律不当回执**：TUI 等其它客户端的结果、服务端主动广播仍走广播事件；`EVT_WIRE` 与它一起删除。
5. **ack 的 `result` 取 `data`**（plan §3.3 `ok ? result = data`），不是整个 ack 对象 —— 调用方已经在信封上看到 `op` / `reqId` / `ok`。
6. **`input.send` 直接用 reqId 当 wire `requestId`**（plan D9），服务端 `input_ack` 按它匹配，不需要第二张映射表。
7. **本地 op 不进 wire**（`language.get/set`、`delta.set_batch`）：`begin` 时超时置 0，处理函数当场 `.completeLocal(result)`。
8. **headless（`on_event == NULL`）不序列化**：`emitOwnedEvent` 在分配前就返回，回执照常结算（只是看不到）。

### 第 3 步：生命周期异步化 (2026-11-24 完成, 与第 2 步同一批文件)

| 位置 | 行为 |
|---|---|
| `stop()` | `stopRequested_` 交换保证幂等（重复调用不再发事件）；Created 直接在调用线程收尾，其余起**收尾线程** `stopInternal()` → `finishStop()` |
| `stopInternal()` | ① `runtime_->stop()` ② client io 屏障（5s）内跑 `onRuntimeStopping`（挂起中断失败 + 未完成调用按 `canceled` 收尾）③ 等 `serverIO()->running()==false`（20s）④ 摘日志 sink ⑤ `shutdownAsync`（25s）→ 停 server io + join ⑥ 停 client io + join ⑦ 显式释放 transport 对象 |
| `finishStop()` | 置 `Stopped` 后发 `EVT_STOPPED`（最后一个事件）|
| `destroy()` | 只在 `Created` / `Stopped` 成功；**内部线程（含收尾线程）上调用直接拒绝**；纯释放 + `joinFinishThreadIfNeeded()`（自我 join 改为 detach）|

偏差与注意:

1. **`interrupt.respond` 保留本地预检**：`hasPendingInterrupt(id)` 不成立时直接同步失败（`ERR_INTERRUPT`），不进 wire；服务端的 `interrupt_not_found` 同样映射到 `-6`。这样“应答一个不存在/已应答的中断”不会白等一次往返。
2. **`session.switch` 在提交时就切本地绑定**（与旧实现一致），要确认生效就等这条回执；回执失败（`session_mismatch`）不会把绑定改回去。
3. **附件用例未逐字段覆盖**：`input.send` 的附件仍经公共 `parseUserInputJsonValue` 校验（与 CLI/插件同一套规则），FFI 测试只验证带附件时提交成功与 ack，具体媒体能力判定不在本任务范围。
4. **`stop` 期间的 `call` 返回 `ERR_STATE`**（`acceptsCalls()` 只接 Starting/Ready），与 plan §3.7 一致。

验证:

- 顶层 Debug 构建 `EXIT=0`。
- `ffi_c_api` **451 passed / 0 failed**（含取消/超时/并发/顺序/回调内调用/headless）。
- `boundaries` 20 passed（修了两处注释用词：`领域` → `业务`）。
- `wire_roundtrip` 285 / `wire_schema` 1377 / `session_admin` 38 / `input_delivery` 140 / `remote_agent` 602 / `agent` 223 全绿。

### 第 4 步：benchmark FFI 场景 (2026-11-25)

| 文件 | 改动 |
|---|---|
| `agent/benchmark/bench_resource.cpp` | `benchResourceFfi` 重写：新增 `FfiBenchLib`（**15 个符号全部解析** + 缺哪个点名）、`FfiBenchEvents`（payload 在回调里释放，按事件类型留计数/回执）、`FfiBenchHandle`（create/start/ready、`callOp` 等回执、stop 等 `EVT_STOPPED`、destroy）；`get_context_messages` → `call("context.get")`；`send_input` → `call("input.send")`；新增三个子场景 |
| 同上 | 子场景一 **调用吞吐**: 预热 20 次后 1000 次 `model.list`（submit+ack），给出总耗时、us/op、op/s |
| 同上 | 子场景二 **事件吞吐**: 起第二个 mock（`AGENTXX_BENCH_FFI_STREAM_CHUNKS`, 默认 64 段 SSE），`model.add` + `model.select` 切过去，合批关 / 24ms 各跑一轮，记事件数 / 字节 / 轮次耗时 |
| 同上 | 子场景三 **首屏尾窗**: 临时句柄（会话落盘开、无插件）跑 40 轮造长会话，`session.switch` 回同一会话量 `EVT_SYNC` 字节/条数/耗时（`syncTailCount=200`）；再用 `syncTailCount=0` 的句柄切到同一会话对比完整同步 |

验证（见下节“验证清单”）：Release 构建与 `resource_ffi` 实跑结果。

偏差:

1. **首屏尾窗的度量位置**: FFI 每次 `create` 都会新建会话（没有“接管已有会话”的配置），所以拿不到“首次接入已有长历史”的场景；改为用 `session.switch` 度量——它走同一个 `buildTailSync(effectiveSyncTailCount())`（服务端同一函数），字节数与延迟的统计方式一致。
2. **`session.list` 的会话 id 字段名是 `sessionId`**（不是 `id`），第一版写错导致尾窗子场景拿不到 id，已修。

顺带修掉的真问题（尾窗子场景跑不出来时才暴露）:

- 现象: 会话落盘打开（`enableSessionStore: true`）时 `session.list` 永远不回执，10 s 后超时；
  `enableSessionStore: false` 时正常（`ffi_c_api` 里原本只有开关关的那条路径）。
- 根因: `session_server_agent_io.cpp` 的 `WireListSessions` 分支在协程开头设了 `resp.reqId`，
  紧接着 `resp = co_await utilxx::offloadAsync<WireSessionList>(...)`（或走内联分支的同名赋值）
  **把整个结构体换成取数结果**，而 `listSessionsFor()` 返回的结构体不带 reqId（它只负责取数），
  于是回执带着 `reqId = 0` 发出去，FFI 侧按“不是回执”丢掉（`[ffi] reply without reqId ignored`）。
  会话库关掉时走的是早退分支 `sendToClient(sender, WireSessionList{.reqId = m.reqId})`，所以看不出来。
- 修法: 取数之后再补 `resp.reqId = req.reqId;`（注释写明了“先设再赋值会被覆盖”）。
- 新增覆盖: `test_ffi_c_api.cpp` 的用例 18 `testSessionStoreOps()` —— 落盘打开的句柄跑一轮后
  `session.list`（断言至少一条且带 `sessionId`）+ `session.rename`（写会话库）。
  修为前该用例稳定失败（3 条断言），修后 `ffi_c_api` 463 通过 / 0 失败。

第二个真问题（Dart `smoke_check` 首跑就崩在 `RtlFreeHeap`）:

- 现象: `NativeCallable.listener` 收到的 payload 在 Dart 侧 `agentxx_ffi_string_free`
  时堆破坏（`RtlFreeHeap+0x7d`）。
- 根因: `emitOwnedEvent()` 把 `AgentxxString payload{buf,size}` 放在**自己的栈上**再传指针。
  同步回调没问题，但 `listener` 是异步投递：原生函数返回后栈帧已回收，Dart 稍后读
  `payload.ref` 拿到的是废内存（不是正文指针）。所谓“payload 归回调所有”只对正文成立。
- 修法: 结构体与正文放进**同一块** `agentxx_ffi_malloc` 内存（结构体在前，`data` 指向
  紧跟的字节），回调一次性 `agentxx_ffi_free(payload)` 释放整块；`agentxx_ffi_string_free`
  保持“只释放出参内容、结构体归调用方”的语义（头里已写明两者区别）。
- 同步改动: 头文件注释、`test_ffi_c_api.cpp` 的事件记录器、`bench_resource.cpp` 的
  `FfiBenchEvents`（改用 `agentxx_ffi_free`）、Dart SDK `_onNativeEvent`、中英 `ffi.md`。

### 第 5 步：Dart 绑定与 SDK 异步化 (2026-11-25)

| 文件 | 改动 |
|---|---|
| `agent/ffi/dart/ffigen.yaml` | 函数清单按 v4 重写成 15 个符号（去掉了不存在的 `set_permission`、事件队列四个、同步查询、`send_wire`、`tool_*`）；`structs` 补 `AgentxxString` / `AgentxxStringView` |
| `agent/ffi/dart/lib/agentxx_ffi_bindings.dart` | 用 ffigen 重新生成（431 行）|
| `agent/ffi/dart/README.md` | 补 v4 调用模型与再生命令（**ffigen 19 从 `--config` 文件读配置**，旧版从 `pubspec.yaml` 的 `ffigen:` 段读）|
| `agent/example/ffi/dart/lib/src/agent_client.dart` | 重写成 `Future`/`Stream`: `call(op, args, {timeout, cancel})` / `callNoWait` / `cancelCall` / `AgentxxCancelToken` / `AgentxxCanceledException`；`NativeCallable.listener` 收事件（payload 回调内 `string_free`）；`reqId → Completer` 表；`stop()` 等 `EVT_STOPPED`、`dispose()` 再 destroy；28 个便捷方法（input/model/session/context/permission/fs/interrupt/hosttool/plugin/language/delta）、`enableDeltaBatch`/`disableDeltaBatch`（**默认不开**）、`start(syncTailCount: 200)` |
| `agent/example/ffi/dart/lib/src/events.dart` | 新增 `ResultEvent` / `StoppedEvent` / `DeltaTextBatchEvent` / `MessageQueueEvent` / `PermissionStateEvent` / `HostToolCallEvent` / `HostToolCanceledEvent` / `CallError`；删 `EVT_WIRE`；`ContextStatsEvent` 补累计用量字段 |
| `agent/example/ffi/dart/lib/src/repl.dart` | 命令全部改 `await`；新增 `/compact` `/page` `/rename` `/batch` `/auth` `/caps`；会话 id 字段改 `sessionId`；权限应答改 `values:{decision,remember}` |
| `agent/example/ffi/dart/bin/agentxx_cli.dart` | 新增 `--sync-tail <n>`；启动传 `syncTailCount` |
| `agent/example/ffi/dart/lib/src/dll_loader.dart` | 补 `../../../build/*/exec` 候选（示例包在 `agent/example/ffi/dart` 而不是 `agent/ffi/dart`）|
| `agent/example/ffi/dart/example/smoke_check.dart` | 重写为 5 个场景：对话流 / HIL 权限 / 调用顺序（add→select→list→未找到→context→page→本地 op）/ 取消令牌 / 生命周期（`EVT_STOPPED` 是最后一条事件）|

偏差:

1. **Dart 侧验证只能用 Release 库**: Debug 库带 ASan (`clang_rt.asan_dynamic-x86_64.dll`)，注入非 ASan 宿主进程（dartvm）时 ASan 自己 abort（`AddressSanitizer: access-violation`）—— 不是我们的代码问题，是“ASan 不能注入未插桩宿主”。
2. **`interrupt.respond` 的 `values` 是控件值对象**（TUI 的权限卡片控件 id 是 `decision` / `remember` / `fullAuth`），FFI 负责包成服务端要的 `{"values":{...}}`；v3 的 Dart 示例传的是 `["true"]` 数组，已改。
3. **`call()` 要把 `canceled` 还原成子类**: 回执里只有 `error.name == "canceled"` / `code == -9`，`call()` 现在直接抛 `AgentxxCanceledException`（不再抛基类），调用方不必自己比错误名。
4. **`smoke_check` 四处稳定性修正**（首跑全暴露）:
   - 场景 C 的 `model.add` 会写 `{dataDir}/agentxx-config.yaml`: 改成用临时 dataDir 并在 `finally` 删掉，否则第二次跑报“已有同名模型”；
   - 场景 D 原来拿空会话的 `session.compact` 当“在飞调用”，但空上下文会立刻返回 `compaction_failed`，竞不过取消: 改成先跑一轮种子消息，再让 mock 每次响应延迟 3 s，取消才必然落在窗口内；
   - 场景 E 断言 `EVT_STOPPED` 是最后一条事件时看的是 **Dart 事件流**（异步投递），`stop()` 返回时流里可能还有排队事件: 改成先等 `StoppedEvent` 真被消费，再等 300 ms 确认后面无新事件；
   - mock 新增 `responseDelayMs`（场景 D 用）。

### 第 6 步：文档与导出白名单收尾 (2026-11-25)

| 文件 | 改动 |
|---|---|
| `docs/zh-cn/design/ffi.md` | 改写为 v4: 15 个符号 / 27 个 op / 事件表 / 生命周期状态机 / 顺序超时取消语义 / 性能默认值；保留中断 UI 渲染指引与输入 JSON；删事件队列、`send_wire`、同步查询约束 |
| `docs/en/design/ffi.md` | 同上（英文版）|
| `AGENTS.md` | “FFI API 抬到 3” → 4，并补一条 v4 汇总（15 符号 / op / 回调所有权 / stop 异步）|
| `docs/zh-cn/design/index.md` | FFI API 版本说明与测试说明改为 v4 口径 |

导出白名单（`agent/lib/ffi_symbols.map`）在第 2 步已一并改成 15 个符号；MSVC 的导出列表从同一份文件生成，无第二处。

## 后续修（2026-11-25, 本任务收尾后）: 服务端回包协程的异常兜底

背景: `asio::co_spawn(..., asio::detached)` 不处理异常 —— 分支里的取数/发布抛异常时，
异常被丢掉，对端永远等不到那一帧（`session.compact` 甚至没有超时）。

| 分支 | 失败时的回包 |
|---|---|
| `WireListSessions` | 空列表 (`{sessions:[], totalCount:0, hasMore:false}`) + 原因记日志 |
| `WireListDir` | `ok=false` + `error` 写异常文本（这个结构本来就有这两个字段）|
| `WireCompactContext` | `sendAck(false, "compact_failed", 原因)`；同时把自己在 `pendingCompactAcks_` 里的那一项摘掉（已经被完成事件回执过就不补第二条，保持“恰好一条回执”）|
| `WirePluginDataUp` | `sendAck(false, "publish_failed", 原因)` |

统一用 `agentxx::util::catchErrorAsync`（取消/中断类异常仍然向上抛，不当普通错误吞掉）。

新增用例 21 `testSessionListBrokenStore`: 往会话库目录里放一个损坏的 `session.db`，
断言分页与全量两条取数都在限定时间内拿到回执。说明: 目前 `SessionStore` 内部对单个
坏库已经做了降级（跳过该目录、不抛异常），所以该用例固定的是“客户端总能拿到回执”这个
外部契约，不直接覆盖 catch 分支本身。

验证: `ffi_c_api` **478 通过 / 0 失败**（新增 15 条）；`session_admin` / `remote_agent` /
`input_delivery` 全绿；全量测试见下。

## 验证清单

- [x] 顶层 Debug 构建 `EXIT=0`
- [x] 顶层 Release 构建 `EXIT=0`（含 benchmark、LTO）
- [x] `ffi_c_api` 全绿（478 条：每个 op / 并发 / 超时 / **取消** / **顺序** / 回调内调用 /
      事件所有权 / 生命周期顺序 / 会话落盘下的 `session.list` + `session.rename` /
      会话库损坏时 `session.list` 仍回执）
- [x] `wire_roundtrip`(285) / `wire_schema`(1377) / `session_admin`(38) / `input_delivery`(140) 全绿，
      schema 生成物已提交
- [x] 全量 `agentxx_test --log-level warn`: **44031 通过 / 0 失败**（2026-11-25，机器无其他构建；
      含本次异常兜底的用例）
- [x] 异常兜底改动后的顶层 Release 构建 `EXIT=0`；Dart `smoke_check` 5 个场景仍全绿
- [x] `agentxx_benchmark resource_ffi` 跑通（调用吞吐 / 合批 / 尾窗三个子场景都有数据，
      数据记入 benchmark.md）
- [x] `dart analyze`: 两个包都 `No issues found`
- [x] Dart `example/smoke_check.dart` 5 个场景全绿（Release `libagentxx.dll`；
      Debug 库带 ASan 无法注入 dartvm）
- [x] 新增 ack 只在请求带 `reqId` 时才发，TUI 客户端发 0 所以拿不到新增回帧；
      TUI 侧的交互仍由全量测试的 `tui_*` / `client_plugins` 覆盖（未做手工界面抽查）
- [x] 文档: `ffi.md`（中/英）、`AGENTS.md`、`index.md`、`benchmark.md`
      （`wire-protocol-fields.md` 与 schema 在第 1 步已生成）

## 剩余事项

- 想区分“真的没有会话”与“取数失败”时，需要给 `WireSessionList` 加一个 `error` 字段
  （现在失败回的是空列表，调用方看不出区别）；本次没加，避免改协议与 schema。
- 其他会话控制分支（`WireSwitchSession` 等）已经自己局部捕获了可预期的失败（加载失败记
  日志后继续切），但整个协程体本身仍没有统一兜底 —— 以后再往里加可能抛的调用时，记得
  用 `catchErrorAsync` 包一层。
