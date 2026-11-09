# 后台任务工具改名与超时体系

## 目标

1. `agentxx_async_start` → **`agentxx_async_run`**（功能不变）。
2. `agentxx_async_cancel` → **`agentxx_async_manager`**：新增 `opt`（`cancel` / `delay`），
   `id` 支持数组，删掉 `group`；`opt=delay` 用 `delayMs` 调整目标任务的延时（0 = 移除延时）。
3. `agentxx_async_run` 的超时拆成两个：`queueTimeoutMs`（排队超时，延时不计入，默认 0 = 不限）
   与 `runTimeoutMs`（运行超时，排队/延时不计入，默认 0 = 不限）。
4. `agentxx_async_await` 只保留一个 `timeoutMs`：等待结果的整体超时（含延时 + 排队 + 运行）。
5. 延时中的任务在列表项开头用 `*` 标记。
6. 说明现有取消机制、为什么轮询、能否优化。

## 范围

- `agent/lib/{include/agentxx/agent/async_work.h,src/agent/async_work.cpp}`（超时、延时调整、
  收尾统一、唤醒器）
- `agent/lib/{include/agentxx/tools/async_tools.h,src/tools/async_tools.cpp}`（改名、参数）
- `agent/lib/src/agent/{base_agent.cpp,prompt.cpp}`（注册与提示词）
- `agent/test/core/{test_async_tools.cpp,test_async_work.cpp,test_event_subagent.cpp}`
- 文档：`docs/zh-cn/design/{events.md,index.md,plugins.md,roadmap.md?,tui.md}`、`AGENTS.md`、
  头文件注释（`async_emit.h` / `subagent.h` / `tui_state.h`）

## 状态

见 work.md。

## 结论

已完成并全量测试通过（Windows debug：`async_work` 138 / `async_tools` 145 / 其余相关 9 个模块
1777 条，全绿）。要点：

1. 改名：`agentxx_async_run` / `AsyncRunTool`、`agentxx_async_manager` / `AsyncManagerTool`，
   不保留旧名别名（与上次 `agentxx_tool_async` → `_start` 的做法一致）。
2. `manager`：`opt` = `cancel` / `delay`，`id` 单个或数组（去重逐条处理），删掉 `group`；
   `delayMs` 在 `delay` 时必填，`0` = 去掉延时。
3. 时限体系：`queueTimeoutMs`（延时结束→起跑，含“改延时重算起点”）、`runTimeoutMs`
   （起跑→结束，两种任务都算数）、`await` 的 `timeoutMs`（只结束这次等待）；
   `AsyncWorkSpec::timeLimit` 改名 `runTimeout` 并真正生效。
4. 列表 `*` 标记延时中的排队项；单条详情补 `queueTimeout=` / `runTimeout=`。
5. 取消：排队中直接终态（**修了不发结果事件的旧缺陷**，`await` 不再挂到超时）；运行中置令牌。
   `kind=tool` 执行方从 50 ms 定时器轮询改成 `AsyncWake`（通知式）；顺手修了排队终态
   不释放会话存储占用的泄漏。

与计划不一致的地方：

- 原计划写“延时 400ms + 排队时限 200ms 不误杀”只在测试注释里，没有真断言；现在补了
  `test_task_timeouts_and_marker` 里的实际用例（延时 600ms + 排队时限 200ms → 到点 `done`）；
- 原计划说 `runTimeoutMs` 只对 `kind=agent`；实现里 `runToolWork` 自己算 deadline，
  所以 `kind=tool` 也生效（只结束等待）。文档与插件头注释按实际行为写；
- `AgentxxPluginAsyncWorkSpec::time_limit_ms` 从“写了没人读”变成真生效（行为变化），
  `plugin_runtime` / `plugin_agent_runtime` 全绿。

仍开放给用户决定：`agentxx_async_await` 的 `start_now` 是否保留（`manager {opt:"delay",
id, delayMs:0}` 已能等效）；插件面是否补 `delay_ms` / `queue_timeout_ms`；
是否给工具加真取消通道（见 plan.md §7.5）。
