# 方案：异步结果交付

## 1. 背景

后台任务（`agentxx_async_run`）的结果有两条通路：

- `notify`（默认 true）：提交时在事件中枢登记一条针对 `agentxx.agent.async.done` 的
  定向等待，任务收尾发事件时由中枢投递成一条会话输入（`[Event] ...`）；
- `agentxx_async_await`：自己再登记一条等待（`delivery=none`）或直接读任务表，
  把正文作为工具结果交回。

同一段正文因此有两个出口，为了不重复而引入了一套仲裁：任务条目上的
`resultTaken`、`claimResult`/`releaseResult`、中枢里的 `Suppressed`、以及"投递失败
归还 / await 超时归还"两条回退。它带来三个问题：

1. 正文来源不一致：终态分支读任务表（`result_max_bytes` 截断），等待分支读事件值
   （全文）；
2. await 依赖中枢：25 ms 轮询、waits 限额、`match`/`replay`/`delivery:none` 特例；
3. 仲裁跨三个模块，且存在"await 领走 → 通知被抑制 → await 超时"的丢结果窗口。

## 2. 现状调查（改前）

- `AsyncWorkManager::submit`：`notifyDelivery` 非空时 `hub->callSync(watch(...))`
- `EventHub::flushWait`：命中等待 → `contentText` → `renderEventText` → `deliver`
- `EventHub::deliver`：前置检查 → 领取投递权（async 通知）→ 按会话状态投递 → 失败归还
- `AsyncAwaitTool::execute_async`：claim → 终态读表 / 未终态 `hub->awaitOnce`（25 ms 轮询）
- `finishEntry`：写任务表终态 → 发事件 → `delivered=true`

## 3. 方案

### 3.1 票据（一次性，无持久化）

```cpp
struct AsyncResultTicket {           // agent/lib/include/agentxx/agent/async_work.h
    std::shared_ptr<AsyncWake> wake;
    std::atomic<bool>          ready{false};
};
```

- 条目新增 `std::vector<std::weak_ptr<AsyncResultTicket>> waiters;`
  （受 `AsyncWorkManager::mutex_` 保护，和任务表同一把锁 → 没有跨锁竞态）；
- `waitTerminal(asyncId, timeoutMs, token)`：锁内查条目——
  不存在/已终态 → `AlreadyTerminal`；否则挂票据，锁外分片等待
  （`AsyncWake::wait(min(100ms, 剩余))`，其间检查 `ready` / 取消 / 截止）；
- 等待方析构时 `removeWaiter` 摘票（终态时票据已被取走，摘不到也无害）；
- `finishEntry`：锁内 `waiters.swap(entry.waiters)`；锁外逐个
  `ready=true` + `wake->signal()`；`woken > 0` → 本次不自动投递。

### 3.2 直接交付（事件中枢只保留投递决策段）

```cpp
// finishEntry（锁外）
agentxx::events::EventWait synthetic;          // 只为复用同一套渲染
synthetic.spec.event = evName;
synthetic.spec.mode  = "first";
synthetic.spec.ref   = entryId;
const auto contentText = hub->contentText(sessionId, evName, {value});
EventDelivery delivery;
delivery.sessionId     = sessionId;
delivery.text          = renderEventText(synthetic, "value", contentText);
delivery.source        = "event:" + evName;   // 保持旧 source（界面/协议零改动）
delivery.delivery      = notifyDelivery;      // next-step
delivery.unpausePaused = true;                // 后台结果必须进上下文
hub->deliver(delivery, &reason);
```

`deliver` 内部仍是唯一的投递决策实现（空闲开新一轮 / 轮内插下一次模型调用前 /
无驱动端落 pendingInputs / headless 起一轮 / 背压丢弃），删除的只有"领取投递权"那一段。

### 3.3 await 重写

```cpp
entry = requireOwnedEntry(...);
if (terminal) return renderEntryDetail(entry);        // 纯读，可重复
... 延时/超时参数 ...
outcome = co_await host->waitAsyncWorkResult(id, timeoutMs, token);
entry   = requireOwnedEntry(...);                     // 醒来统一读任务表
if (terminal) return renderEntryDetail(entry);
if (outcome == Canceled) throw CancelledException;
return renderAwaitTimeout(spec, timeoutMs);
```

### 3.4 删除

`AsyncWorkEntry::resultTaken`、`claimResult`/`releaseResult`、
`AgentHost::claimAsyncResult`/`releaseAsyncResult`、`AsyncResultClaimGuard`、
`renderResultAlreadyDelivered`、`DeliverResult::Suppressed`、`EventHubStats::suppressed`、
hub 的 claim 段与两个转发函数、`submit` 里的 notify watch。

## 4. 并发与边界

| 场景 | 行为 |
|---|---|
| 票据先挂，任务后结束 | 不投递，await 读表给正文（1 份） |
| 任务结束并投递完，await 才来 | 注册发现已终态 → 读表给正文（2 份，工具语义） |
| 挂票与终态同时 | 同一把锁二选一：要么拿到票（1 份），要么走纯读（2 份） |
| await 超时 | 摘票；下一轮读表仍是运行态 → 超时文本；若恰已终态 → 给正文 |
| 会话取消 | 分片内看到 `is_cancelled` → 摘票 → 抛取消（任务照常跑） |
| 同一任务两个 await | 两张票据都在列表里，收尾全部唤醒，本次投递取消一次 |
| 投递失败 | 记 WARN + `dropped` 计数，不重试；await/query 仍可读任务表 |

## 5. 分步实施

1. 抽出 `AsyncWake` 到 `agent/async_wake.h`（票据与运行循环共用）；
2. `async_work.{h,cpp}`：票据 + `waitTerminal` + `finishEntry` 直接交付，删 claim；
3. `agent_host.{h,cpp}`：`waitAsyncWorkResult`；
4. `hub.{h,cpp}`：删 `Suppressed` / claim 段；
5. `async_tools.{h,cpp}` + `prompt.cpp`：await 重写与文案；
6. 测试与文档。

## 6. 验证

- 构建：`agent/script/windows_debug_build.bat`（Debug，含 client/test）；
- 测试：`async_tools` / `async_work` / `event_hub` / `event_tools` / `event_delivery` /
  `share_store` / `input_delivery` / `boundaries`；
- 关键用例：运行中 await → 0 条 `[Event]` 结果输入；结束后再 await → 再给一份正文；
  `notify:false` → 无自动投递、await 照常给正文。

## 7. 风险

- 票据列表与任务表共锁，`finishEntry` 的锁外唤醒要保证 `ready` 先写、`signal` 后发；
- 取消只按 100 ms 分片观察（取消令牌无回调接口，且不能与引擎共用 asio cancellation slot）；
- `[Event]` 输入形态沿用旧的，`finishEntry` 里的渲染必须与中枢一致
  （用 `contentText` + `renderEventText`，不自己拼字符串）；
- 交付发生在 `finishEntry`（容器 io 线程；排队中取消可能来自任意线程），
  `EventHub::deliver` 的跨线程分支会把投递 post 回 io 线程。
