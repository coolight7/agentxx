# 设计方案: 后台任务工具改名、延时调整与超时体系

## 1. 背景

上一轮已实现「任意工具可异步 + `delayMs` 延时排队 + `await {start_now}`」。这一轮把工具族
收敛成更清楚的一套：

- `agentxx_async_start` 名字与「开始一条后台任务」不够贴，改名 `agentxx_async_run`；
- `agentxx_async_cancel` 只做取消，但延时任务需要「调延时 / 去掉延时」，这类「对已存在任务
  做调整」的操作应该收进一个管理入口：`agentxx_async_manager`（`cancel` / `delay`），并且
  一次能处理多个 id（`id` 变数组），因此原来按 `group` 批量取消的入口没必要保留；
- 超时过去只有 `await` 的 `timeout_ms`，任务本身没有任何时限：排队排多久、跑多久都不受控。
  现在拆成两个**任务侧**时限（排队 / 运行）和一个**等待侧**时限（await 整体等待）。

## 2. 现状调查（文件与函数一级）

| 关注点 | 位置 |
|---|---|
| 工具注册 | `BaseAgent::initTools`（`initTools` 里 4 个 `std::make_unique`） |
| 工具实现 | `agent/lib/src/tools/async_tools.cpp`：`AsyncStartTool` / `AsyncQueryTool` / `AsyncAwaitTool` / `AsyncCancelTool` |
| 工具提示词 | `agent/lib/src/agent/prompt.cpp` 的 `AgentPrompt::toolPrompt`（键 = 工具名） |
| 台账与队列 | `AsyncWorkManager::submit` / `pump` / `runner` / `cancel` / `startNow` / `entryJson` / `listJson` |
| 提交快照 | `AsyncWorkSpec`（含**从未被读取**的 `timeLimit`） |
| 取消令牌 | `neograph::graph::CancelToken`（第三方，原子置位）；`kind=agent` 经 `RunConfig::cancel_token` 传给引擎；`kind=tool` 由 `runToolWork` 50 ms 轮询 |
| 工具侧取消 | `getSessionCancelToken(ctx, args)`（按 `sessionId` 取**会话轮次**令牌，与后台任务令牌无关） |
| 界面续推 | `SessionServerAgentIO::hasActiveAsyncWorks`（上一轮已排除纯延时项） |

## 3. 工具改名与参数

### 3.1 改名对照

| 旧 | 新 |
|---|---|
| `agentxx_async_start` / `AsyncStartTool` | `agentxx_async_run` / `AsyncRunTool` |
| `agentxx_async_cancel` / `AsyncCancelTool` | `agentxx_async_manager` / `AsyncManagerTool` |
| `agentxx_async_query` / `AsyncQueryTool` | 不变 |
| `agentxx_async_await` / `AsyncAwaitTool` | 不变（参数改 `timeoutMs`） |

不做旧名别名：与 `agentxx_tool_async` → `agentxx_async_start` 那次一样，直接换名，
提示词里给出的永远是当前名字。

### 3.2 `agentxx_async_run`

参数：`tool`、`args`、`notify`、`label`、`delayMs`、**`queueTimeoutMs`**、**`runTimeoutMs`**。
校验与提交逻辑与现在的 `agentxx_async_start` 完全一致（只拦自己、`agentxx_subagent` 走
`kind=agent`、其余工具查内置索引/注册表、提交前问一次权限）。

### 3.3 `agentxx_async_manager`

| 参数 | 说明 |
|---|---|
| `opt` | 必填：`cancel` / `delay`；其他取值按参数错误拒 |
| `id` | 必填：字符串或字符串数组（数组按顺序逐个处理，重复项只处理一次） |
| `delayMs` | `opt=delay` 必填，`>= 0`；**0 = 移除延时**（任务立即进入排队起跑） |

