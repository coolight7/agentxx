# 设计方案: 任意工具可异步 + 延时排队

## 1. 背景

后台任务 (`async work`) 把一次工具调用或一个子代理放到后台跑，结果经事件中枢回投会话。
现状有两条限制：

1. `agentxx_async_start` 拒绝把**后台任务管理工具**放进后台
   （`agentxx_async_query` / `_await` / `_cancel` 也被一并拦住）；
2. `kind=tool` 只在**创建者的工具注册表** (`AgentContext::toolRegistry`) 里查工具，
   lib 内置工具（`agentxx_share_store` / `agentxx_async_emit` / 中间件贡献的
   `agentxx_subagent` 等）由**图引擎**独立持有，因此"内置工具不能后台跑"。

用户要求：只拦 `agentxx_async_start` 自己，其他工具全部允许；并新增延时与"提前开始"。

## 2. 现状调查（文件与函数一级）

| 关注点 | 位置 |
|---|---|
| 拦截名单 | `agent/lib/src/tools/async_tools.cpp` `AsyncStartTool::execute_async`（4 个名字硬编码） |
| 工具查找 | `agent/lib/src/agent/async_work.cpp` `runToolWork`（只查 `creator->toolRegistry->find`） |
| 内置工具 | `agent/lib/src/agent/base_agent.cpp` `initTools` / `initMiddlewareTools` / `bind_tools` 步骤的 `engine->own_tools(...)`（工具归引擎，`engine` 是 `BaseAgent` 的 `shared_ptr`） |
| 队列推进 | `AsyncWorkManager::pump`（一次只起一条，额度满就 break）、`runner`（收尾后再 pump） |
| 台账 JSON | `AsyncWorkManager::entryJson` / `listJson`（`queueAhead` 只数位置） |
| 界面续推 | `SessionServerAgentIO::hasActiveAsyncWorks`（`includeFinished=false` 有就按秒续推） |
| 提交快照 | `AsyncWorkSpec`（`agentxx.agent.runtime` 的 `AgentxxPluginAsyncWorkSpec` 由它派生，`delay` 默认 0 即旧行为） |

## 3. 方案

### 3.1 延时（核心）

- `AsyncWorkSpec` 追加 `std::chrono::milliseconds delay{0}`（<=0 视为不延时）。
- `AsyncWorkEntry` 追加 `delayMs`（提交时的原值，台账展示用）与
  `delayUntilSteadyMs`（steady 时钟，0 = 不延时）。
- `submit`：`delay` 夹到 >= 0，算出 `delayUntilSteadyMs`；状态仍 `Queued`，
  入队位置照旧（FIFO 顺序不变）。
- `pump` 改成"扫一遍队列，把能起的都起"：
  - 台账已回收 / 非 `Queued` → 从队列剔除；
  - `delayUntilSteadyMs > now` → **留在队列里跳过**，记下最早到期时刻；
  - 额度 (`maxConcurrent`) 用满 → 停止扫描（后面排队的等收尾）；
  - 其余 → 置 `Running`、起协程（沿用原来的 `co_spawn` + 强引用容器）。
- 到期唤醒：管理器持一条 `asio::steady_timer`（容器 io 线程独占），
  `armDelayTimer()` 取所有 `Queued` 条目里最早的到期时刻；
  到点后 `pumpAllQueues()`。定时器句柄只捕获 `weak_ptr<AgentHost>`，
  避免 `host → manager → timer → handler → host` 的环把容器吊住。
  `~AsyncWorkManager` 不碰定时器（io 线程可能已经停了）。
- 对外新增 `AsyncWorkManager::startNow(asyncId)` / `AgentHost::startAsyncWorkNow(asyncId)`：
  清掉剩余延时并对该队列 pump 一次；不在排队中（已跑/已终态/不存在）返回 `false`。
- 台账：`entryJson` / `listJson` 的 `queued` 项追加 `delayRemainingMs`（>0 表示还在延时），
  `entryJson` 另给 `delayMs`（原始值）；提交方回调 `Queued` 的 payload 也带 `delayMs`。
- 界面：`hasActiveAsyncWorks` 只把 `running` 和"没有剩余延时"的 `queued` 当作需要按秒续推，
  否则一个 1 小时延时的任务会让端点每秒推 3600 次。延时到期由 pump 的
  `postAsyncWorksChanged` 补一次推送。

