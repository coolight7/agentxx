# 后台任务段显示拆分与刷新修复

## 目标

1. TUI 信息侧边栏「后台任务」段拆成三段显示：`|- 运行 n` / `|- 排队 n` / `|- 完成 n`，
   每段下面是 `|- xxx` 列表项；所有列表项统一用 hint 色，列表项前不再带状态标记，
   只有「完成」段的条目带一个单字符标记。
2. 修复后台任务状态变化（排队 / 起跑 / 终态）在界面上更新不及时的问题。

## 现状调查（文件 / 函数级）

### 客户端展示

- `agent/client/src/io/tui/tui_sidebar_content.cpp` 的 `renderInfoSidebar()`
  「后台任务段」：从 `ctx_.frameState->asyncWorks` 取快照，echo 一行汇总
  `|- 运行 n / 排队 m`，再逐条 `|- {mark} {id} {name} {时长}`；标记 `>` `.` `ok` `x` `-`
  按状态区分，各状态颜色不同（排队/完成 hint、失败 error、运行 normal）。
- `agent/client/src/io/tui/framework/tui_state.h` 的 `TUIAsyncWorkItem`（id/kind/name/
  state/elapsedMs/queueAhead）。
- `agent/client/src/io/tui/agent_tui.cpp` 的 `onContextStats()`：整份覆盖 `st.asyncWorks`
  后 `postRedraw()`。
- i18n：`agent/client/src/io/tui/tui_i18n.cpp` 的 `info.asyncWorks` / `info.asyncWorksEmpty` /
  `info.asyncWorkSummary` / `info.asyncWorkAhead`。

### 数据来源

- `agent/lib/src/agent/io/session_server_agent_io.cpp` 的 `sendContextStats()`：把
  `asyncWorkItemsOf(host->asyncWorksJson({}, sessionId, true, 10), 10)` 填进
  `WireContextStats::asyncWorks`，是本段唯一的数据来源。
- `asyncWorkItemsOf()`（同文件静态函数）把台账 JSON 转成 `WireAsyncWorkItem`，
  用 `startedAtMs/finishedAtMs` 现算 `elapsedMs`。
- `agent/lib/include/agentxx/agent/io/context_stats_wire.h` 的 `toWireContextStats()`
  只填占用与用量，**不填** `asyncWorks`。

### 推送时机（问题根因）

| 推送点 | 位置 | 带 asyncWorks |
| --- | --- | --- |
| 握手 / 切会话 / 轮次结束 / 改名 | `SessionServerAgentIO::sendContextStats()` | 是 |
| 流式期间按秒推 tps | `EventBridge::pushTpsIfDue()` | **否（空数组）** |
| llm 节点结束 | `EventBridge::handleLLMOutput` 尾段 | **否（空数组）** |
| 压缩完成 | `SummarizationMiddlewareHandle::compactSessionContext` | **否（空数组）** |

两个问题：

1. **有流式推送时本段被清空**：客户端 `onContextStats` 是整份覆盖，tps 推送不带
   `asyncWorks`（空数组），于是轮次进行中每推一次 tps，界面上的后台任务就消失一次，
   直到下一次 `sendContextStats` 才回来 —— 表现为「闪一下 / 一直显示( 无 )」。
2. **状态变化本身不推送**：后台任务的提交 / 起跑 / 终态只写台账，没有任何推送触发点。
   轮次空闲时（后台任务最典型的场景：轮次已结束、任务还在跑）界面完全不动，
   已跑时长也永远停在最后一次推送时的值。

另外 `AsyncWorkManager::listJson()` 按 `asyncId` 字典序截断（`async-10` 排在 `async-2` 前），
先取 10 条再给界面，完成态条目多时会把正在运行的挤掉。

## 方案

### 1. 统计组装单点化（`context_stats_wire`）

新增 `agent/lib/src/agent/io/context_stats_wire.cpp`：

- `WireContextStats io::makeWireContextStats(ctx, sessionId, stats, tps)`：
  调 `toWireContextStats` 后再填 `asyncWorks`（经 `ctx->host` 取
  `asyncWorksJson`；无容器 / 无会话时不填，即保持空数组）。