- 归属不变：只能操作**本会话启动的**任务（别会话的 id 逐条报「not started by this session」）。
- 逐条结果文本（一行一条，末尾一行汇总），全部失败时返回 `ToolResult::fail`：
  - `cancel`：`queued` → 出队并终态 `canceled`（**发结果事件**，见 §6）；`running` → 置取消令牌 +
    唤醒；终态 → `already finished (state=done)` 跳过。
  - `delay`：`queued` → 改延时（重算排队超时起点）；`running` / 终态 → `not queued (state=running)`
    跳过；不存在 → `not found`。
- `group` 参数删除（要批量就传 id 数组）。

### 3.4 `agentxx_async_await`

参数：`id`、**`timeoutMs`**（别名 `timeout_ms`，默认 30000、上限 300000）、`start_now`。
`timeoutMs` 是**等待结果**的整体超时：延时、排队、运行全算在里面（这正是"等多久还拿不到结果
就放弃"的语义）。剩下的行为保持：已终态直接回台账；`start_now` 抹掉剩余延时；剩余延时比
`timeoutMs` 还长时直接给说明（不挂到必然超时）。

## 4. 超时体系

### 4.1 两个任务侧时限

| 参数 | 计时起点 | 到点后果 |
|---|---|---|
| `queueTimeoutMs` | **延时结束那一刻**（无延时 = 提交时刻） | `pump` 扫描时把该条移出队列、按 `failed` 收尾，`error` 写明「queue timeout」并发出结果事件 |
| `runTimeoutMs` | **起跑那一刻** | `kind=tool`：等待循环到点立即按 `failed` 收尾（执行体继续跑、结果丢弃）；`kind=agent`：到点置 `timedOut` + 置取消令牌，引擎在下个检查点停下，同样按 `failed` 收尾 |

- 默认都是 0 = 不限制。
- `AsyncWorkSpec::timeLimit` 改名为 `runTimeout`：它此前一直没被任何代码读取（插件侧
  `time_limit_ms` 传进来也无效），这次接上；`queueTimeout` 是新字段，插件接口表暂不暴露。
- 排队超时不占额外定时器：复用管理器的共享定时器（取「最早的延时到期」与「最早的排队超时」
  两者最小值），到点 `pumpAllQueues()`。
- 运行超时：`runner` 在起跑时按 `runTimeout` 挂一个定时器（kind=agent 用协作式取消；
  kind=tool 由等待循环自己算 deadline）。

### 4.2 等待侧（await）

只有一个 `timeoutMs`，覆盖整段等待；不再有"任务 timeout"概念（那是任务侧两个参数的事）。

## 5. 列表标记

`renderTaskList` 里排队项的 bullet：延时中（`delayRemainingMs > 0`）用 `*`，其余用 `-`：

```text
- async-3 [running] tool agentxx_execute_bash_command elapsed=00:12
* async-4 [queued]  tool agentxx_filesystem_grep ahead=1 delay=1.5m left
```

单条详情（`renderEntryDetail`）仍用 `state=queued` + `ahead=` + `delay=… left` 表达。

## 6. 顺手修两个既有缺陷（与本次强相关）

1. **排队中取消不发结果事件**：现状 `cancel()` 只改台账 + 通知界面，不发
   `agentxx.agent.async.done` —— 于是 `agentxx_async_await` 等一条排队中被取消的任务会一直挂到
   超时，`notify` 也收不到"被取消了"。本次把收尾统一成 `finishEntry()`，排队取消 / 排队超时 /
   运行终态都走它，都发结果事件。
2. **排队终态没释放会话存储占用**：`submit` 时就 `claimSession`，只有 `runner` 用 RAII 释放；
   排队中被取消或排队超时的任务永远不会跑 `runner` → 占用泄漏（同一存储根再提交就报
   "session store ... is busy"）。`finishEntry()` 一并释放。

## 7. 取消机制：现状、为什么轮询、能否优化

### 7.1 现状（按状态分两条路）

