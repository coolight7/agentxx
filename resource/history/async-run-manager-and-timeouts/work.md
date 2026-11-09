# 实施记录

## 进度

- [x] 核心：两个超时字段 + `finishEntry` 统一收尾 + `AsyncWake` + `setDelay`
- [x] 工具：改名 `async_run` / `async_manager`、`queueTimeoutMs` / `runTimeoutMs`、`timeoutMs`、`*` 标记
- [x] 提示词与注册
- [x] 测试
- [x] 文档

## 每步改动与验证

### 1. 核心（`agent/lib/include/agentxx/agent/async_work.h` + `src/agent/async_work.cpp`）

- 提交快照 `AsyncWorkSpec`：`timeLimit` → **`runTimeout`**（此前没有任何代码读取它，插件侧
  `time_limit_ms` 传进来等于无效），新增 **`queueTimeout`**；
- 台账 `AsyncWorkEntry`：新增 `delayMs` / `queueTimeoutMs` / `runTimeoutMs` /
  `queueDeadlineSteadyMs` / `wake`（`std::shared_ptr<AsyncWake>`）；
- 新增 `AsyncWake`（`async_work.cpp` 顶部、匿名 namespace 之前）：原子计数 + 共享
  `asio::steady_timer`；`signal()` 可从任意线程调（置标志 + `post` 到容器 io 线程
  `cancel()` 定时器），`wait(ms)` 只在 io 线程调，醒来重新判断条件。完成 / 取消 / 超时三条
  路径都调它，取代 `runToolWork` 里 50 ms 定时器轮询；
- 新增 `finishEntry(...)`：终态收尾的唯一入口 —— 写台账、发 `agentxx.agent.async.done`、
  `postAsyncWorksChanged` 通知界面、回调、**释放会话存储占用**、指标记账、队列补位。
  `runner` 正常收尾、排队中取消、排队超时三条路径共用它（顺带修了两个既有缺陷：
  排队中取消不发结果事件 → `await` 挂到超时；排队终态没释放 `claimSession` 占用）；
- `pump`：扫描队列时先判排队超时（到期移出队列、锁外走 `finishEntry` + `failed` +
  `error` 含 `queue timeout`），再判延时未到（跳过、不占名额、不挡后面），再判额度；
- 新增 `armDeadlineTimer` / `onDeadlineTimerFired`：共享定时器取“最早延时到期”与
  “最早排队超时”两者最小值，到点 `pumpAllQueues()`；成员改名 `deadlineTimer_` /
  `deadlineTimerAtMs_`；
- `runner`：起跑时把 `wake` 存进 entry；`kind=agent` 且 `runTimeout > 0` 时挂定时器，
  到点置 `timedOut` + 置取消令牌，收尾按 `failed` 报 `run timeout`；
- `runToolWork(creator, spec, token, wake)`：完成时 `wake->signal()`；等待循环改成
  `co_await wake->wait(剩余)` + deadline 判定（不再 `steady_timer` 轮询）；
- `cancel`：排队中直接走 `finishEntry`；运行中置 token 后 `wake->signal()`；
- 新增 `setDelay(asyncId, ms)`：改延时并**重算排队超时的计时起点**（延时不算排队时间）；
  `startNow` 抹掉延时同样重算；
- `entryJson` / `listJson`：补 `queueTimeoutMs` / `runTimeoutMs` / `queueRemainingMs`
  （列表里原本已有 `delayMs` / `delayRemainingMs` / `queueAhead`）；
- `agent_host.h` / `agent_host.cpp`：新增 `setAsyncWorkDelay`（与 `startAsyncWorkNow` 同层）。

### 2. 工具（`agent/lib/include/agentxx/tools/async_tools.h` + `src/tools/async_tools.cpp`）

- `AsyncStartTool` → **`AsyncRunTool`**（`agentxx_async_run`）、`AsyncCancelTool` →
  **`AsyncManagerTool`**（`agentxx_async_manager`），不保留旧名别名；
