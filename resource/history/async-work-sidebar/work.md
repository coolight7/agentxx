# 实施记录

## 进度

- [x] 计划 (`plan.md`)
- [x] `context_stats_wire` 单点组装 (新增 `agent/lib/src/agent/io/context_stats_wire.cpp`)
- [x] 四个统计推送点切换到 `makeWireContextStats`
- [x] `SessionInputSink::onAsyncWorksChanged` + `AsyncWorkManager` 四处变化通知
- [x] `SessionServerAgentIO::onAsyncWorksChanged` 合并推送 + 按秒续推
- [x] TUI 后台任务段拆三段 + 颜色 + 单字符标记 + i18n 键
- [x] 测试: `tui_widget` 用例重写、`async_work` 新增通知链路用例
- [x] 构建 + 跑测试
- [x] 文档: `docs/zh-cn/design/tui.md` §2.7、`events.md` 第 8 节、`AGENTS.md`

## 各步改动与验证

### 1. 统计组装单点化

- 新增 `agent/lib/src/agent/io/context_stats_wire.cpp`
  - `io::makeWireContextStats(ctx, sessionId, stats, tps)`: 先 `toWireContextStats`,
    再经 `ctx->host` 取 `asyncWorksJson` 填 `asyncWorks`; 无容器/无会话 id 时保持空数组
  - `asyncWorkItemsOf()` 从 `session_server_agent_io.cpp` 迁入, 并改成"先取 100 条,
    按 运行 → 排队 → 完成 (完成按 `finishedAtMs` 倒序) 稳定排序, 再取前 10 条"
- `agent/lib/include/agentxx/agent/io/context_stats_wire.h`: 声明 `makeWireContextStats`
- 推送点切换 (`toWireContextStats` → `makeWireContextStats`):
  - `SessionServerAgentIO::sendContextStats` (`session_server_agent_io.cpp`)
  - `EventBridge::pushTpsIfDue` / `EventBridge::handleLLMOutput` 尾段 (`event_stream.cpp`)
  - `SummarizationMiddlewareHandle::compactSessionContext` (`summarization.cpp`)
- 验证: 构建通过; `agentxx_test event_bridge summarization remote_agent wire_roundtrip`
  全绿 (见下方结果表)

### 2. 台账变化通知

- `agent/lib/include/agentxx/agent/session_input_sink.h`: 新增
  `virtual void onAsyncWorksChanged() {}` (默认空实现, 老端点/测试探针不受影响)
- `agent/lib/src/agent/async_work.cpp`
  - 新增文件内 helper `postAsyncWorksChanged(host, creator, sessionId)`:
    `asio::post` 到容器 io 线程 → 按会话 id 取 `Session::inputSink` → 调钩子
    (调用方可能在任意线程, 而 `SessionsManager` / `Session::inputSink` 只允许 io 线程访问)
  - 调用点: `submit()` (入队)、`pump()` (排队→运行, 锁内取创建者锁外通知)、
    `runner()` (终态)、`cancel()` (排队中被取消)
- 验证: `agentxx_test async_work` 的 `test_works_changed_notification` (新增)

### 3. 端点合并推送

- `agent/lib/include/agentxx/agent/io/session_server_agent_io.h`
  - `onAsyncWorksChanged() override`
  - 私有 `scheduleAsyncWorksStatsPush(delay)` / `hasActiveAsyncWorks()`
  - 成员 `asyncWorksStatsTimer_` (构造时建, 与 `collectTimer_` 同一模式) / `asyncWorksStatsPending_`
- `agent/lib/src/agent/io/session_server_agent_io.cpp`
  - `onAsyncWorksChanged()`: `asio::dispatch(ex_, ...)` 回到端点执行器
  - `scheduleAsyncWorksStatsPush`: 无存活客户端或已停止时不排; 已排定时忽略 (合并);
    到期 `sendContextStats()`, 若 `hasActiveAsyncWorks()` 则 1 秒后续排, 否则停
  - `stopImpl()` 取消该定时器
  - 常量 `kAsyncWorksChangeDelayMs = 100` / `kAsyncWorksTickMs = 1000`
- 验证: `agentxx_test remote_agent` (真实 server/client 两进程以内) 与 `jsonl_mode` 全绿

### 4. 客户端展示

- `agent/client/src/io/tui/tui_sidebar_content.cpp`
  - 新增文件内 `asyncWorkMark(state)`: 完成 `+` / 失败 `x` / 取消 `-`
  - 后台任务段改为三段: 分组头 `|- 运行 n` / `|- 排队 n` / `|- 完成 n` (恒显示, normal 色),
    条目 `|- <id> <名字> [时长/排队位]` (完成段多一个单字符标记), 条统一 `hintColor`
