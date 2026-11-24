# FFI 接口异步化：调用提交 + 事件回执 + 可取消（FFI API v4）

## 目标

把 libagentxx 的 C 导出层从"同步查询 + 借用事件 + 阻塞收尾"改成"提交 + 回执"，让任何语言
的 FFI 绑定都能写成 `async/await`，调用线程（包括事件回调线程）上不再出现跨线程等待。

1. **一个异步入口**：`agentxx_ffi_call({op,args,timeoutMs})` → 立即返回 `reqId`，结果经
   `EVT_RESULT {reqId, op, ok, result|error}` 送达；同时可存在任意多个未完成调用。
2. **每个 reqId 恰好一个回执**：有服务端副作用的 op 以服务端回执完成（typed 应答或新增的
   通用 `WireAck`），纯本地 op 立即回执 —— 客户端要顺序保证时"先 await 前一步再发下一步"。
3. **可取消**：`agentxx_ffi_call_cancel(reqId)` 让仍在等待的调用立刻以
   `code=-9 canceled` 收尾；SDK 用显式取消令牌把"用户放弃"接到 `Future` 抛取消异常。
4. **事件自带所有权**：`on_event` 的 payload 改成"归回调所有"的 `AgentxxString`，
   Dart `NativeCallable.listener` 一类绑定的线程安全投递不再需要 C 侧队列 → 删掉
   `agentxx_ffi_event_queue_*` 四个符号与 SDK 的 4ms 轮询。
5. **生命周期异步**：`stop` 受理即返回，收尾完成发 `EVT_STOPPED`（最后一个事件）；
   `destroy` 只释放句柄（要求已停止），不再阻塞 join。
6. **导出面收敛**：35 个符号 → 15 个；新增能力只加 op 名，不动 C ABI。
7. **性能默认值**：首屏只同步尾窗（默认 200 条，更早历史用 `context.view_page` 拉）、
   headless 不序列化事件；增量合批**默认关闭**，SDK 只提供 `enableDeltaBatch` 开关与建议值
   （~24ms）；细节与可选项见 plan §9。
8. **不保留旧版本兼容**：`AGENTXX_FFI_API_VERSION` 3 → 4，旧语义直接删。

## 范围

- `agent/lib/include/agentxx/ffi_api.h`、`agent/lib/src/ffi/*`（重写 + 删除 `event_queue.cpp`）、
  `agent/lib/ffi_symbols.map`
- wire 协议：9 对 typed 请求/应答 + 12 个命令类请求补 `reqId`；新增通用回执 `WireAck`；
  服务端在每个命令的完成处回 ack（`agent_io_transport.h` / `wire_protocol.*` /
  `session_server_agent_io.cpp` / schema 生成物）
- 中间件：`session.compact` 的完成信号（`SummarizationCompactDone` 总线事件，约 15 行）
- `agent/test/test_ffi_c_api.cpp`（重写）、`agent/benchmark/bench_resource.cpp`（FFI 场景）
- `agent/ffi/dart`（ffigen 重新生成）、`agent/example/ffi/dart`（SDK 异步化 + 取消令牌）
- `docs/{zh-cn,en}/design/ffi.md`、`docs/zh-cn/design/wire-protocol-fields.md`、`AGENTS.md`

## 状态

**已完成（2026-11-25）。** 详细方案见 [plan.md](plan.md)，进度与验证见 [work.md](work.md)。

提交:

- `6b814ee3` wire 相关性 + 通用回执（第 1 步）
- `e76ce2a9` FFI 内部换代 + 生命周期异步化 + 测试重写（第 2、3 步）
- `750170bc` benchmark / Dart SDK / 文档收尾，并修 `session.list` 回执 reqId 丢失与
  事件 payload 结构体悬垂两个真问题（第 4~6 步）

## 关键决策

| 编号 | 决策 |
|---|---|
| D1 | 导出面收敛为 `agentxx_ffi_call` + `call_cancel` + 本地同步的 `get_capabilities` / `drain_logs` |
| D2 | 回执走事件（`EVT_RESULT` + `reqId`），不做 per-call 完成回调 |
| D3 | `error.name` 稳定字符串 + `code` 为 `AGENTXX_FFI_ERR_*`，SDK 按名字判定 |
| D4 | 事件 payload 所有权交给回调: 结构体与正文是**一块**堆内存, 回调 `agentxx_ffi_free(payload)` 释放整块（`NativeCallable.listener` 这类异步投递因此可用） |
| D5 | 删除 `agentxx_ffi_event_queue_*` 四个符号 |
| D6 | wire 请求/应答补 `reqId` 回显（并发调用的前提） |
| D7 | 新增通用 `WireAck`：**所有命令类请求**都有回执（含 `hosttool.register`） |
| D8 | 删 `send_wire` 与 `EVT_WIRE`（同进程同版本，全部 op 化） |
| D9 | `input.send` 直接用自己的 reqId 当 wire `requestId` |
| D10 | `stop` 异步 + `EVT_STOPPED`；`destroy` 仅在 Created/Stopped 成功 |
| D11 | `get_capabilities` / `drain_logs` 保持同步（不跨线程等待） |
| D12 | 不做兼容层，API 版本抬到 4 |
| D13 | 提供 `agentxx_ffi_call_cancel`（只停止等待，不撤回已提交操作；`settled` CAS 保证恰好一个回执） |
| D14 | 每个 op 都有回执；`select_model` / `set_full_auth` 因此能确认是否真的生效 |
| D15 | 每个 op 有默认超时（`compact` 不限 / `switch` 60s / `model.add`、`model.remove`、`fs.list_dir` 30s / 其余 10s） |
| D16 | `session.compact` 的完成由中间件新增的 `SummarizationCompactDone` 事件驱动 |
| D17 | 性能默认值：合批**默认关闭**（SDK 只给开关与建议值）、首屏只同步尾窗（默认 200）、headless 不序列化；去日志后台线程 |

## 结论

原来的预期都成立了，另外多出两条实施中才暴露的约束。

- **只有一种调用形态**: 提交拿 `reqId` + `EVT_RESULT` 回执；本地同步入口只剩
  `get_capabilities` / `drain_logs` / 内存与版本函数——它们不跨线程等待。
- **每个受理的 reqId 恰好一条回执**: 成功 / 失败 / 超时 / 取消 / 收尾中断都算。
  `ffi_c_api` 463 条断言全绿（含并发 20 个在飞、取消后迟到应答被丢弃、回调内调 API）。
- **顺序只靠 await**: `model.add → model.select → model.list` 这类链在 Dart 侧与 C++
  侧都能稳定重现，不需要额外同步原语。
- **性能**: 1000 次跨 C 边界往返 55~63 us/op（约 1.6~1.8 万 op/s）；合批把 66 条事件压到 24 条、
  轮次耗时降到 1/5；首屏尾窗 200 条 vs 全量 240 条省 7.9 KiB 同步体积。数据见
  [benchmark.md](../../../docs/zh-cn/design/benchmark.md) §1 与 `resource/benchmark/2026-11-25_750170bc_windows-resource-ffi/`。
- **两条实施中发现的约束**（已写进代码注释与 `ffi.md`）:
  1. 服务端“取数”→“回执”的协程里，回执 id 必须在取数**之后**补——先把 `resp.reqId`
     设好再整体赋值会被取数结果覆盖，回执带 `reqId=0` 会被当成广播丢掉；
  2. 事件 payload 的结构体不能放在回调返回前就失效的栈上——`NativeCallable.listener`
     是异步投递，结构体必须与正文同在一块堆内存里活到回调释放它。