- `AsyncRunTool`：新增 `queueTimeoutMs` / `runTimeoutMs`（兼容 `queue_timeout_ms` /
  `run_timeout_ms`），提交时填进 `AsyncWorkSpec`；受理文本里给延时提示时指向
  `agentxx_async_manager {opt:"delay", ...}` 与 `await {start_now:true}`；
- `AsyncManagerTool`：`opt` = `cancel` / `delay`（其他取值按参数错误拒），`id` 支持字符串或
  数组（去重、逐条处理、逐条一行结果，全部失败时返回 `ToolResult::fail`），`delayMs` 在
  `delay` 时必填、`0` = 去掉延时；`group` 参数删除；归属仍限本会话；
- `AsyncAwaitTool`：参数改 `timeoutMs`（兼容 `timeout_ms`），保留 `start_now`；
- 列表渲染：延时中的排队项 bullet 用 `*`（其余 `-`），并跟 `delay=<剩余> left`；
- 单条详情 `renderEntryDetail`：只在 `startedAtMs > 0` 时打印 `elapsed`；新增
  `queueTimeout=` / `runTimeout=`（设了才打印）；
- 单条 JSON（`asyncWorkJson`）补 `queueRemainingMs`（列表里原本已有）。

### 3. 提示词与注册

- `base_agent.cpp`：注册换成 `AsyncRunTool` / `AsyncQueryTool` / `AsyncAwaitTool` /
  `AsyncManagerTool`；
- `prompt.cpp`：新增 `agentxx_async_run`（含 `delayMs` / 两个时限的边界说明：
  “延时结束才开始算排队时限”“排队与延时不算运行时限”“两个都是任务侧时限，
  `await` 的 `timeoutMs` 只管等多久”）与 `agentxx_async_manager` 的提示词；
  `agentxx_async_await` 改成 `timeoutMs`；`agentxx_subagent` 描述里的工具名同步；
- 文档字符串里的工具名（`async_emit.h` / `subagent.h` / `subagent.cpp` / `tui_state.h` /
  `tui_sidebar_content.cpp` / `plugin_kit.h`）批量改名；顺手修了 `subagent.h` 的
  `renderSubagentStartedText` 里一处漏格式化的 ``{}``（现在写成
  `agentxx_async_await {id:"..."}` / `agentxx_async_manager {opt:"cancel", id:"..."}`）。

### 4. 测试（`agent/test/core/test_async_tools.cpp` 等）

- `AtFixture::buildFixture(fx, maxConcurrent = 3)` 新增额度参数（排队超时用例要占满额度）；
- `test_cancel` → `test_manager_cancel`（`opt:"cancel"`、id 数组、别会话 id 被拒）；
- 新增 `test_manager_delay`（改延时、`delayMs:0` 立即起跑、运行中的报 `not queued`、
  参数错误）；
- 新增 `test_task_timeouts_and_marker`（额度 1 时排队超时 `failed` + `error` 含
  `queue timeout`、运行超时（工具任务）`failed` + `run timeout`、
  **延时不计入排队时限**（延时 600ms + 排队时限 200ms 到点正常跑完）、列表 `*` 标记）；
- `test_event_subagent.cpp`、`test/include/agentxx-test/core/test_async_tools.h` 同步改名。

### 5. 文档

- `docs/zh-cn/design/events.md`：§6 工具表（run 的三个新参数、manager 的 `opt`/`id` 数组、
  await 的 `timeoutMs`）；§8 正文（改名后的例子、延时不算排队时间、`*` 标记、取消与改延时）；
  新增 **§8.1 三个时限各管一段**（表格 + 起点/到点后果/`runTimeoutMs` 只对 `kind=agent`）、
  **§8.2 取消是怎么做的**（排队中 / 运行中两条路、为什么工具停不下来、为什么曾经轮询、
  `AsyncWake` 改法、还剩下的三条限制）；§14 测试模块说明同步；