- 把 `asyncWorkItemsOf()` 从 `session_server_agent_io.cpp` 搬到这里，并改为
  **先取多（100 条）再按状态优先挑选**：运行 → 排队 → 完成（完成按结束时间倒序），
  最后按界面上限（10 条）截断。界面只关心「正在发生什么」，完成态只是附带信息。
- 四个推送点统一改成 `makeWireContextStats(...)`，`asyncWorks` 在任何一次推送里
  都不会丢。

### 2. 状态变化主动推送

- `SessionInputSink` 追加可选钩子 `onAsyncWorksChanged()`（默认空实现）：端点据此
  重推一次上下文统计。语义等于「驱动这个会话的端点，界面数据需要刷新了」。
- `AsyncWorkManager` 在四处状态变化后经 `asio::post` 到容器 io 线程，按
  `creatorSessionId` 找会话、取 `Session::inputSink` 调该钩子：
  `submit()`（受理入队）、`pump()`（排队 → 运行，同时补位）、`runner()`（终态）、
  `cancel()`（排队中被取消）。
- `SessionServerAgentIO::onAsyncWorksChanged()`：合并推送（同一窗口内的多次变化只推
  一次），并**在还有排队/运行中的任务时按 1 秒续推**，让「已跑时长」跟着走；
  任务全部终态后推最后一次即停。无存活客户端时不排定时器。

### 3. 客户端展示

`renderInfoSidebar()` 的后台任务段改为：

```
后台任务
|- 运行 1
|- async-7 research 00:42
|- 排队 0
|- 完成 2
|- + async-5 worker 00:31
|- x async-3 worker 00:02
```

- 三个分组头恒显示（count 可以是 0），列表项只在有内容时出现。
- 所有 `|- xxx` 列表项统一 `hintColor`；分组头保持 `normalColor`（与原汇总行一致）。
- 列表项前不再有 `>` / `.` 标记；只有「完成」段的条目带单字符标记：
  完成 `+`、失败 `x`、取消 `-`（ASCII，窄侧边栏不会被截成半个字符）。
- 排队条目仍显示 `(前面 N 个)`，运行 / 完成条目显示已跑时长。
- i18n：`info.asyncWorkSummary` 拆成 `info.asyncWorkRunning` /
  `info.asyncWorkQueued` / `info.asyncWorkDone`。

## 边界与兼容

- `WireContextStats.asyncWorks` 是老对端不认识的追加字段（空数组即「没有任务」），
  协议不变。
- 无容器（单机库调用 / 未挂载）或无会话时 `asyncWorks` 保持空数组，界面显示 `( 无 )`。
- 队列上限 10 条不变；只是挑选顺序改为运行/排队优先。
- 完成态条目的 linger（默认 5 分钟）结束后由管理器清扫，界面在下一次推送时更新
  （终态推送 + 之后任何一次统计推送），不额外为清扫起定时器。

## 分步实施

1. `context_stats_wire.{h,cpp}`：`makeWireContextStats` + `asyncWorkItemsOf` 迁移与排序；
   四个推送点切换。
2. `session_input_sink.h` + `async_work.cpp`：变化通知。
3. `session_server_agent_io.{h,cpp}`：`onAsyncWorksChanged` 合并推送 + 按秒续推。
4. `tui_sidebar_content.cpp` + `tui_i18n.cpp`：三段显示。
5. 测试：`test_tui_widget` 的后台任务段用例按新排版断言；新增通知链路用例
   （探针 `SessionInputSink` 收到 `onAsyncWorksChanged`）。

## 验证

- debug 构建（`agent/script/windows_debug_build.bat`）。
- `agentxx_test tui_widget async_work event_bridge remote_agent wire_roundtrip`。
- 手工检查：`agentxx` TUI 里 Info 面板「后台任务」段分段显示、颜色为 hint，
  轮次进行中不再闪成 `( 无 )`，后台任务排队 → 运行 → 完成的切换与已跑时长会变。

## 风险

- 按秒续推只在「有排队/运行中的任务」期间发生，且只推一条小消息，开销与流式期间的
  tps 推送同级；任务结束后自动停止。
- `AsyncWorkManager` 已持锁的位置只捕获（creator 弱引用 + 会话 id），通知在锁外投递，
  不会引入重入死锁。
