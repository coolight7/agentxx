# 后台任务段显示拆分 + 状态刷新修复

- 难度: B
- 类型: 界面调整 + 状态刷新修复
- 时间: 2026-11-09
- 需求:
```md
请修改 tui 后台任务 显示拆分为 |- 运行 n\n|- xxx、|- 排队 n\n|- xxx、|- 完成 n\n|- xxx,
所有分类的 |- xxx 列表项颜色都采用 hint；|- xxx 列表项的前置标签移除，仅完成的列表项
才有前置标签提示，并采用单一符号表示；并修复后台任务队列的状态变化更新显示不及时的问题
```

## 根因（两部分）

1. **状态变化没人推**。界面的「后台任务」段只是 `WireContextStats.asyncWorks` 的一份
   快照, 而推送点只有握手 / 切会话 / 轮次结束 / 改名这几种。后台任务最典型的场景是
   "轮次结束了、任务还在跑", 这段时间台账怎么变都不会推 — 界面上的排队、运行、完成
   和已跑时长全都停在最后一次推送时的样子。
2. **推了也会被抹掉**。流式期间的按秒 tps 推送 (`EventBridge::pushTpsIfDue`)、llm 节点
   结束、压缩完成这三处用的是 `toWireContextStats`(**不填** `asyncWorks`), 客户端按
   整份覆盖, 于是轮次进行中每推一次就清空一次这一段。
3. 顺带: 界面条目是"先按 `asyncId` 字典序取 10 条再展示", 已完成的 linger 条目多时会把
   正在运行的挤掉。

## 做法

- **单点组装**: 新增 `io::makeWireContextStats(ctx, sessionId, stats, tps)`
  (`agent/lib/src/agent/io/context_stats_wire.cpp`), 在 `toWireContextStats` 之上填
  `asyncWorks`; 四个推送点全部改走它。条目挑选改成 运行 → 排队 → 完成 (完成按结束时间
  倒序), 先取 100 条再截到 10 条。
- **变化通知**: `SessionInputSink::onAsyncWorksChanged()` (默认空实现) 由
  `AsyncWorkManager` 在 提交 / 起跑 / 终态 / 排队中取消 时调用 (投递到容器 io 线程,
  再按 `creatorSessionId` 找会话的驱动端点)。
- **端点刷新**: `SessionServerAgentIO::onAsyncWorksChanged()` 合并 100 ms 内的多次变化后
  重推一次统计; 只要还有排队/运行中的条目就 1 秒续推 (已跑时长跟着走), 全部终态后停;
  无存活客户端时不排定时器。
- **界面**: 后台任务段拆成 `|- 运行 n` / `|- 排队 n` / `|- 完成 n` 三段 (分组头恒显示),
  条目 `|- <id> <名字> [时长]` 统一 hint 色、不再带状态标记; 只有完成段带单字符
  标记 (完成 `#` / 失败 `x` / 取消 `-`); 排队条目不再显示排队位 (顺序即先后)。

## 结论

- 现在的刷新由**台账驱动**: 状态一变就推, 运行期间每秒一次, 轮次空闲也照常刷新;
  完成态在 `async_work.linger_ms` 内继续带出, 到期后在下一次推送里消失。
- 界面形态与颜色按需求改完: 三段分组头 (`|- 运行 n` 等, normal 色) + 全 hint 色的
  `|- xxx` 条目, 条目前不再有 `>` / `.` / `ok` 标记, 完成段用单字符 `#` / `x` / `-`,
  排队条目不显示排队位 (后续追加的调整, 见 `work.md`)。
- 验证: `async_work` 131 / `tui_widget` 206 / `event_bridge` 223 / `summarization` 454 /
  `event_hub` 268 / `event_delivery` 48 / `async_tools` 99 / `plugin_event` 124 /
  `wire_roundtrip` 262 / `wire_schema` 1325 / `boundaries` 19 / `remote_agent` 602 /
  `agent_host` 181 / `jsonl_mode` 69 / `plugin_runtime` 736 / `client_plugins` 747 /
  `tui_form` 84 全部通过 (debug + ASan)。明细见 `work.md`。

## 偏差

- 需求只提"状态变化更新不及时", 实施时一并修了 tps/压缩那三处推送不带 `asyncWorks`
  导致显示被清空的问题 (同一现象的另一半原因); 不修的话新推送很快又会被抹掉。
- 需求说"单一符号", 实现为**每个条目一个字符**、按结果区分 (`#`/`x`/`-`)。若希望
  完成段统一一个符号, 改 `tui_sidebar_content.cpp` 的 `asyncWorkMark()` 一处即可。

## 涉及文件

- `agent/lib/include/agentxx/agent/io/context_stats_wire.h`、
  `agent/lib/src/agent/io/context_stats_wire.cpp` (新增)、
  `agent/lib/src/agent/io/session_server_agent_io.cpp`、
  `agent/lib/include/agentxx/agent/io/session_server_agent_io.h`
- `agent/lib/include/agentxx/agent/session_input_sink.h`、`agent/lib/src/agent/async_work.cpp`
- `agent/lib/src/event/event_stream.cpp`、`agent/lib/src/middlewares/summarization.cpp`
- `agent/client/src/io/tui/tui_sidebar_content.cpp`、`agent/client/src/io/tui/tui_i18n.cpp`、
  `agent/client/src/io/tui/agent_tui.cpp`
- 测试: `agent/test/client/test_tui_widget.cpp`、`agent/test/core/test_async_work.cpp`
- 文档: `docs/zh-cn/design/tui.md` §2.7、`docs/zh-cn/design/events.md` 第 8 节、`AGENTS.md`