- `docs/zh-cn/design/index.md`：工具表（run/query/await/manager）与正文的取消/时限段落；
- `docs/zh-cn/design/plugins.md`：`time_limit_ms` 现在真的生效（= 运行时限，只对 `kind=0`）；
  延时与排队时限还没在插件面开放；
- `docs/zh-cn/design/tui.md`：取消入口文案；
- `docs/zh-cn/design/roadmap.md`：补 2026-11-09（延时排队与任意工具）与 2026-11-10
  （本轮：改名 / 管理入口 / 三个时限 / 通知式等待）两条记录；
- `AGENTS.md`：工具族参数、取消/时限/`AsyncWake` 段落；
- 头文件注释：`plugin_api.h` 的 `time_limit_ms`、`plugin_kit.h` 的 `timeLimitMs`。

## 验证清单

构建（Windows debug，按顺序；改头文件后必须装一次再编测试）：

```powershell
cd agent/build/windows-debug
cmake --build agentxx_lib_repo-prefix\src\agentxx_lib_repo-build --config Debug --target agentxx_static
cmake --install agentxx_lib_repo-prefix\src\agentxx_lib_repo-build --config Debug
cmake --build agentxx_test_repo-prefix\src\agentxx_test_repo-build --config Debug --target agentxx_test
Copy-Item agentxx_test_repo-prefix\src\agentxx_test_repo-build\Debug\agentxx_test.exe exec\agentxx_test.exe -Force
```

测试结果（`exec\agentxx_test.exe --log-level warn|error ...`）：

| 模块 | 结果 |
|---|---|
| `async_work` | 138 passed / 0 failed |
| `async_tools` | 145 passed / 0 failed |
| `event_tools` / `event_delivery` / `event_subagent` / `plugin_event` | 97 / 48 / 52 / 124 passed，0 failed |
| `plugin_runtime` / `plugin_agent_runtime` / `plugin_sdk` | 736 / 214 / 141 passed，0 failed |
| `tui_widget` / `chatbot_task` / `boundaries` | 208 / 138 / 19 passed，0 failed |

（`async_work` + `async_tools` 一轮共 283 条；其余一轮共 1777 条，全绿。）

关键断言：

- `queueTimeoutMs`：额度占满时排队超过限时 → 状态 `failed`、`error` 含 `queue timeout`，
  `notify` 与 `agentxx_async_await` 都立刻拿到终态（日志可见
  `[async_work] async-2 queue timeout (limit=200ms)`）；
- `runTimeoutMs`：两种任务都算数 —— agent 任务到点置令牌、引擎在检查点停下；
  工具任务由 `runToolWork` 的等待到点，终态 `failed` + `run timeout`（工具本体继续跑、
  结果丢弃）；
- **延时不计入排队时限**：延时 600ms + 排队时限 200ms 的任务到点正常跑完（`done`），
  而不是在 200ms 时报 `queue timeout`；
- `manager{cancel}`：id 数组一次取消多条、逐条结果、别会话的 id 被拒；
- `manager{delay}`：改延时后不跑；`delayMs:0` 立刻起跑；运行中的报 `not queued`；
- 列表：延时中的项以 `*` 开头，普通排队项 `-` 开头；
- 排队中被取消：结果事件照发（`await` 不再挂到超时）、会话存储占用被释放。

## 追加改动 (同日第二轮, 按用户反馈)

1. **`agentxx_async_manager` 的 `id` 只按数组声明/解析**: schema 本来就是 `array`, 但代码里
   还留了一条“接受单个字符串”的兼容分支（`parseIdList` 的 `else collect(*it)`）。现在删掉
   这条分支 —— 模型给单个 id 时由执行层 `autoFixToolArgsTypes` 按 schema 自动包成数组
   (`tool_args_fix.cpp` 的 `string -> [string]`), 文档与提示词同步写成数组；测试里的 manager
   调用也改成传数组。