### 3.2 任意工具可异步

- `AgentContext` 追加两件（都是"看图引擎"用的借用，不持有工具）：
  ```cpp
  std::map<std::string, neograph::Tool*, std::less<>> builtinToolIndex; // 名字 → 引擎持有的工具
  std::weak_ptr<neograph::graph::GraphEngine>           engine;         // 索引的有效期凭据
  ```
  在 `BaseAgent::init` 的 `bind_tools` 步骤（`engine->own_tools` 之前）填充索引，
  并写入 `agentContext->engine`；`engine_build` 步骤回滚时一并清空。
- `AsyncStartTool`：唯一拦截 `agentxx_async_start`；`agentxx_subagent` 仍走 `kind=agent`；
  其余工具要求"内置索引里有 **或** 工具注册表里有"，都没有则报 "tool not found"。
- `runToolWork`：先 `creator->engine.lock()`（拿住引擎 = 拿住工具寿命），
  再按 `builtinToolIndex` → `toolRegistry` 顺序找；`XXToolBase` 走 Json 主接口，
  原生 `neograph::Tool` 走图桥接（与 `ToolcallWrapNode::runToolCallBody` 同一套分发）。
  引擎/索引都取不到时按失败收尾，文案说明创建者已销毁。

### 3.3 `agentxx_async_await` 的 `start_now`

- 参数 `start_now`（boolean，默认 `false`）。
- 目标还在延时中时：
  - `start_now=true` → `startAsyncWorkNow`，然后照常挂等待；
  - `start_now=false` 且剩余延时**大于** `timeout_ms` → 不等，直接返回说明文本
    （任务在延时、还剩多久、怎么提前开始），避免必然超时；
  - 剩余延时小于 `timeout_ms` → 照常等（定时器到期后任务会起跑）。
- 文案渲染 `renderEntryDetail` / `renderTaskList` 的 `queued` 分支补延时信息。

## 4. 边界与兼容

- `delayMs` 负数/非数字按 0 处理（不报错，不拒绝）；超大值不设上限（用户未要求，
  见 work.md「可选限制」）。
- 插件侧 `AgentxxPluginAsyncWorkSpec` 本次**不加** `delay_ms` 字段（会动接口表版本与
  `plugin_runtime` 的布局断言）；`AsyncWorkSpec::delay` 默认 0，插件行为不变。
  需要时按"表尾追加 + `specHas` 判定"补。
- `kind=tool` 参数自愈、`sessionId` / `tool_call_id` 注入、提交前权限询问全部保持不变，
  只是工具查找多了一条内置来源。
- 取消/终态/linger/落盘路径不变：延时任务在 `Queued` 状态下取消直接出队。

## 5. 分步实施

1. 核心：`delay` 字段 + pump 扫描/跳延时 + 唤醒定时器 + `startNow` + 台账字段。
2. 内置工具索引：`context.h` + `base_agent.cpp`。
3. 工具面：拦截规则、`delayMs`、`start_now`、文案；`async_work.cpp` 的查找顺序。
4. 提示词：`prompt.cpp` 三个工具的 depict/args。
5. 界面续推过滤。
6. 测试与文档。

## 6. 验证

- `agentxx_test async_work async_tools event_tools event_subagent chatbot_task plugin_agent_runtime`
- 新用例：
  - 延时任务在延时内保持 `queued`（不进入 `running`），到期后跑完；
  - 延时任务不阻塞同队列的非延时任务（后者先跑）；
  - `startAsyncWorkNow` 清掉剩余延时（manager 层 + `await{start_now:true}`）；
  - `await` 在剩余延时 > 超时时直接给说明文本（不挂到超时）；
  - 内置工具（`agentxx_share_store`）后台跑成功，结果回台账；
  - 其他后台任务管理工具（`agentxx_async_query`）可后台跑；
  - 只有 `agentxx_async_start` 被拒。

## 7. 风险

- pump 一次起多条：事件回调 (`onEvent`) 与界面推送次数变多（每条一次，与旧行为同粒度）。
- 唤醒定时器跨线程：只在容器 io 线程创建/重置，句柄只持弱引用。
- 内置工具索引是裸指针：有效期由 `context->engine` 的弱引用保证（先锁引擎再取指针）。
