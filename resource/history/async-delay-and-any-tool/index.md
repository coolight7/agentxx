# 后台任务: 任意工具可异步 + 延时排队

## 目标

1. `agentxx_async_start` 只拦自己 (`agentxx_async_start`)，其余工具 —— 其他后台任务管理工具
   (`query`/`await`/`cancel`/`emit`)、lib 内置工具 (`agentxx_share_store` 等)、插件/动态注册
   工具 —— 都允许放进后台跑。
2. `agentxx_async_start` 新增 `delayMs`：延时未到**一直算排队中**，不进入运行。
3. `agentxx_async_await` 新增 `start_now`：目标还在延时中时，抹掉剩余延时立即排队起跑。
4. 顺带整理"后台任务管理工具"清单，供用户判断要不要增补能力。

## 范围

- `agent/lib/include/agentxx/agent/async_work.h`、`agent/lib/src/agent/async_work.cpp`
  （延时字段、可跳过延时的 pump、唤醒定时器、`startAsyncWorkNow`）
- `agent/lib/include/agentxx/agent/context.h`、`agent/lib/src/agent/base_agent.cpp`
  （内置工具索引 + 图引擎弱引用，供后台按名执行内置工具）
- `agent/lib/src/tools/async_tools.{h,cpp}`（拦截规则、`delayMs`、`start_now`、文案）
- `agent/lib/src/agent/prompt.cpp`（工具提示词）
- `agent/lib/src/agent/io/session_server_agent_io.cpp`（只有延时任务时不再按秒续推）
- 测试 `agent/test/core/test_async_tools.cpp`、`agent/test/core/test_async_work.cpp`
- 文档 `docs/zh-cn/design/events.md` §8、`AGENTS.md`

## 状态

已实现并验证（见 work.md）。

## 关键决策

- 延时任务 `state` 仍是 `queued`（用户要求"归类为排队中"），只是台账多出 `delayRemainingMs`；
  界面「后台任务」段无需改协议。
- pump **跳过还没到期的条目**，不阻塞队头：一个 5 分钟延时的任务不该把同队列的活活埋住。
  到期由管理器的一条共享唤醒定时器触发重推。
- 内置工具在 `AgentContext` 上留"名字 → neograph::Tool*"索引 + 图引擎弱引用：工具对象归图引擎
  所有，后台任务先锁住引擎再取指针，引擎没了就按失败收尾（不悬垂）。
- 只拦 `agentxx_async_start`：套自己会无限递归，其他工具套一层没有危害。

## 结论

- 拦截规则只剩 `agentxx_async_start` 自己；`kind=tool` 的工具来源扩为
  “内置工具索引（`AgentContext::builtinToolIndex` + 图引擎弱引用）+ 动态注册表”。
- `delayMs` 的整个延时期间 `state` 保持 `queued`，pump 跳过延时项（不占名额、不挡后面的），
  到期由管理器共享定时器唤醒重推。
- `agentxx_async_await {start_now:true}` 抹掉剩余延时；不给且剩余延时大于 `timeout_ms` 时
  直接回说明文本。
- 界面：只有“运行中 / 无延时排队”的条目触发每秒续推。
- 验证与遗留事项见 work.md（含“可选限制”清单，待用户决定）。
