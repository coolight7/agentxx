# 设计方案

## 1. 背景

- `agentxx_async_run` 把一次工具调用放到后台跑，结果以事件
  `agentxx.agent.async.done`（值里 `kind=tool` + `content`）投递回创建者会话。
- 事件中枢的 `[Content]` 段有长度限制（`AgentConfig::eventContentLimitLength`
  = `toolcallSummaryLimitOutputLength` 2K 字符 与 `event.value_max_bytes` 32K 取小），
  超过就整段转 share store 并留取回提示。
- 工具本身可能已经裁剪过自己的输出（`agentxx_execute_command` 自己截断 30K 并写
  share store；filesystem/codegraph 一类声明了 `AUTO_SUMMARY`），于是同一段输出在
  事件层又被套一层提示，模型要多取一次，share store 里也多了重复条目。

## 2. 现状调查（文件 → 函数）

| 位置 | 现状 |
|---|---|
| `agent/lib/src/nodes/toolcall.cpp` `ToolcallWrapNode::finalizeToolCall` | 前台工具结果按 `tool->extra["autoSummaryOutput"]` + `toolcallSummaryLimitOutputLength` 压缩 |
| `agent/lib/src/agent/async_work.cpp` `runToolWork` | 后台工具任务直接 `execute_async`，**没有任何裁剪**，原文进事件值 |
| `agent/lib/src/event/hub.cpp` `EventHub::contentText` | 对任何事件值（含 `kind=tool`）做长度压缩 |
| `agent/lib/src/event/hub.cpp` `EventHub::deliver` | 空闲投递走 `sink->submitTurnInput(text, source, d.unpausePaused)`，`unpausePaused` 默认 false |
| `agent/lib/src/agent/io/session_server_agent_io.cpp` `run()` 轮末 | 失败/中断 → `setQueueState(Paused)`，驱动循环 `isQueueHalted()` 后不再取队列 |
| `agent/lib/src/agent/base_agent.cpp` `rolloverLeftoverNextStepInputs` | 轮末把残留 next-step 以 `unpausePaused=false` 提升为下一轮输入（此时队列还没置暂停） |

## 3. 方案

### 3.1 工具输出压缩收敛成一处

`agentxx/util/share_store_offload.h` 新增：

```cpp
bool offloadToolResultIfOversized(agentContext, toolExtra, sessionId, ToolResult& result);
```

命中条件 = `toolExtra["autoSummaryOutput"] == "true"` 且正文 ≥
`toolcallSummaryLimitOutputLength`；压缩本身仍走 `offloadShareStoreIfOversized`
（提示格式、按行/按字符两种截断、落地失败退回原文全部沿用）。

调用点：`finalizeToolCall`（前台，保留原有 `sessionId` 断言）与 `runToolWork`
（后台，`sessionId` 取 `spec.creatorSessionId`，工具 `extra` 在移交协程前拷贝一份）。

### 3.2 事件层只裁非工具结果

`EventHub::contentText`：事件名落在 `agentxx.agent.async.` 且所有值 `kind == "tool"`
时直接返回渲染文本，不做整段卸载。其余情况不变（含 `agentxx_async_await` 用的同一处渲染）。

配套：`agentxx_async_await` 声明 `autoSummaryOutput=true`，让"等结果"这条路径仍有
同一阈值的压缩（它没有 `kind=tool` 之外的保护）。

### 3.3 后台结果一定进上下文

- **空闲投递**：`EventHub::deliver` 判定事件名属于 `agentxx.agent.async.*` 时，
  `submitTurnInput` 的 `unpausePaused` 传 true（会话因上一轮失败/取消停在暂停态也开新一轮）。
- **轮内提交（轮末兜底）**：`MessageQueueItem` 新增 `resumeRequested`；
  `pushMessageQueueItem` 多一个 `resumeIfPaused` 参数，只有 `submitTurnInput`
  （事件投递）会置位；`rolloverLeftoverNextStepInputs` 在来源里有
  `event:agentxx.agent.async.*` 时传 true。驱动循环轮末按"有该标记就不置暂停"处理。
- 其他事件仍按原语义（默认不解除暂停，需要唤醒的由 `EventWaitSpec::unpausePaused` 显式要求）。

## 4. 边界与兼容

- 不为压缩新增配置键；阈值仍由 `toolcallSummaryLimitOutputLength` /
  `event.value_max_bytes` 决定。
- 后台工具任务若工具**没有**声明 `autoSummaryOutput`（如 `agentxx_execute_command`
  之外的多数插件工具），其结果与前台调用一致：不再被事件层裁剪。要压缩就让工具声明该标记。
- 暂停态的原始语义（积压消息不自动继续）只对"用户输入 / 其它事件"保持；
  后台结果这一族例外。
