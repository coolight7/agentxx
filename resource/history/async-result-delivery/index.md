# 异步结果交付：票据 + 直接交付（去掉投递权仲裁）

## 目标

把后台任务结果"回到会话"的路径从事件中枢里拆出来，去掉"同一份正文两个出口互相
抢"的那套补丁（`resultTaken` + claim/release + `DeliverResult::Suppressed`）：

1. 任务收尾时由 `AsyncWorkManager` **直接**把结果交给会话端点（`EventHub::deliver`
   的投递决策段），事件只做广播；
2. `agentxx_async_await` 不再订阅 `agentxx.agent.async.done`，改成在任务条目上挂
   一张**票据**等终态，醒来后读任务表（正文只有一份来源）；
3. await 是"读"不是"消费"：任务已结束时再 await 会再给一份，这是工具语义，不做去重；
4. 不引入插件扩展点（本轮明确不做 before/after 功能埋点）。

## 范围

- `agent/lib`：`async_work.{h,cpp}`（票据、直接交付、去掉 claim）、`agent_host.{h,cpp}`
  （`waitAsyncWorkResult`）、`event/hub.{h,cpp}`（删 `Suppressed` 与 claim 段）、
  `tools/async_tools.{h,cpp}`（await 重写）、`agent/prompt.cpp`（工具文案）、
  `agent/async_wake.h`（从 async_work.cpp 抽出，票据共用）
- 测试：`test_async_tools.cpp`
- 文档：`docs/zh-cn/design/events.md`、`index.md`、`AGENTS.md` 对应段

## 状态

已完成（实现 + 测试 + 文档 + 构建回归）。未改动对外 wire 协议、插件接口表、
配置键；`[Event] ...` 输入文本形态与 `source` 保持不变。

## 关键决策

- **正文单一来源 = 任务表**：票据只做唤醒、不带正文；await 醒来统一读条目渲染。
  之前"事件值全文 vs 任务表 8 KB 截断"两条来源不一致的问题随之消失。
- **一次投递机会**：任务结果在收尾时只决定一次"要不要自动投递"。有票据（有人在等）
  就不投 —— 这不是持久状态，票据在内存里、终态即清空。
- **await 超时/取消不需要"归还"**：票据在等待方析构时摘掉；任务收尾时若已无票据
  就照常投递。因为 await 只在终态之后写结果，不存在"领了但没交出去"的中间态。
- **事件仍然发布**：`agentxx.agent.async.done` 的 value/end 照旧，插件与外部订阅者
  不受影响；`[Event] ... / [Info] ... / [Content] ...` 的输入文本形态保持不变
  （界面、wire、轮末提升的 source 前缀判断都不用改）；`[Info]` 的 `wait` 行改成 `-`。
- **取消看分片**：等待循环用 100 ms 分片检查 `CancelToken::is_cancelled()`
  （取消令牌没有回调接口，且不能与引擎抢同一个 asio cancellation slot）；
  任务真正结束时票据会立即叫醒，分片只是上限。
- **不做插件扩展**：本轮明确不加 before/after 功能埋点（后续要加就挂在
  `finishEntry` 的交付段之前，不影响票据）。

## 结论

- `agentxx_async_await` = 读工具：任务结束就读任务表返回，没结束就等票据；
  不再订阅事件、不再与自动通知抢正文，运行中调用时自动通知那一份不会出现。
- 去重从"三个模块协调 + 两条归还路径"变成"一次内存票据 + 收尾时一个条件"；
  `resultTaken` / claim / release / `Suppressed` 全部删除。
- 回归：2843 条断言全绿（见 work.md）。