- `agent/client/src/io/tui/tui_i18n.cpp`: `info.asyncWorkSummary` 拆成
  `info.asyncWorkRunning` / `info.asyncWorkQueued` / `info.asyncWorkDone`
- `agent/client/src/io/tui/agent_tui.cpp`: `onContextStats` 的后台任务段注释同步
- 验证: `agentxx_test tui_widget` 的 `test_info_sidebar_async_works`

### 5. 测试

- `agent/test/client/test_tui_widget.cpp`
  - 新增 `renderScrollItemsScreen` / `screenRowOf` 两个小工具 (颜色断言用)
  - `test_info_sidebar_async_works` 重写: 三段计数与顺序、条目文本形态 (无标记 /
    排队位 / 时长)、条目颜色 = `hintColor`、分组头 = `normalColor`、完成段三种单字符标记、
    清空回到 `( none )`
- `agent/test/core/test_async_work.cpp`
  - 新增探针 `AwProbeSink` (实现 `SessionInputSink`, 只数 `onAsyncWorksChanged`)
  - 新增 `test_works_changed_notification`: 提交/起跑 → 有通知; 下发的条目里运行中的
    排在最前; 排队中取消 → 再通知; 终态 → 通知累计 ≥ 4; 端点解绑后不再通知

## 验证结果

| 模块 | 结果 | 耗时 |
| --- | --- | --- |
| `async_work` | passed=131 failed=0 | 75.8 s |
| `tui_widget` | passed=206 failed=0 | 0.3 s |
| `event_bridge` | passed=223 failed=0 | 1.6 s |
| `summarization` | passed=454 failed=0 | 1.2 s |
| `event_hub` | passed=268 failed=0 | 0.7 s |
| `event_delivery` | passed=48 failed=0 | 19.3 s |
| `async_tools` | passed=99 failed=0 | 12.7 s |
| `plugin_event` | passed=124 failed=0 | 0.4 s |
| `wire_roundtrip` | passed=262 failed=0 | 0.1 s |
| `wire_schema` | passed=1325 failed=0 | 0.3 s |
| `boundaries` | passed=19 failed=0 | 14.7 s |
| `remote_agent` | passed=602 failed=0 | 61.0 s |
| `agent_host` | passed=181 failed=0 | 16.8 s |
| `jsonl_mode` | passed=69 failed=0 | 18.6 s |
| `plugin_runtime` | passed=736 failed=0 | 3.2 s |
| `client_plugins` | passed=747 failed=0 | 4.3 s |
| `tui_form` | passed=84 failed=0 | 0.2 s |

构建: `cmake --build agent/build/windows-debug --config Debug`(debug + ASan) 通过。

## 与计划不一致的地方

- 计划里只要求"状态变化推送", 实施时顺带把**四个统计推送点**统一到
  `makeWireContextStats`: 否则流式期间的 tps 推送 (不带 `asyncWorks`) 会把界面上的
  这一段清空 —— 这正是"显示不及时"的另一半原因, 不修的话新推送很快又会被抹掉。

## 后续调整 (用户在同一需求上追加)

- 排队条目不再显示右侧的排队位 `(前面 N 个)`: 排队段的条目顺序就是先后顺序;
  `queueAhead` 仍随线消息下发 (`TUIAsyncWorkItem` 保留字段), 只是界面不展示;
  因此 i18n 键 `info.asyncWorkAhead` 删除;
- 完成状态条目的标记由 `+` 改为 `#` (失败 `x` / 取消 `-` 不变);
- 同步: `agent/test/client/test_tui_widget.cpp` 的断言 (排队无排队位、`#` 标记)、
  `docs/zh-cn/design/tui.md` §2.7、`AGENTS.md`; 重跑 `tui_widget` 全绿。

## 注意事项 / 留给后人的点

- `AgentContext` / `SessionsManager` / `Session::inputSink` 只允许 io 线程访问:
  `AsyncWorkManager` 的通知一律先 `asio::post` 回容器 io 线程, 不要在调用线程直接查会话。
- 续推只在"有排队/运行中的条目"期间发生 (1 秒一次); 全部终态后推一次即停。已完成的
  条目留在台账里 `async_work.linger_ms` (默认 5 分钟), 到期清扫发生在上一次 `submit()`
  / `cancel()` 时, 界面随之在"下一次推送"里消失 (不额外为清扫起定时器)。
- 界面上限仍是 10 条; 挑选顺序改为运行/排队优先 (完成态不会挤掉正在跑的)。
- 完成段用单字符标记区分 完成/失败/取消 (完成 `#` / 失败 `x` / 取消 `-`)。若想
  统一成一个符号, 改 `tui_sidebar_content.cpp` 的 `asyncWorkMark()` 一处即可。