- 不改 wire 协议：`MessageQueueItem::resumeRequested` 只在服务端内部使用。

## 5. 分步实施

1. `share_store_offload.{h,cpp}` 加共用压缩函数；
2. `toolcall.cpp` / `async_work.cpp` 改用它；
3. `hub.cpp`：`contentText` 跳过 `kind=tool`，`deliver` 对 async 结果要求解除暂停；
4. `conversation_types.h` / `session_server_agent_io.{h,cpp}` / `base_agent.cpp`：
   `resumeRequested` 全链路；
5. `async_tools.cpp`：`agentxx_async_await` 打开 `autoSummaryOutput`；
6. 测试 + 文档。

## 5.1 第二轮：结果正文只进会话一次（await 与 notify 去重）

**问题**：`notify`（默认 true）在提交时就登记了自动通知等待；`agentxx_async_await`
又登记一条自己的等待。同一条结果因此有两个消费者：

- 任务还在跑时 await：值一到，两个等待同时满足 —— 通知把 `[Event] ... [Content]`
  插进会话，await 把同一段正文当工具结果返回 → 上下文里两份；
- 任务已结束再 await：await 走"直接看台账"分支，`renderEntryDetail` 里带
  `result:` 正文；如果通知早已投递，同样两份。

**方案：任务表条目上记一个"正文已放进会话"的布尔标记（只放一次）**

- `AsyncWorkEntry::resultTaken`(布尔) + `AgentHost::claimAsyncResult` /
  `releaseAsyncResult` 是唯一入口（`asyncWorkJson` 里以 `resultTaken` 暴露给测试与诊断）；
- **谁要放正文谁先领**：await 在拿结果前领（终态与等待共用一处），
  领不到 = 正文已经在会话里 → 返一句回执（`renderResultAlreadyDelivered`：状态 +
  "已由自动通知或更早的一次 await 放进会话" + `agentxx_async_query` 取回建议），
  不贴正文；
- await 超时 / 被取消 / 抛异常 → 守卫（`AsyncResultClaimGuard`）归还，
  自动通知照常投递（结果不会因为等过而丢）；
- **自动通知在投递前领**：`EventHub::deliver` 认出"框架自己的通知等待"
  （`EventDelivery::owner == async_work` + `agentxx.agent.async.*` + `ref=asyncId`），
  领不到就整条不投递（`DeliverResult::Suppressed` + `stats.suppressed`，只记 info 日志，
  不算 dropped）；投递结果是 `Queued`/`Deferred` 才留着标记，被拒/丢弃时归还；
- 任务表条目被 linger 清扫后查不到时按"领到"处理（没法记标记时让内容照常出去）。

**边界**：`agentxx_async_query {id}` 仍是显式"看任务表"，返回副本，不受标记影响；
插件自己订阅 `agentxx.agent.async.done` 的等待不属于这条规则（owner 不同）——
插件提交的任务本来就不走这条路。

## 6. 验证

- `event_hub`：`kind=tool` 长内容无提示且 `kind=agent` 有提示；空闲投递的
  `unpause` 标记（async 结果 true、普通事件 false）。
- `async_tools`：后台大输出工具（声明/不声明 `autoSummaryOutput` 各一次）投递文本里
  卸载提示恰好 1 个 / 0 个。
- `event_delivery`：失败轮次后暂停的队列收到 async 结果仍开新一轮；取消一轮 +
  残留 next-step（来源 async）仍被执行。
- `async_tools`：后台大输出工具（声明/不声明 `autoSummaryOutput` 各一次）投递文本里
  卸载提示恰好 1 个 / 0 个；`test_await_single_delivery`（跑的时候 await：正文只出现在
  工具结果里、`[Event]` 输入 0 条、`stats.suppressed >= 1`、任务表 `resultTaken=true`、
  再 await 一次给回执）；`test_await_after_notify_returns_receipt`（通知先到：await 给回执且
  `[Event]` 只有 1 条；`notify:false` 的任务 await 照常给正文）。
- 回归：`event_tools` / `async_work` / `share_store` / `input_delivery` /
  `session_admin` / `session_sync` / `interrupt_ui` / `event_subagent` /
  `message_supplement` / `view_replica` / `misc_fixes` / `assembly_snapshot` /
  `plugin_runtime` / `prompt_stability`。

## 7. 风险

- 后台结果唤醒暂停会话：取消后仍会因结果再开一轮。这是"结果必须进上下文"的代价，
  用户可用 `notify:false` 关掉自动投递。
- 事件层不再兜底未声明 `autoSummaryOutput` 的工具输出长度：与前台行为一致，
  但单条可能很长（仍受 1 MB 发布硬上限约束）。