`AsyncWorkManager::cancel(asyncId)`：

1. **排队中**：从队列 vector 移除 → 台账置 `Canceled` → 发终态回调 → 通知界面 → 再 `pump`
   让后面的任务补位。任务从未起跑，**立即**结束。
2. **运行中**：`entry.token->cancel()`（`neograph::graph::CancelToken`，本质是原子 bool），
   然后由任务自己发现并停下。分两种执行体：
   - **`kind=agent`（子代理）**：令牌经 `AgentHost::spawnOneTask` 塞进
     `neograph::graph::RunConfig::cancel_token`，图引擎/`AgentRunner` 在节点边界、模型调用、
     工具调用前检查它 → 抛 `CancelledException` → 这是**真取消**（协作式，粒度 = 检查点）。
   - **`kind=tool`（异步工具）**：`runToolWork` 里 `co_spawn` 一具 detached 协程跑工具本体，
     自己则进一个循环：每 50 ms 醒来一次，看 `token->is_cancelled()` 与 `state->done`。
     看到取消 → 本函数返回 `Canceled`，**工具本体继续跑**（它的结果被丢弃）。
     工具自己不会知道被取消了 —— 它只会在自己的超时/取消检查点停。

### 7.2 为什么工具停不下来

工具执行接口是 `XXToolBase::execute_async(const utilxx_base::Json& args)`，**没有取消参数**。
工具内唯一能拿到的取消信号是 `getSessionCancelToken(agentContext, args)`：按
`args["sessionId"]` 取该会话**当前轮次**的令牌。后台任务的令牌是它自己的（`entry.token`），
跟会话令牌没有任何关系；而且后台任务可能跨多个轮次、创建者轮次早就结束了（令牌已置位），
拿会话令牌判断会得到完全错误的结论。所以只能"发起方停止等待"，不能"让工具停止"。

### 7.3 为什么用 50 ms 轮询

等待的是两个信号：**工具协程完成**（只有一个 `std::atomic<bool> done` 标志）与
**取消置位**（`CancelToken` 里的原子 bool）。asio 没有"等待一个普通条件变量"的原语
（`asio::experimental::channel` / `awaitable_operators` 需要整套改造），最省事又不会阻塞
io 线程的写法就是 `async_wait` 一个 `steady_timer`，醒来查一次标志。50 ms 是"取消延迟"
与"每秒 20 次唤醒"之间的折中。

代价：
- 每个运行中的工具任务每秒 20 次定时器唤醒（空转成本不高，但完全是浪费）；
- 取消生效最多延迟 50 ms（体感上无所谓，但统计/TUI 上的"已取消"会慢一拍）；
- 定时器粒度与 50 ms 常量是隐藏耦合，`runTimeoutMs` 想精确到毫秒就没办法。

### 7.4 本次的优化（已实现）

引入 `AsyncWake`（内部小对象，持一个 `asio::steady_timer`）：
- 完成、取消、到期都调 `signal()`（任意线程：置标志 + `asio::post` 到容器 io 线程 `cancel()`
  定时器）；
- 运行方只 `co_await wake->wait(剩余超时)` 醒一次，醒来按 `done / token / deadline` 判定。

效果：
- 去掉每秒 20 次空转；取消**立刻**唤醒（不再等最多 50 ms）；
- `runTimeoutMs` 顺带精确到毫秒（kind=tool 的 deadline 就是同一个 wait 的到期时间）；
- 逻辑从"轮询猜状态"变成"事件驱动 + 醒后复核"，与项目里其它异步代码一致
  （`EventBus` / `AsyncMutex` 都是通知式）。

### 7.5 还能怎么优化（本次不做，等确认）

