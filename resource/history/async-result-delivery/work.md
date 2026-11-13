# 实施记录：异步结果交付

## 进度

- [x] 抽出 `AsyncWake` 到 `agent/lib/include/agentxx/agent/async_wake.h`（票据共用）
- [x] `async_work.{h,cpp}`：票据 + `waitTerminal` / `removeWaiter`；`finishEntry` 直接交付；
      删除 `claimResult` / `releaseResult` / `resultTaken` / submit 里的 notify watch
- [x] `agent_host.{h,cpp}`：`waitAsyncWorkResult`
- [x] `event/hub.{h,cpp}`：删除 `DeliverResult::Suppressed` / `stats.suppressed` / claim 段
- [x] `tools/async_tools.{h,cpp}`：await 重写（票据 + 读任务表），删 `AsyncResultClaimGuard`、
      `renderResultAlreadyDelivered`、`kAsyncOwner`
- [x] `agent/prompt.cpp`：`agentxx_async_await` 文案改为"有结果就返回"
- [x] 测试 `test_async_tools.cpp`：运行中 await = 0 条事件输入；结束后再 await = 再给一份；
      `notify:false` 照常给正文
- [x] 构建 + 回归（Windows Debug，exit=0）
- [x] 文档：`events.md` §6/§8、`index.md`、`AGENTS.md`

## 每步改了什么 / 怎么验证

### 1. `async_wake.h`

把 `AsyncWake`（完成/取消/到期叫醒一次）从 `async_work.cpp` 的私有实现抽成头文件，
票据与运行循环共用同一份实现；`async_work.cpp` 只留 include。

### 2. `async_work.{h,cpp}`

- `AsyncResultTicket { shared_ptr<AsyncWake> wake; atomic<bool> ready; }`（头文件，供票据
  与测试可见）；
- 条目新增 `waiters`（weak 列表，和任务表同一把锁）；
- `waitTerminal(asyncId, timeoutMs, token)`：锁内判终态并挂票 → 分片等待（100 ms 上限，
  每片检查 `ready` / 取消 / 截止）→ 离开时 `WaiterGuard` 摘票；
- `finishEntry`：锁内写终态并 `waiters.swap(...)`；锁外先叫醒（`ready` 先写、`signal`
  后发），再按 `woken == 0 && notifyDelivery` 决定是否直接交付；
- 交付文本复用 `contentText` + `renderEventText`，`source` 仍是
  `event:agentxx.agent.async.done`，`unpausePaused=true`；
- 日志新增 `toSession=` 与 `awaited=` 两个字段，便于排查"结果没进会话"。

### 3. `agent_host.{h,cpp}`

`waitAsyncWorkResult` 转发到管理器；容器没有管理器时返回 `AlreadyTerminal`（调用方走读表）。

### 4. `event/hub.{h,cpp}`

删 `claimAsyncResultForNotify` / `releaseAsyncResultForNotify`、`deliver` 里的
async 通知判定与 `Suppressed` 返回、`EventHubStats::suppressed` 与 `statsJson` 字段、
`DeliverResult::Suppressed` 枚举值。`deliver` 现在只做投递决策，不再关心"结果是否已被
别处取走"。

### 5. `async_tools.{h,cpp}`

`agentxx_async_await`：终态读表 → 未终态 `waitAsyncWorkResult` → 醒来再读表
（终态给正文；否则取消抛异常 / 超时给说明文本）。删掉 claim/guard/回执渲染。
`agentxx_async_await` 不再依赖事件中枢（`eventHub` 为空也能用）。

### 6. 测试

- `test_await_single_delivery`：运行中 await → 正文来自工具结果、0 条 `[Event]`；
  再 await 一次 → 再给一份正文（仍 0 条事件输入）；
- `test_await_after_notify_returns_body`（原 `..._returns_receipt`）：自动通知已投递
  （1 条事件输入）后 await → 仍返回正文；`notify:false` 的任务照常给正文。

## 遇到的问题 / 注意

- `CancelToken` 只有 `is_cancelled()`（轮询）与 `slot()`（asio cancellation slot）；
  `slot()` 是单槽，和引擎的 in-flight 操作抢同一个槽会互相顶掉，因此等待用
  **100 ms 分片 + 标志检查**，不绑取消槽。
- 票据与任务表共用 `mutex_`，`finishEntry` 可能在任意线程（排队中取消），
  所以 `ready` 用原子、唤醒用 `AsyncWake::signal()`（内部 post 到 io 线程）。
- 交付仍走 `EventHub::deliver`：跨线程调用时它会 post 回 io 线程，返回 `Queued`。
- 交付文本里的 `[Info]` 的 `wait` 行给 `-`（不再编一个不存在的 wait id），
  其余分段与旧文本一致；测试里没有任何对 `wait:` 值的断言。
- 既有失败（与本任务无关）：`session_persistence` 的 `testPersistenceResilience`。

## 验证结果

- 构建：`agent/script/windows_debug_build.bat`（Debug，含 client/test）两次均 exit=0。
- 测试（`--log-level error`）：
  - `async_tools` 178 / `event_hub` 288 / `event_tools` 97 / `event_delivery` 62 /
    `async_work` 138 = **763 全绿**；
  - `share_store` 63 / `input_delivery` 139 / `session_admin` 38 / `session_sync` 30 /
    `interrupt_ui` 338 / `event_subagent` 52 / `message_supplement` 95 / `view_replica` 114 /
    `assembly_snapshot` 38 / `plugin_runtime` 726 / `plugin_broadcast` 73 / `chatbot_task` 138 /
    `config_keys` 217 / `boundaries` 19 = **2080 全绿**。
- `async_tools` 的断言数从 181 降到 178：删掉了 `stats().suppressed`、`resultTaken`、
  回执文本三个已不存在的概念对应的断言。