2. **`agentxx_async_await` 删掉 `start_now`**: schema、实现、提示词、头文件注释、events.md /
   index.md / AGENTS.md 全部同步；要提前起跑用
   `agentxx_async_manager {opt:"delay", id:["async-7"], delayMs:0}`。
   `AgentHost::startAsyncWorkNow` 保留（内部 API，测试 `test_delay_and_start_now` 仍用它）。
   剩余延时 > `timeoutMs` 时仍然直接回说明文本（不再提 `start_now`）。
   `test_await_start_now` → `test_await_delayed_task`：先用 manager 去掉延时，再 `await` 拿结果。
3. **插件面补 `delay_ms` / `queue_timeout_ms`**: `AgentxxPluginAsyncWorkSpec` 尾部追加两个
   `int64_t`（基线仍停在 `offsetof(tool_name)`，读侧 `specHas`）；`plugin_manager_vtable.cpp`
   映射到 `AsyncWorkSpec::delay` / `queueTimeout`；SDK `AsyncWorkOptions` 增加 `delayMs` /
   `queueTimeoutMs` 并填进 spec。`plugin_agent_runtime` 新增 7c 用例：延时 60s + 排队时限 5s
   的任务在台账里看得到两个字段、状态 `queued`，延时中取消立即终态。
4. **原始字符串统一写成 `R"_(...)_"`**: 用一次性脚本按字面量顺序改写 `agent/lib` /
   `agent/client` / `agent/plugins` / `agent/benchmark`（不含 `third_party`）共 **43 个文件、
   211 处**，无遗留 `R"(`；脚本对每个字面量校验正文里不出现 `)_"`。测试目录（`agent/test`）
   未改（用户只要主程序与插件），如需可同样批量处理。

验证（同上流程 + 重编全部插件）：

- `async_tools` 145、`plugin_agent_runtime` 221（+7）、`plugin_runtime` 736、`plugin_sdk` 141、
  `async_work` 138、`event_subagent` 52、`boundaries` 19，全部 0 failed；
- `agentxx` 客户端与 25 个插件动态库均重新编译通过（`build_plugins.ps1`）。

## 备注

- **延时与排队时限的关系**：实现取“延时结束那一刻”作为排队计时起点
  （`submit` 里就是 `delayUntil + queueTimeout`），所以“延时 5 分钟 + 排队时限 1 分钟”
  不会因为延时而失败；`setDelay` / `startNow` 改延时后也从新的延时结束点重算 ——
  否则会出现“还在延时里就被判排队超时”（`pump` 是先判排队超时、再判延时）。
- **`start_now` 保留**：用户没有要求删；`agentxx_async_manager {opt:"delay", delayMs:0}`
  能达到同样效果，是否删掉留给用户决定。
- **插件面未开放**：`AgentxxPluginAsyncWorkSpec` 仍没有 `delay` / `queueTimeout` 字段
  （`time_limit_ms` 现在真的生效，映射到运行时限），插件提交的任务不会因排队超时失败。
- **`runTimeoutMs` 对两种任务都有用，但能停到哪一步不同**：agent 任务置令牌后由引擎在
  检查点真的停下；工具任务只能结束等待（工具本体不认识任务令牌，它可能继续跑到自己的超时，
  结果被丢弃）。要真把工具停下，用工具自己的超时参数更可靠。
- `time_limit_ms` 从“写了但没人读”变成真正生效属于行为变化：以前给插件任务设了该值也没有
  超时，现在到点会被取消。`plugin_runtime` / `plugin_agent_runtime` 全绿，说明现有用例不受影响。
- 老记录（`resource/history/async-work/`、`async-tool-family/`、`async-delay-and-any-tool/`）
  里的旧工具名按历史保留，不改写。