1. **给工具真正的取消通道**：新增
   `virtual asio::awaitable<ToolResult> execute_async(const Json&, const std::shared_ptr<graph::CancelToken>&)`
   重载（默认转发到单参版本），后台执行时把**任务令牌**传下去。`agentxx_execute_command` 这类
   已经在轮询令牌的工具就能真停（子进程被杀）。影响面：`XXToolBase` 头文件、`runToolWork`、
   插件 SDK（新能力位/新表版本才能把令牌交给 .so 内部）。
2. **让 `getSessionCancelToken` 认识后台令牌**：把任务令牌作为"本次调用的附加令牌"挂在
   调用上下文里，`getSessionCancelToken` 返回一个组合令牌（会话令牌 or 任务令牌任一置位）。
   好处是零改动的工具也能真停；代价是需要在 `AgentContext`/会话上加"当前调用令牌"的栈，
   多线程/并发工具调用下要小心串味（`supportsParallel` 的工具会并发执行）。
3. **取消的确认语义**：现在接口是"尽力而为"。可以改成两段：`cancel` 立即把台账置
   `Canceling`，执行体停下后置 `Canceled`；`manager` 可以只报"已请求取消"。这样界面与
   `query` 能看出"取消中"。代价是台账多一个状态、界面文案要跟着改。
4. **agent 任务的取消粒度**：引擎只在检查点停（模型调用返回、节点边界），一次长工具调用
   期间取消不了。可以给引擎的 `RunConfig` 加"工具调用级取消"（引擎侧支持才行）。

## 8. 分步实施

1. 核心：`AsyncWorkSpec` 两个超时字段 + 台账字段；`finishEntry()` 统一收尾（含结果事件与会话
   占用释放）；pump 排队超时；`runner` 运行超时；`AsyncWake` 替换 50 ms 轮询；`setDelay()`
   （+ `AgentHost::setAsyncWorkDelay`）。
2. 工具：改名两个类与名字；`AsyncRunTool` 加两个超时参数；`AsyncManagerTool`（`opt`/`id` 数组/
   `delayMs`）；`AsyncAwaitTool` 的 `timeoutMs`；列表 `*` 标记。
3. 提示词与注册（`prompt.cpp` / `base_agent.cpp`）。
4. 测试：改名同步 + 新增 manager（cancel 数组 / delay 调整 / 归属 / 参数错误）、排队超时、
   运行超时、await 整体超时、`*` 标记、排队取消发事件。
5. 文档：events.md（工具表 + 超时 + 取消机制）、index.md 工具表、plugins.md（`time_limit_ms`
   现在真的生效）、AGENTS.md、各处头文件注释。

## 9. 验证

- `agentxx_test async_work async_tools event_subagent event_delivery plugin_event plugin_runtime boundaries`
- 关键断言：
  - `queueTimeoutMs`：额度被占满时，排队超过限时 → `failed` + `error` 含 `queue timeout`，
    且 `await` 能立刻拿到结果（不再挂到自己的超时）；
  - `runTimeoutMs`（tool / agent 各一条）：到点 `failed` + `error` 含 `run timeout`；
  - `manager{cancel}` 数组：多条一起取消，逐条结果，别会话任务被拒；
  - `manager{delay}`：延时改大后不跑；`delayMs:0` 立刻跑；运行中的任务报 `not queued`；
  - `await` 只给 `timeoutMs`：延时 30s 的任务用 `timeoutMs=100` → 给说明/超时文本；
  - 查询列表：延时项 bullet 为 `*`，普通项为 `-`。

## 10. 风险

- `finishEntry` 是收尾唯一入口，改动面覆盖 cancel / pump / runner 三条路径：排队终态现在也会
  发结果事件，`event_subagent` / `event_delivery` 里"取消后仍发终态"的既有断言需要复核。
- `AsyncWake` 的定时器对象要活到 `signal` 之后：用 `shared_ptr` 持有，`signal` 里的 `post`
  闭包捕获同一份 `shared_ptr`。
- `AsyncWorkSpec::timeLimit` → `runTimeout` 改名会动插件桥接（`opt.timeLimitMs` 字段名保留）。
