# 实施记录：FFI 接口异步化

## 进度

- [x] 现状调查（阻塞点 / 事件通路 / 相关性 / 消费方）与方案定稿：见 [plan.md](plan.md)
- [x] 规划通读核对（2026-11-24）：合批改默认关闭（D17 / §9.2）、D15 超时表补 `fs.list_dir`、
      事件枚举名与 `AgentxxFFICallbacks.struct_size`、D1 本地同步入口数、日志 sink 线程数、
      `ffi_api.h` 引用了不存在的 `agentxx_ffi_get_view_messages` — 均已改入 plan
- [ ] 第 1 步：wire 相关性 + 通用回执（`reqId` + `WireAck` + 服务端 ack + schema）
- [ ] 第 2 步：FFI 内部换代（`ffi_call` / `call_cancel` / `ffi_ops` + C API v4 + 事件所有权 + 测试重写）
- [ ] 第 3 步：生命周期异步化（`stop` → `EVT_STOPPED`、`destroy` 约束）
- [ ] 第 4 步：benchmark FFI 场景更新（含调用吞吐 / 合批对比 / 首屏尾窗三个子场景）
- [ ] 第 5 步：Dart 绑定重新生成 + SDK/示例异步化（含取消令牌与顺序用法）
- [ ] 第 6 步：文档与导出白名单收尾
- [ ] 第 7 步：全量回归 + 收尾

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

（每完成一步在这里补：改了哪些文件、怎么验证、遇到的问题。未开工。）

## 验证清单

- [ ] 顶层 Debug 构建 `EXIT=0`
- [ ] `ffi_c_api` 全绿（每个 op / 并发 / 超时 / **取消** / **顺序** / 回调内调用 / 事件所有权 / 生命周期顺序）
- [ ] `wire_roundtrip` / `wire_schema` 全绿，schema 生成物已提交
- [ ] TUI 交互抽查（`select_model` / `compact` / 队列操作）未被新增 ack 影响
- [ ] Release 构建通过（含 benchmark、LTO）
- [ ] `agentxx_benchmark` 的 `resource_ffi` 场景可跑通
- [ ] `dart analyze` 无告警；`example/smoke_check.dart` 全绿
- [ ] 性能：合批开/关的事件数与 CPU 对比、首屏尾窗 0 vs 200 的字节/延迟、headless 不序列化
      （均记入基准报告）
- [ ] 全量 `agentxx_test --log-level warn` 0 失败
- [ ] 文档：`ffi.md`（中/英）、`wire-protocol-fields.md`、`AGENTS.md` 已更新

## 剩余事项

（实施中出现、但不属于本任务范围的，记在这里。）
