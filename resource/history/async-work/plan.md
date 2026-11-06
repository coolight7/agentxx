# 方案: 内置异步任务模块 (AsyncWork) —— 工具触发的后台任务、事件回结果、平等 agent

- 难度: A- (动到 AgentHost 的派发、AgentConfig、内置工具、wire JSON 字段、插件接口表追加成员)
- 类型: 架构扩展 + 命名重构 (本文仅为设计方案, **未实施**)
- 时间: 2026-11-04 (第 3 版: 按评审意见调整: `async_work` 命名与开关、去掉排队/深度/全局上限、
  单一事件名 `agentxx.agent.async.done` + id 传参、去掉工具 `async` 参数、chatbot 默认 allow、
  TUI 只进 INFO 段、会话目录 `{创建者会话目录}/agent/{async_id}`、async_id 即 session_id)
- 状态: 设计完成, 待确认 (§12), 未实施
- 代码依据: 主仓当前工作树
- 相关文档:
    - [events.md](../../../docs/zh-cn/design/events.md): 事件中枢、等待投递、异步派发现状
    - [index.md](../../../docs/zh-cn/design/index.md) "事件系统" / "多会话与并发": 总线与并发模型
    - [plugins.md](../../../docs/zh-cn/design/plugins.md) §19/§20: 插件运行时表与事件表
    - [tui.md](../../../docs/zh-cn/design/tui.md) §2.7: Info 面板段落 (本次要加一段)
    - [configuration.md](../../../docs/zh-cn/design/configuration.md) §5: 配置归属
    - [chatbot/plan.md](../chatbot/plan.md): chatbot 插件 (本次是它的下游消费者)
    - [event-async/plan.md](../event-async/plan.md): 事件中枢与异步派发的原始设计

阅读路径: 只想知道"做什么/怎么落地"看 §0 / §3.2 / §3.4 / §8; 关注与现有机制的关系看 §2 / §3.5 / §7;
评审关注点集中在 §3.3 (显式清单) · §3.4 (只留队列额度) · §3.5 (创建者先死) · §3.7 (HIL) ·
§3.9/§3.13 (模型与界面可见) · §11 (chatbot 接入) · §12 (待确认)。

目录: [§0 结论摘要](#0-结论摘要) · [§1 目标与非目标](#1-目标与非目标) · [§2 现状核对](#2-现状核对) ·
[§3 设计](#3-设计) · [§4 接口设计](#4-接口设计) · [§5 关键流程](#5-关键流程) · [§6 边界与失败模式](#6-边界与失败模式) ·
[§7 影响面与兼容性](#7-影响面与兼容性) · [§8 实施计划](#8-实施计划) · [§9 测试计划](#9-测试计划) ·
[§10 风险与备选方案](#10-风险与备选方案) · [§11 chatbot 接入](#11-chatbot-接入) · [§12 待确认问题](#12-待确认问题) ·
[§13 文档更新清单](#13-文档更新清单)

## 0. 结论摘要

需求可以拆成三件事, 每件事缺的东西不一样:

1. **"后台跑任务"缺的是一条统一通道**: 现在只有"子代理可异步"这一种形态 (`agentxx_subagent {async:true}`),
   而且是"受理即 `co_spawn`, 撞并发上限直接失败" —— 上限还是**全局**的 (`maxConcurrentSubagents`, 默认 8),
   一个会话派满就把所有会话都堵住; 工具类的后台任务 (异步 tool) 完全没有。
2. **"创建者结束、销毁后任务继续跑"现在做不到**: `spawnOneTask` 每一步都从创建者现取配置与会话
   (`parentConfig` 派生、`parentCtx->eventHub` 收结果、`parentSession->io/bus` 承接 HIL), 创建者一销毁,
   任务既跑不起来也交不出结果。
3. **看不到也停不了**: `agentxx_event` 只有事件维度, 没有"后台任务有几个、卡在哪、我怎么停掉它";
   界面上也没有展示。

所以方案是三件拼起来, 每件都尽量复用现有机制:

| 拼图 | 做法 | 复用 |
|---|---|---|
| 统一的异步任务 | 新增内置 **`AsyncWork`** 模块 (进程一份, 挂在 `AgentHost`): 一个任务 = 一次后台执行 (`kind` = `tool` / `agent`), 提交即可, 结果**统一走事件**; 每队列 (默认按创建者) 一条 FIFO + 额度 3 | 现有 `spawnOneTask` 的派生/回收、`CancelToken`、`AgentRegistry`、工具注册表 |
| 平等 agent | 提交时把派生配置 (显式清单)、会话、结果去处一次带走 (自包含); 运行期不回读创建者; `async_id` 同时就是该任务的 `sessionId` | `makeSubagentConfig` 的派生规则、`claimSession` 的会话独占 |
| 可见与可控 | 扩展内置工具 **`agentxx_event`** (`opt=works` 查、`opt=status/await` 按 `id` 看、`opt=cancel` 停) + TUI **Info 段** + 插件表 **`agentxx.agent.runtime`** 追加成员 | 事件工具的 `opt` 风格、`WireContextStats` 追加 JSON 字段、归属回收的既有做法 |

三条"减法/取舍":

- **不做常驻通道池**。曾经考虑"K 条通道当工人、队列在插件侧", 它要额外解决"会话上下文不能重置"、
  "关闭通道后 `Session` 仍留在根内存里"两个问题 (见 §10.1); 每任务一个自销毁 agent 天然没有这两件事。
- **不做任务持久化**。队列在进程内, 重启丢失; 只把"会话落盘"留给异步任务自己 (§3.8), 默认关。
- **不保留旧命名与旧兼容层**。`job` / `subagent.async` 措辞、旧函数名、旧配置键一次替换到位
  (libagentxx 是自有库, 没有外部 ABI 约束); 唯一保留的是模型工具名 `agentxx_subagent`。

一条贯穿全篇的判断: **创建者与异步任务的 agent 是"平等 agent", 不是父子生命周期**。创建关系只用于
(a) 归属记账与批量回收, (b) 结果往哪送, (c) HIL 往哪冒泡 (可关)。任务能不能跑、结果落到哪,
都不以创建者存活为前提 (§3.5)。

## 1. 目标与非目标

目标:

1. **一个概念**: 后台执行单元统一叫 **async work (异步任务)**, `kind` 区分 `tool` / `agent`;
   模型侧一律"调工具即提交", 结果**由事件通知**, 不需要 `async` / `event_name` 这类工具参数。
2. **排队**: 每个队列 (默认 = 创建者) 并发额度 3, 超出的任务排队等位; **不限排队条数、不限嵌套深度、
   不设全局并发上限**, 也**删除**现有的全局并发上限。
3. **平等**: 提交时快照全部运行参数; 创建者轮次结束、会话关闭、甚至 agent 销毁后, 已受理的任务照常运行到结束。
4. **可控**: 模型和插件都能取消 (单条 / 自己名下全部); agent 关停与进程退出按归属整批取消并发终态。
5. **可见 (模型)**: 能查"后台任务有几个、都在干什么、某条现在什么状态", 能阻塞等待某条结果。
6. **可见 (界面)**: TUI 的 **Info 侧边栏**有一段"后台任务", 显示运行/排队/最近完成与耗时。
7. **可见 (插件)**: 插件 (chatbot 等) 能提交/取消/查询, 不必自己造"通道工人", 也不依赖模型代调工具。
8. **配置直白**: 派生配置**直接列清单** (插件 / MCP / Skill / Memory / RAG / 工作目录), 默认全空;
   开关只有一个 `async_work.enable` (per agent node), 没有 profile 这类二次抽象。
9. **会话可落盘 (可选)**: 异步任务的会话可以像主会话一样保存到
   `{创建者会话目录}/agent/{async_id}`, `sessionId` 就是 `async_id`, 默认关。

非目标 (明确不做):

- **不加任何并发闸门**: 没有 `max_concurrent_global`, 不再保留 `maxConcurrentSubagents`;
  不设排队上限、不设嵌套深度上限、不设软阈值告警 (§10.3 记录后果)。
- **不做跨进程/跨重启的任务队列**: 进程退出即丢。
- **不做"常驻 worker 池"**: 见 §10.1。
- **不共享 provider/HTTP 连接池**: 每个异步任务的 agent 与现状子代理一样自带 provider 实例 (§10.2)。
- **不改 wire 消息类型**: 只在 `WireContextStats` 的 JSON 里**追加字段** (`asyncWorks`), 老客户端忽略。
- **不重命名模型工具 `agentxx_subagent`**: 它是"启动一个 agent 任务"的入口名, 只是不再需要 `async` 参数。
- **不做"异步 tool"的具体实现**: 模块与结构支持 `kind=tool`, 本期先落 `kind=agent` (§10.4)。

## 2. 现状核对

> 本节把方案要碰到的现有机制逐条核对, 后面所有设计都以这里的结论为准。

### 2.1 一次异步派发现在是怎么跑的

```
模型调用 agentxx_subagent {async:true, event_name:"..."}       (tools/subagent.cpp)
   └─ requestInterrupt("subagent") → AgentRunner 处理中断
        └─ 总线 service.subagent (RR) → AgentHost::spawnBatch(req)
             ├─ 同步任务: 立即 co_spawn spawnOneTask 并等全部完成
             └─ 异步任务: 立即返回 {started:true,eventName} + startAsyncSubagentTask(...)
                            └─ co_spawn runAsyncSubagentTask
                                 ├─ spawnOneTask → 建独立 BaseAgent/AgentContext → 跑一轮 → destroyAgentAsync
                                 └─ 结果: parentCtx->eventHub->postValue/postEnd(eventName, {...})
```

要点:

- **受理点与预算点不在一处**: `spawnBatch` 只校验事件名/中枢就受理; 并发检查在 `spawnOneTask` 里,
  撞上限时任务**已经"受理成功"**, 失败后来以结果事件形式回来 —— 这就是"没有排队"的根因。
- **异步任务只支持 toolcall 路径**: `result_id`(即 `tool_call_id`) 为空且 `async=true` 直接抛错;
  所以插件想"我自己派一个"不能靠 `call_tool_async` 绕 (§2.6)。
- **令牌独立于轮次**: `startAsyncSubagentTask` 自建 `CancelToken`, 由 `cancelAsyncJobs(parentCtx)` 驱动;
  父轮次取消不会带走它。
- **`cancelAsyncJobsOfSession` 目前没有调用方** (全仓只有定义)。
- **每次任务的 session id 是另生成的** (`subagent_<名>_<agentId>_<n>`), 与任务 id (`job-<n>`) 是两个值。

### 2.2 现有的并发上限在哪 (本次要删掉)

| 位置 | 现状 | 本方案 |
|---|---|---|
| `AgentHost::Config::maxConcurrentSubagents` (默认 8) | 进程级并发上限, 同步/异步一起卡 | **删除** |
| `AgentHost::runningSubagentCount_` / `SpawnCleanup` 计数 | 上面那道上限的计数实现 | **删除** |
| `AgentHost::Config::maxDepth` (默认 3) + `sessionDepth_` | 嵌套深度 (防递归) | **删除** (按 "不限制深度") |
| 队列条数 | 无 (本来就没有队列) | 也不设上限 |
| `kMaxChannelsPerRoot` / `kMaxTriggersPerInstance` | 插件运行时表的通道/触发器预算 | 不动 (与本文无关) |

### 2.3 回收: "销毁即回收"已经成立

`spawnOneTask` 收尾 `co_await cleanup.host->destroyAgentAsync(agentId)`, 配合 `makeSubagentConfig`
(`enableSessionStore=false`、`pluginMountPolicy=None`、清空 mcp/rag/skill/memory)。结论:

- 异步任务跑完连 `AgentContext` (含自己的 `SessionsManager`) 一起销毁, **不会**在创建者内存里留下会话对象;
- 所以"重置/回收"不需要新接口 —— 这也是放弃常驻通道池的主要理由;
- 反过来说: 现在它一定不留痕, 想"事后回看这个任务做了什么"就查不到 → §3.8 的可选会话存储。

### 2.4 为什么"父死子活"现在做不到

| 依赖 | 现状 | 创建者销毁后的后果 |
|---|---|---|
| 派生配置 | 从 `parentAgentCtx` 取 `parentConfig`, 再 `makeSubagentConfig` | 取不到配置 → 直接返回错误 |
| 结果去处 | `parentCtx->eventHub->postValue/postEnd` | 没有中枢 → 结果丢弃 (只记 WARN) |
| HIL | 子会话 `io` / `bus` 借父会话的 | 父会话没了 → 有人提问没人答, 等到超时 |
| 工具继承 | `tools: ["*"]` 解析为**父 agent 的**工具名 | 没有父上下文 → 白名单为空 |
| 取消 | `cancelAsyncJobs(parentCtx)` 在 agent 关停时调用 | 正常, 但要区分"创建者死"与"任务该跑完" |

结论: 要"创建者可以先销毁", 提交时就必须自带快照, 运行期不再回读创建者 (§3.3)。

### 2.5 结果是怎么回到会话的

- 结果值形状: `{resultId, subagent, agentId, content, hasError, Canceled, errorMessage}`,
  紧跟 `postEnd(eventName, {...})`;
- 事件名默认 `agentxx.agent.event.subagent.done.<result_id>`; 自定义名走 `validateAgentEventName`,
  **只接受内置族 `agentxx.agent.event.*`**; 值事件族的判定在 `agentxx/event/naming.h`
  (`isEventName`: `agentxx.agent.event.*` 或 `plugin.<名>.*`);
- **等待不支持按值过滤**: `EventWait` 只按事件名匹配, 所以"每任务一个事件名"是现在唯一的定向等待手段;
- 没人等时值留在中枢记录里 (`last` / `history`), 后注册的等待可用 `replay` 取回;
- 会话唤醒靠 `EventWait` (投递模式 `next-step` / `inject` / `next-turn` / `none`), 经 `SessionInputSink`
  交回当前驱动者排队; 插件也能 `register_wait` 替会话登记。

### 2.6 插件面现状

- `agentxx.agent.runtime` 表只有根/通道/触发器/定时器; 没有"派一个后台任务"。
- `agentxx.agent.tools.call_tool_async` 走不通: 见 §2.1 的 `tool_call_id` 限制。
- 插件能观察的只有通道事件 (`TURN_*`) 与总线字符串事件; 异步完成值**不经总线**
  (`hub->postValue` 只写中枢), 触发器订阅不到。

### 2.7 派生子代理的配置来源

`makeSubagentConfig` 把 `pluginMountPolicy` 置 `None`、清空 `pluginMountList` / `mcpServerUrls` /
`skillDirPaths` / `memoryFilePaths` / `ragDocsPaths`, 于是子代理**只有内置工具**。
`SubagentBatchItem` 与 `agentxx_subagent` 都**没有**挂载清单字段 —— 这是"重任务跑不动"的硬阻塞。
本方案改为显式清单 (§3.3), 默认仍为空 (与现状一致)。

### 2.8 界面现状

Info 侧边栏的段落数据全部来自 `WireContextStats`: 服务端 `toWireContextStats` 统一组装 → `makeContextStats`
按需带字段 (空值不发) → 客户端 `TUIClientAgentIO::onContextStats` 解析 →
`tui_sidebar_content.cpp` 渲染 (会话标题 / Context / Append / 插件段落)。
追加一个"后台任务"数组字段不需要改消息类型, 老客户端忽略即可。

### 2.9 缺口清单

| 需求 | 现状 | 缺什么 |
|---|---|---|
| 统一的异步任务 (tool/agent) | 只有子代理可异步 | 模块 + `kind` |
| 排队等位 | 撞全局上限直接失败 | 队列 + 每队列额度 (并删全局上限/深度) |
| 创建者先死的任务 | 跑不起来/交不出结果 | 提交快照 + 结果去处多样化 |
| 后台任务的工具集 | 只有内置工具 | 显式挂载清单 |
| 取消 | 只有 agent 关停时整批取消 | 按 id / 按创建者的显式取消 (含模型侧) |
| 目标等待 | 只能靠"每任务一个事件名" | 单一事件名 + 按 id 过滤等待 |
| 模型查询 | 无 | 事件工具新 `opt` |
| 界面展示 | 无 | `WireContextStats` 追加字段 + Info 段 |
| 插件提交 | 无入口 | runtime 表追加成员 |
| 会话可落盘 | 强制不落盘 | `session_store` 开关 |
| 任务台账 | 跑完即无痕 | 内存条目 (按 `linger` 回收, 见 §3.5) |

## 3. 设计

### 3.1 名词与改名

| 层 | 名词 | 含义 |
|---|---|---|
| 概念 | **agent** | 进程内一个独立 `BaseAgent` + `AgentContext` (根 / 插件挂的根 / 异步任务的 agent 平等) |
| 概念 | **async work (异步任务)** | 一次后台执行单元, `kind` = `tool` (异步工具) / `agent` (启动一个 agent) |
| 概念 | **async_id** | 任务 id (`async-<n>`); **同时就是该任务 agent 的 `sessionId`** (一个值, 不另造) |
| 概念 | **创建者 (creator)** | 提交方 (根 agent / 插件根 / 另一个异步任务); 只是归属记录, 不是生命周期 |
| 概念 | **队列键 (queue_key)** | 排队与额度的分组键, 默认 = 创建者 agentId |
| 概念 | **任务条目 (entry)** | 进程内的台账: id / kind / 状态 / 时间 / 结果摘要。用于排队、查询、Info 段; **不落盘**, 不是会话记录 (§3.5) |

改名 (**直接替换, 不保留兼容层**):

| 现名 | 新名 |
|---|---|
| `AgentHost::AsyncSubagentJob` | `AsyncWorkEntry` |
| `AgentHost::startAsyncSubagentTask` | `AsyncWorkManager::launch` (由 `submit` 触发) |
| `AgentHost::runAsyncSubagentTask` | `AsyncWorkManager::runner` |
| `AgentHost::runningAsyncJobs()` | `runningAsyncWork()` (+ `queuedAsyncWork()`) |
| `AgentHost::cancelAsyncJobs(ctx)` | `cancelAsyncWorks(ctx)` |
| `AgentHost::cancelAsyncJobsOfSession(sid)` | `cancelAsyncWorksOfSession(sid)` |
| `asyncJobs_` / `asyncJobsMutex_` / `asyncJobSeq_` | `asyncWorks_` / `asyncWorksMutex_` / `asyncWorkSeq_` |
| `jobId` (`job-<n>`) / 日志 `[subagent] async job` | `asyncId` (`async-<n>`) / `[async_work]` |
| `AgentHost::Config::maxConcurrentSubagents` / `maxDepth` + `runningSubagentCount_` / `sessionDepth_` | **删除** (§3.4) |
| 任务 session id (`subagent_<名>_<agentId>_<n>`) | 直接用 `asyncId` |
| 结果值里的 `subagent` / `resultId` 键 | `asyncId` (单一标识) |
| 事件名 `agentxx.agent.event.subagent.done.<result_id>` | **固定** `agentxx.agent.async.done` (§3.6) |
| 工具参数 `async` / `event_name` | **删除** (开关统一为 `async_work.enable`) |
| 配置块 `async_agent.*` | `async_work.*` (§3.11) |

**不改**: 模型工具名 `agentxx_subagent`, `SubagentBatchItem` 字段名 (内部仍按 subagent 中断传递),
`service.subagent` 主题, 配置键 `subagent.enable` (含义仍是"模型能否看到委派工具")。

### 3.2 模块边界与文件布局

```
                 ┌──────────────────────────── AgentHost (进程一份) ────────────────────────────┐
提交入口         │  AsyncWorkManager                                                           │
─────────────────│   ├─ std::map<asyncId, AsyncWorkEntry>     台账 (queued/running/finished)     │
 模型:           │   ├─ std::map<queueKey, AsyncWorkQueue>    每队列 FIFO + 额度 3 (无其他上限)  │
 任何工具调用    │   ├─ submit(spec, creator) → {asyncId, state}                               │
  (含 subagent)  │   ├─ cancel(id) / cancelByCreator / cancelBySession                          │
─────────────────│   ├─ pump(queueKey)  队列有名额就 launch 下一个                              │
 插件:           │   ├─ launch(entry)  建 agent → 跑 → destroy → 发事件 → pump 下一个            │
 runtime 表      │   ├─ entryJson(id) / listJson(filter) / counts                               │
  submit_async_  │   └─ shutdown(reason)                                                       │
  work           │                                                                             │
─────────────────│  AgentRegistry (平等登记: 根 / 插件根 / 异步 agent)                          │
 C++ (宿主):     │  claimSession (async_id 作为 sessionId 的会话级独占)                          │
 submitAsyncWork │                                                                             │
                 └─────────────────────────────────────────────────────────────────────────────┘
```

新增文件:

| 文件 | 内容 |
|---|---|
| `agent/lib/include/agentxx/agent/async_work.h` | `AsyncWorkSpec` / `AsyncWorkState` / `AsyncWorkEntry` / `AsyncWorkSubmitResult` / `AsyncWorkManager` |
| `agent/lib/src/agent/async_work.cpp` | 队列、准入、launch、回收、台账、统计 (从 `agent_host.cpp` 把异步派发这段搬出来) |

不改动的地方:

- **不新增中间件句柄**。`SubagentManagerMiddlewareHandle` 继续负责"把 `agentxx_subagent` 注入模型 +
  挂总线服务"; 队列归 host。
- **不改 `AgentRegistry` 结构**: 异步任务的 agent 依旧是 `AgentNode`, `parentAgentId` 填真实创建者 agentId。
- **模块是进程级设施、agent 级开关**: 与 subagent 同为内置能力 (普通会话 / 插件挂的根 / chatbot 人设根
  共用同一份 manager); 每个 agent node 用 `async_work.enable` 决定"我能不能提交后台任务、工具是否异步、
  模型/界面能否看到后台任务"。插件根经 `config_json` 的 `asyncWork` 段配置 (§11.1)。

### 3.3 提交自包含: 显式清单

一次提交把"运行这个任务需要的一切"都带走, 运行期不再回读创建者。**默认全部为空**
(= 现状 `makeSubagentConfig` 的效果: 只有内置工具), 要什么就列什么:

| 字段 | 默认 | 说明 |
|---|---|---|
| `kind` | `agent` | `agent` = 启动 agent (子代理形态); `tool` = 异步工具 (§10.4) |
| `tools` | 空 = 默认工具集 | `[]` = 无工具; `["*"]` = 继承创建者当时的 `toolNames`; 名单 = 只留这些 |
| `plugins` | 空 = 不挂插件 | 与 yaml `plugin.mount` 同构: `{"mode":"listed","list":[...],"args":{...}}` |
| `mcp_servers` | 空 | MCP 服务 URL 列表 (直接映射 `AgentConfig::mcpServerUrls`) |
| `skills` | 空 | Skill 目录列表 |
| `memory_files` | 空 | Memory 文件列表 |
| `rag_docs` | 空 | RAG 文档路径列表 |
| `work_dir` | 空 = 跟随创建者 | |
| `language` | 空 = 跟随创建者 | |
| `permission_allow_paths` / `permission_deny_paths` | 空 = 跟随创建者 | 与创建者同一套路径边界 |
| `session_store` (+ `dir`) | 关 | §3.8 |
| `interrupt_policy` (+ timeout) | `bubble` | §3.7 |
| `model` | 空 = 创建者的 subagent 模型 | |
| 结果去处 | 事件 + 台账 | §3.6 |

对 chatbot 的用法 (§11.1): 它手里已有自己的插件列表、人设的 MCP/Skill/Memory 配置, 直接
"去掉 `agentxx_chatbot` 后原样传进去"即可:

```cpp
// chatbot 侧 (伪代码)
spec.kind        = AsyncWorkKind::Agent;
spec.plugins     = pluginMountFrom(persona.mount, /*exclude=*/"agentxx_chatbot");
spec.mcp_servers = persona.mcpServers;      // 与主会话同一份
spec.skills      = persona.skillDirs;
spec.memory_files= persona.memoryFiles;
spec.tools       = {"*"};                    // 或按人设白名单
```

### 3.4 只留一条约束: 每队列并发额度

| 约束 | 现状 | 本方案 |
|---|---|---|
| 每队列并发 | — | `async_work.max_concurrent` (默认 **3**) |
| 排队条数 | — | **不限制** |
| 嵌套深度 | `maxDepth` 默认 3 | **不限制** (删除) |
| 全局并发 | `maxConcurrentSubagents` 默认 8 | **删除**, 不做替代 |

调度规则:

1. `submit` 只做校验 + 入队, 立即返回 `{asyncId, state: queued|running}`; 只有"未启用"与"参数非法"会被拒,
   **不会因额度/排队条数被拒**;
2. `pump(queueKey)` 在**容器 io 线程**上跑: 该队列 `running < max_concurrent` 时取队首 launch;
3. 每个任务结束 (成功/失败/取消) → 出队 → `pump` 同一个队列键;
4. **队列之间互不影响**: 一个会话派满 3 个不会挡别的会话;
5. **队列只在内存**, 进程重启即丢 (非目标);
6. `async_work.enable=false` 的 agent node 不接受提交: 工具调用按旧行为同步执行, 插件提交回`not_enabled`
   (§6)。

不限制深度带来的后果写在 §10.3 (递归风险由提交方自己负责: 是否把委派工具给后台任务)。

### 3.5 生命周期与平等性 (+ 任务条目是什么)

四条硬规则:

1. **创建者只是账本**: 提交后创建者的 `AgentContext` 只被 `weak_ptr` 持有 (用于结果投递与 HIL);
   拿不到它不算错误, 只按"没有归处"降级 (§3.6)。
2. **创建者先死, 任务照跑**: 配置、工具、模型、结果去处都在快照里。此时结果无处投递 → 写台账 + 日志
   (`delivered=false`); HIL 无处冒泡 → 按 `interrupt_policy` 处理 (§3.7)。
3. **默认不级联取消**: 创建者某一轮被取消/失败/正常结束**不**带走后台任务; 只有 agent 关停
   (`BaseAgent::shutdown`) 与进程退出会按归属批量取消。
4. **任务条目 (entry)**: 每个 async work 在 manager 里有一条内存台账 —— `asyncId` / `kind` / `state` /
   `queueKey` / 创建者 / 开始结束时间 / 结果摘要 (正文前 N 字节) / 错误 / 是否投递成功。
   用途: 队列出队判断、`opt=status` 与 `opt=works`、Info 段展示、插件查询。
   **它不是会话记录、也不落盘**: 运行中的一直在; 结束后保留 `linger_ms` (默认 5 分钟) 供查询, 到期自动清除
   (纯内存回收, 不是"限制"); 要看完整执行过程请开 §3.8 的会话存储。

#### 3.5.1 agent 间的交互面 (只用这三个, 不共享状态)

| 面 | 用途 | 现状 |
|---|---|---|
| **事件中枢** (值事件) | 完成通知/状态; 谁等谁拿, 支持 `first/each/all` 与 `replay` | 已有 (`EventHub`); 异步任务的结果走这条 |
| **`agent.message` RR** | 点对点问答 (需要对方有 mailbox) | 已有 (`AgentHost::sendMessage` / `setMailbox`) |
| **A2A** | 跨进程/跨设备的 agent 对话 | 已有 (`registerRemoteAgent` / `sendViaA2a`) |

明确**不共享**: `Session` / `AgentContext` / 工具实例 / 中间件句柄 / 变量。两条 agent 交换数据只有
"事件值"与"消息"两种形态 —— 这也是"创建者可以先销毁"能成立的前提。

### 3.6 结果投递: 一个事件名 + id 传参

| 去处 | 触发条件 | 实现 |
|---|---|---|
| **事件** (主路) | `event.enable` 打开且创建者中枢仍在 | 固定事件名 `agentxx.agent.async.done`, 每次完成发一个值 + `postEnd` |
| **会话输入** (便捷) | `notify_delivery` 非空 (`next-step` / `next-turn` / `inject`) | manager 在**入队前**替该会话 `register_wait` (带 `match={"asyncId":...}` 与 `replay`), 完成时值投递 → 会话被唤醒 |
| **台账** (兜底) | 永远 | `AsyncWorkEntry.resultText/error`, 查询与 Info 段可见, 不依赖任何会话存活 |

值形状 (单一事件名, 靠 `asyncId` 区分):

```jsonc
{ "asyncId": "async-7", "kind": "agent", "name": "research",
  "content": "……", "hasError": false, "canceled": false, "errorMessage": "",
  "creatorSessionId": "...", "startedAtMs": 0, "finishedAtMs": 0 }
```

- 事件名进 `agentxx/event/naming.h` 的**值事件族**: 新增 `agentxx.agent.async.` (框架保留),
  `agentxx.agent.event.*` 与 `plugin.*` 的规则完全不动;
- **等待要能定向**: `EventWait` 现在只按事件名匹配, 单名多任务时等不到"我那一条"。因此给等待加一个可选的
  **值过滤** `match` (顶层字段相等匹配), 例: `match={"asyncId":"async-7"}`;
  模型侧对应 `agentxx_event {opt:"await", id:"async-7"}` / `{opt:"watch", id:"async-7"}`,
  插件侧对应 `register_wait` 的新字段 `match_json`;
- 其他"事件与动作接口"同一套: 查状态、取消、等待都只传 `id`, 不再自己编事件名;
- `event.enable=false` 时没有中枢 → 只走台账兜底 (提交回执里没有事件可等, 只能查 `opt=status`)。

### 3.7 HIL 与权限 (可配)

后台任务没有界面, 触发权限询问/人工中断必须按配置决断:

| `interrupt_policy` | 权限询问 (`service.permission`) | 其他节点中断 | 实现 |
|---|---|---|---|
| `bubble` | 创建者存活 → 冒泡到创建者会话 (现状行为); 创建者已销毁 → 按拒绝 | 同左 (冒泡/超时) | 子会话 `io`/`bus` 借父会话的 |
| `deny` | 一律拒绝 | 按取消收尾 | 在异步 agent **自己的总线**上注册固定应答器 |
| `allow` | 一律允许 | 用空结果 resume (等价"跳过该询问") | 同上, 应答允许/空值 |

- 配置入口: `async_work.interrupt_policy` (agent 级默认, 见 §3.11) + 插件根 `config_json` 的
  `asyncWork.interruptPolicy` + 单次提交覆盖; `interrupt_timeout_ms` 作为最后保险 (到点按取消收尾);
- `deny`/`allow` 的应答器挂在 manager 里 (与 `registerSubagentService` 同一处注册/注销):
  不冒泡时总得有人在子会话的总线上答 (`PermissionMiddlewareHandle` 经会话 `bus` 发 RR);
- **chatbot 默认 `allow`** (§11.1): 机器人后台任务不要在群里弹权限窗。

### 3.8 会话持久化 (可选)

默认**不落盘** (与现状一致): `enableSessionStore=false`, 跑完连会话一起销毁。开启时:

```yaml
async_work:
  session_store: false          # 默认关
  session_store_dir: ""         # 空 = {创建者会话目录}/agent
```

- 目录: **`{创建者会话目录}/agent/{async_id}`**, 即
  `sessionStoreDirectory = {创建者会话目录}/agent`, `sessionId = async_id` →
  库文件 `{创建者会话目录}/agent/{async_id}/session.db`;
- 独占: 用 **会话级** `AgentHost::claimSession(存储根, async_id, owner)` 而不是 agent 级
  `claimSessionStoreRoot` —— 同一个创建者会话下的多个后台任务共用一个 `agent` 目录, 只有 `async_id` 不同;
- **不做自动恢复**: 重启不会把上次的任务"接着跑"; 落盘只服务"事后回看"
  (会话历史、用量台账 `usage` 表按现有机制写入);
- `async_work.session_store_keep_days` (默认 0 = 不自动清理) 留给运维。

### 3.9 模型可见面 (扩展 `agentxx_event`, 工具去掉 `async` 参数)

**工具侧统一**: `agentxx_subagent` 不再有 `async` / `event_name` 参数 —— 调用即提交一次 async work,
立即拿到 `async_id`; 是否异步由 `async_work.enable` 决定 (关闭时该工具按旧行为同步等结果)。
"我要现在就拿到结果"用 `opt=await id=<async_id>`。

`agentxx_event` 的 `opt`:

| `opt` | 作用 | 关键参数 |
|---|---|---|
| `works` (新) | 列后台任务 (数量 + 明细) | `scope`: `mine`(默认)/`all`; `state`: `queued|running|done|all`; `kind`: `agent`(默认)/`tool`/`all`; `id`; `limit` |
| `status` (扩展) | 查事件/等待 (**原**) 或查某条后台任务 (给 `id`) | `wait` / `event` / `id` |
| `await` (扩展) | 阻塞等 (给 `event` 或给 `id` 等某条任务) | `event` / `id` / `timeout_ms` |
| `watch` (扩展) | 非阻塞登记 (给 `event` 或给 `id`) | `event` / `id` |
| `cancel` (扩展) | 取消等待 (**原**) 或取消后台任务 | `wait` / `all` (原) + `id` / `scope: "mine"` |
| `list` / `emit` / `end` | 事件清单 / 发值 / 结束流 (**原**) | 不变 |

`opt=works` 的返回文本 (给模型读):

```
后台任务: 运行 2 / 排队 3 (本会话上限 3)
- async-7 [running] agent research 模型=m1 已跑 00:42
- async-8 [running] agent write-report 模型=m1 已跑 00:05
- async-9 [queued]  agent translate 前方 2
最近完成: async-3 agent [ok] 用时 01:20
```

约定:

- 工具注册条件从"有中枢"放宽为"有中枢 **或** 启用了 async work";
- `opt=works` 只给摘要, 不给上下文正文; `id` 查单条时结果正文按 `result_max_bytes` 截断;
- 取消运行中的任务走取消令牌, 终态仍会发布事件 → 等待方不会挂到超时;
- 推荐用法写进提示词: 提交 → 拿到 `async_id` → `opt=watch id=<id>` (或 `opt=await id=<id> timeout_ms=<n>`);
  不要了 `opt=cancel id=<id>`。

### 3.10 插件可见面 (`agentxx.agent.runtime` 表追加成员)

表尾追加 4 个成员 (基线不变, `VERSION` +1, `MIN_VERSION` 不动, 读取方用 `ifaceHas` 判定):

```c
int32_t submit_async_work(host, const AgentxxPluginAsyncWorkSpec* spec, PluginxxString* out_async_id);
int32_t cancel_async_work(host, const PluginxxStringView* async_id);
int32_t get_async_work(host, const PluginxxStringView* async_id, PluginxxString* out_json);
int32_t list_async_work(host, const PluginxxStringView* session_id, PluginxxString* out_json);
```

`AgentxxPluginAsyncWorkSpec` 字段 (全部可选/有默认, 带 `struct_size`; **没有 `event_name`**):

| 字段 | 说明 |
|---|---|
| `session_id` | 创建者会话 (通道的会话); 结果事件与 HIL 都按它找创建者 |
| `queue_key` | 空 = 创建者 agentId |
| `kind` | 0=`agent` 1=`tool` (本期只支持 0) |
| `name` / `system_prompt` / `message` | 身份与任务正文 (与 `agentxx_subagent` 参数同义) |
| `model` | 空 = 创建者的 subagent 模型 |
| `tools_json` | 空 = 默认; `[]` / `["*"]` / 名单 |
| `plugins_json` | yaml `plugin.mount` 同构; 空 = 不挂 |
| `mcp_servers_json` / `skills_json` / `memory_files_json` / `rag_docs_json` | 显式清单 (§3.3) |
| `work_dir` / `language` | 空 = 跟随创建者 |
| `permission_allow_json` / `permission_deny_json` | 空 = 跟随创建者 |
| `session_store` (+ `session_store_dir`) | §3.8 |
| `interrupt_policy` / `interrupt_timeout_ms` | §3.7 |
| `notify_delivery` | 空 = 不投递会话 |
| `on_event` + `user_data` | 任务级回调 (`queued`/`started`/`finished`/`canceled`), **在容器 io 线程调用、必须快速返回** |
| `time_limit_ms` | 0 = 不限 |

归属与回收: 与根/通道/触发器同一处 —— 插件禁用/卸载时按创建者整批取消 (只置取消令牌, 收尾照常发终态),
台账条目随之释放; 能力声明在进程级, 重复声明幂等。

### 3.11 配置

**开关是 agent node 级的**, 不是进程级 (与 `subagent.enable` 同级):

| 键 | 默认 | 说明 |
|---|---|---|
| `async_work.enable` | `true` | 该 agent node 是否启用异步任务: 工具调用走后台 (结果经事件)、插件可提交、模型/界面可见; 关 = 工具同步执行, 提交被拒 |
| `async_work.max_concurrent` | `3` | 每队列并发额度 (唯一的并发约束) |
| `async_work.interrupt_policy` | `bubble` | `bubble` / `deny` / `allow` (§3.7) |
| `async_work.session_store` | `false` | §3.8 |
| `async_work.session_store_keep_days` | `0` | 目录保留天数 (0 = 不自动清理) |
| `async_work.linger_ms` | `300000` | 结束后台账条目保留时长 (内存回收) |
| `async_work.result_max_bytes` | `8192` | 台账里保留的结果正文上限 |

**没有**: `max_queue` (不限排队)、`max_depth` (不限深度)、`max_concurrent_global` (无全局闸门)。
`subagent.enable` 含义不变 (模型能否看到委派工具)。

### 3.12 可观测性

| 位置 | 新增内容 |
|---|---|
| `KeyMetrics` | `asyncWorkSubmitted` / `asyncWorkFinished` / `asyncWorkFailed` / `asyncWorkCanceled` / `asyncWorkQueuedPeak` / `asyncWorkRunMs` |
| 加载快照 (`assembly_snapshot.cpp`) | `root["async_work"] = {enable, maxConcurrent, interruptPolicy, sessionStore, running, queued, entries}` |
| `--dump-diagnostics` | 同上的文字段 (`async_work: running=1 queued=2 finished=5`) |
| 插件 `list_runtime` | 增加 `asyncWorks` 段 (本实例提交的那些) |
| 日志 | `[async_work] submitted/launch/finished/canceled` 各一条, 带 `asyncId` / `creator` |

### 3.13 界面可见面 (TUI Info 段)

数据通道复用 `WireContextStats` (追加 JSON 字段, 不改消息类型):

```jsonc
// makeContextStats 里按需追加 (为空则不写, 老客户端忽略)
"asyncWorks": [
  {"id":"async-7","kind":"agent","name":"research","state":"running","elapsedMs":42000},
  {"id":"async-9","kind":"agent","name":"translate","state":"queued","queueAhead":2}
]
```

- 服务端: `SessionServerAgentIO::sendContextStats` 组装时向 `AgentHost` 取"本会话创建的"运行/排队/最近完成项
  (最多 10 条);
- 触发时机: 状态变化 (提交/开始/结束/取消) 时推一次, 有运行项时按现有统计节拍低频刷新;
- 客户端: `WireContextStats` 解析新增数组 → Info tab 新增一段「后台任务」(排在 Context 段之后、插件段落之前):
  `运行 2 / 排队 1`, 逐条 `async-7 research 00:42`, 已完成显示 `✓/✗`; 空则显示"( 无 )";
- **只在 Info 段展示, 不做独立面板、不在 TUI 里取消** (取消走模型工具或 chatbot 自己的面板)。

## 4. 接口设计

### 4.1 C++ (libagentxx)

```cpp
namespace agentxx::agent {

/// 异步任务状态
enum class AsyncWorkState { Queued, Running, Finished, Failed, Canceled };

/// 任务种类 (本期只落 Agent; Tool 为异步工具预留)
enum class AsyncWorkKind { Agent = 0, Tool = 1 };

/// 中断(权限询问/HIL)处理策略
enum class AsyncInterruptPolicy { Bubble, Deny, Allow };

/// 提交参数: 自包含, 提交后不再回读创建者; 派生配置清单默认全空
struct AsyncWorkSpec {
    AsyncWorkKind kind = AsyncWorkKind::Agent;

    /// 归属账本 (由提交入口填; C++ 调用方可留空 = 内置根)
    std::string creatorAgentId;
    std::string creatorSessionId;   ///< 结果事件与 HIL 的去处; 可空
    std::string queueKey;           ///< 空 = creatorAgentId

    /// 身份与任务
    std::string name = "async";
    std::string systemPrompt;
    std::string message;
    std::optional<utilxx_base::Json> messages; ///< 结构化上下文 (优先于 message)
    std::string model;                          ///< 空 = 创建者 getSubagentModel()
    std::optional<utilxx_base::Json> tools;     ///< 空=默认 / []=无 / ["*"]=继承创建者 / 名单

    /// 显式挂载清单 (空 = 不挂, 与现状子代理一致)
    std::optional<utilxx_base::Json> plugins;   ///< yaml `plugin.mount` 同构
    std::vector<std::string>         mcpServers;
    std::vector<std::string>         skills;
    std::vector<std::string>         memoryFiles;
    std::vector<std::string>         ragDocs;
    std::string                      workDir;    ///< 空 = 跟随创建者
    std::string                      language;   ///< 空 = 跟随创建者
    std::vector<std::string>         permissionAllowPaths;
    std::vector<std::string>         permissionDenyPaths;

    /// 会话持久化 (§3.8): 目录 = {创建者会话目录}/agent/{asyncId}, sessionId = asyncId
    bool        sessionStore = false;
    std::string sessionStoreDir; ///< 空 = {创建者会话目录}/agent

    /// 中断策略 (§3.7)
    AsyncInterruptPolicy      interruptPolicy = AsyncInterruptPolicy::Bubble;
    std::chrono::milliseconds interruptTimeout{0};
    std::chrono::milliseconds timeLimit{0};

    /// 结果 (§3.6): 事件名固定 `agentxx.agent.async.done`, 无需指定
    std::string notifyDelivery; ///< 空 = 不投递会话; next-step/inject/next-turn
    int64_t     lingerMs = -1;  ///< <0 = 用配置默认
    size_t      resultMaxBytes = 0;

    /// 同上下文压缩等特殊场景 (与 SubagentBatchItem 同义)
    bool keepProvidedSystemMessage = false;
    bool inheritRequestTools       = false;
    bool toolExecutionDisabled     = false;
};

/// 提交回执 (队列不限条数, 因此只有"受理/拒绝"两种)
struct AsyncWorkSubmitResult {
    bool            accepted = false;
    std::string     asyncId;  ///< "async-<n>"; 同时是该任务 agent 的 sessionId
    AsyncWorkState  state = AsyncWorkState::Queued;
    size_t          queueAhead = 0;
    std::string     error;    ///< accepted=false 时的原因 (未启用 / 参数非法)
};

/// 任务条目 (查询与界面展示用; 不含上下文正文)
struct AsyncWorkEntry {
    std::string     asyncId;
    AsyncWorkKind   kind = AsyncWorkKind::Agent;
    AsyncWorkState  state = AsyncWorkState::Queued;
    std::string     queueKey;
    std::string     creatorAgentId;
    std::string     creatorSessionId;
    std::string     agentId;    ///< 运行期 registry id (结束后仅用于日志关联)
    std::string     name;
    std::string     model;
    int64_t         createdAtMs = 0;
    int64_t         startedAtMs = 0;
    int64_t         finishedAtMs = 0;
    std::string     resultText; ///< 截断后的结果正文
    std::string     error;
    bool            hasError = false;
    bool            canceled = false;
    bool            delivered = false; ///< 结果是否真的投递到了中枢
};

} // namespace agentxx::agent
```

`AgentHost` 上的公开入口:

```cpp
class AgentHost {
public:
    /// 提交一个后台任务 (任何线程; 内部投递到容器 io 线程)
    AsyncWorkSubmitResult submitAsyncWork(AsyncWorkSpec spec,
                                          std::shared_ptr<AgentContext> creator);

    /// 单条取消 / 按创建者 / 按创建者会话 (只置取消令牌; 排队中直接出队)
    bool   cancelAsyncWork(std::string_view asyncId);
    size_t cancelAsyncWorks(const std::shared_ptr<AgentContext>& creator = nullptr);
    size_t cancelAsyncWorksOfSession(std::string_view creatorSessionId);

    /// 查询 (creatorAgentId 为空 = 全部; 供工具/插件/界面共用)
    utilxx_base::Json asyncWorkJson(std::string_view asyncId) const;
    utilxx_base::Json asyncWorksJson(std::string_view creatorAgentId,
                                     bool includeFinished = true,
                                     size_t limit = 20) const;
    size_t runningAsyncWork() const;
    size_t queuedAsyncWork(const std::string& queueKey = {}) const;

    /// 关停 (agent 关停/进程退出): 取消全部在跑与排队项
    size_t shutdownAsyncWorks(std::string_view reason);
};
```

`BaseAgent::shutdown` 里把 `cancelAsyncJobs(ctx)` 换成 `cancelAsyncWorks(ctx)` (语义不变)。

### 4.2 配置键

见 §3.11 的表。落地时同步四处:

1. `AgentConfig` 加 7 个字段 (`enableAsyncWork`, `asyncWorkMaxConcurrent`, ...);
2. `agent/client/src/config_loader.cpp` 增加 `async_work` 块解析 (照 `event` 的写法, 非法取值警告并回退默认);
3. `agent/lib/src/plugins/plugin_manager_runtime_config.cpp` 让插件根 `config_json` 的 `asyncWork` 段映射到
   同名字段;
4. `agent/test/core/test_config_keys.cpp` 的键表加同名 `KeyProbe`; 渲染生成物 `config-keys.md`。

删除项: `AgentHost::Config::maxConcurrentSubagents` / `maxDepth`、`sessionDepth_`、`runningSubagentCount_`
(代码参数, 没有 yaml 键, 直接去掉)。

### 4.3 插件表成员 (append-only)

`agentxx.agent.runtime`:

```c
/* ---- 异步任务 (v1 尾部追加; 基线不变) ---- */
typedef struct AgentxxPluginAsyncWorkSpec {
    uint32_t struct_size;                 ///< sizeof(...); 0 = 基线布局
    int32_t  kind;                        ///< 0=agent 1=tool (本期只支持 0)
    int32_t  interrupt_policy;            ///< 0=bubble 1=deny 2=allow
    int32_t  session_store;               ///< 0/1
    int32_t  _reserved;
    int64_t  interrupt_timeout_ms;        ///< 0 = 不限
    int64_t  time_limit_ms;               ///< 0 = 不限
    PluginxxStringView session_id;        ///< 创建者会话 (通道的会话)
    PluginxxStringView queue_key;         ///< 空 = 创建者 agentId
    PluginxxStringView name;
    PluginxxStringView system_prompt;
    PluginxxStringView message;
    PluginxxStringView model;             ///< 空 = 创建者的 subagent 模型
    PluginxxStringView tools_json;        ///< 空 = 默认; "[]" / "[\"*\"]" / 名单
    PluginxxStringView plugins_json;      ///< yaml plugin.mount 同构; 空 = 不挂
    PluginxxStringView mcp_servers_json;  ///< 字符串数组
    PluginxxStringView skills_json;       ///< 字符串数组
    PluginxxStringView memory_files_json; ///< 字符串数组
    PluginxxStringView rag_docs_json;     ///< 字符串数组
    PluginxxStringView work_dir;          ///< 空 = 跟随创建者
    PluginxxStringView language;          ///< 空 = 跟随创建者
    PluginxxStringView permission_allow_json; ///< 字符串数组; 空 = 跟随创建者
    PluginxxStringView permission_deny_json;  ///< 字符串数组; 空 = 跟随创建者
    PluginxxStringView notify_delivery;   ///< 空 = 不投递会话
    AgentxxPluginAsyncWorkEventFn on_event; ///< 可空; 容器 io 线程调用, 必须快速返回
    void* user_data;
} AgentxxPluginAsyncWorkSpec;
```

(没有 `event_name` / `result_id`: 事件名固定, 标识统一用返回的 `async_id`。)

回调 `kind` 取值 (追加在枚举尾部): `QUEUED` / `STARTED` / `FINISHED` / `CANCELED`,
`payload_json` 带 `asyncId` / `kind` / `queueAhead` / `hasError` / `canceled` / `error`。
回调纪律: **容器 io 线程、必须快速返回**, 字符串只在回调期间有效。

版本步骤 (项目既定规则):

1. 4 个成员只加在 `AgentxxPluginRuntimeIface` **表尾**; `_BASELINE_SIZE` 不变, `_VERSION` 1 → 2,
   `_MIN_VERSION` 仍 1;
2. 插件侧 `ifaceHas<&Iface::submit_async_work>(iface)` 判定, 老主程序自动降级;
3. 新结构体给 `AGENTXX_PLUGIN_ASYNC_WORK_SPEC_BASELINE_SIZE`, 读取方用 `specSizeOk` / `specHas`;
4. `plugin_iface_meta.h` 加一行 `IfaceMeta`; `boundaries` 成员顺序台账补一行;
   `plugin_runtime` 补"三态判定 + 偏移"校验。

### 4.4 工具参数

**`agentxx_subagent`(提交一个 agent 类任务)**: 删除 `async` / `event_name` 参数, 其余不变:

```jsonc
{ "subagent": "research", "message": "……", "tools": ["*"] }
// 返回: "[Async work started] id=async-7 (结果经事件 agentxx.agent.async.done 返回,
//        想立刻拿结果用 agentxx_event opt=await id=async-7)"
```

**`agentxx_event`**:

```jsonc
{ "opt": "works",  "scope": "mine", "state": "all", "limit": 10 }
{ "opt": "works",  "id": "async-7" }
{ "opt": "status", "id": "async-7" }              // 单条任务状态
{ "opt": "await",  "id": "async-7", "timeout_ms": 60000 }
{ "opt": "cancel", "id": "async-7" }
{ "opt": "cancel", "scope": "mine" }              // 取消自己名下全部
```

`schema`: `opt` 的 `enum` 追加 `"works"`; 新增 `id` 字段; `scope` / `state` / `kind` 都在 `opt=works` 里生效。
提示词 (`AgentPrompt::toolPrompt` 的 `agentxx_subagent` / `agentxx_event` 段) 同步改写:
"委派即提交后台任务, 结果由事件返回; 要用结果就 `opt=await id=<async_id>`, 不要了就 `opt=cancel id=<async_id>`"。

## 5. 关键流程

### 5.1 模型侧: 调工具即提交 (唯一路径)

```
模型: agentxx_subagent {subagent, message}
  └─ 中断 → spawnBatch → AsyncWorkManager::submit(spec 由 task 构造, 清单默认全空)
        ├─ async_work.enable=false → 工具按旧行为同步执行 (阻塞本轮, 直接返回结果文本)
        └─ enabled → 入队 → 立即返回 "[Async work started] id=async-7"
                       └─ pump: 队列有名额 → launch
                             └─ 完成 → 台账 + postValue/postEnd(`agentxx.agent.async.done`)
                                  └─ 创建者会话若登记了 wait (match asyncId) → 被唤醒
```

没有"受理后被并发上限打回"的可能: 队列不限条数, 只有额度 3 决定何时开跑。

### 5.2 插件侧提交 (chatbot 形态)

```
chatbot 队列 (插件 io 线程) ─ submit_async_work(session_id=聊天会话, plugins=自身插件去掉 chatbot,
                                mcp/skill/memory=人设配置, notify_delivery="next-step") ─▶ 核心
   ▲                                                                                    │
   │ on_event(FINISHED) → 出队记完成, 派下一个                                           │
   └────────────────────────────────────────────────────────────────────────────────────┘
                          结果事件 → 聊天会话被唤醒 → 模型读结果 → chatbot_send_message
```

插件自己的队列只做"业务优先级/重试", **并发额度交给核心** (`async_work.max_concurrent`, chatbot 配 3)。

### 5.3 取消 (模型与插件都能发起)

| 场景 | 入口 | 行为 |
|---|---|---|
| 模型取消单条 / 自己全部 | `agentxx_event {opt:"cancel", id:"async-7"}` / `{scope:"mine"}` | 运行中 → 置取消令牌 → 按取消收尾并发终态; 排队中 → 直接出队, 发 `CANCELED` 回调/条目 |
| 插件取消 | `cancel_async_work(id)` | 同上; 只能取消自己提交的 (按实例归属校验) |
| 插件被禁用/卸载 | 主程序按创建者整批取消 | 同 `cancelAsyncWorks` |
| 创建者 agent 关停 | `BaseAgent::shutdown` → `cancelAsyncWorks(ctx)` | 只取消该创建者名下的 |
| 进程退出 | `AgentHost` 析构 / `shutdownAsyncWorks` | 全部取消, 尽力发终态 |

保留现状语义: **创建者某一轮被取消不带走后台任务**。

### 5.4 创建者已销毁

| 阶段 | 行为 |
|---|---|
| 排队中 | 继续排队, 照常运行 (快照在手); 没有事件去处 → 条目 `delivered=false` |
| 运行中 | 同上; HIL 按 `interrupt_policy` 降级 (`bubble` → 按拒绝) |
| 结束时 | 跳过中枢投递, 记一条 WARN (带 asyncId), 条目仍保留到 `linger` |

### 5.5 启动/关停顺序

- 启动: `AsyncWorkManager` 随 `AgentHost` 建立 (无线程/子进程), 不依赖任何 agent 就绪;
  `async_work.enable` 只影响"该 agent node 能否提交、工具是否异步、界面是否显示";
- 关停: 先按 agent `cancelAsyncWorks` → 等收尾 (上限 5 s) → `AgentHost` 析构;
  超时未收尾的直接 `destroyAgentAsync` (与现状同一路径)。

## 6. 边界与失败模式

| 情况 | 结果 | 谁看到 |
|---|---|---|
| 该 agent 未启用 (`async_work.enable=false`) | 工具同步执行; 插件提交 `accepted=false`, `error="not_enabled"` | 工具文本 / 插件回执 |
| 队列已满额度 | 排队 (进 `queueAhead`), 不拒绝 | 工具回执 / `opt=works` / Info 段 |
| 中枢不可用 (`event.enable=false`) | 受理, 只写台账, 没有事件可等 | 回执 / `opt=status` |
| 派生配置非法 (模型不存在等) | 受理后 `state=Failed` | 台账 + (有中枢时) 结果值 |
| 会话存储目录被占用 | `accepted=false`, `error="session_store_conflict"` | 回执 |
| 运行期抛异常 | `state=Failed`, `hasError=true` | 台账 + 结果值 |
| 超 `time_limit` | 按取消收尾, `canceled=true` | 台账 + 结果值 |
| HIL 无人可答 | 按 `interrupt_policy` 决断; 条目写最后一次中断名 | 台账 `error` |
| 进程退出时仍在跑 | 尽力发终态, 之后丢失 (非目标: 不持久) | 日志 |
| 条目超过 `linger` | 由 manager 清扫 | `opt=works` / Info 段查不到 |

## 7. 影响面与兼容性

### 7.1 改名与搬迁清单

| 文件 | 改动 |
|---|---|
| `agent/lib/src/agent/agent_host.cpp` | 异步派发三段 (`spawnBatch` 的 async 分支 / `startAsyncSubagentTask` / `runAsyncSubagentTask`) 搬到 `async_work.cpp` 并改名; **删除 `maxConcurrentSubagents` 检查、`runningSubagentCount_` 计数、`maxDepth`/`sessionDepth_` 深度检查**; 任务 `sessionId` 改为 `asyncId` |
| `agent/lib/include/agentxx/agent/agent_host.h` | 删除 `AsyncSubagentJob` / `asyncJobs_` / `maxConcurrentSubagents` / `maxDepth` / `sessionDepth_` / `runningSubagentCount_`; 新增 `submitAsyncWork` / `cancelAsyncWork` / `cancelAsyncWorks` / `cancelAsyncWorksOfSession` / `asyncWorkJson` / `asyncWorksJson` / `runningAsyncWork` / `queuedAsyncWork` / `shutdownAsyncWorks` |
| `agent/lib/src/agent/base_agent.cpp` | `shutdown` 里 `cancelAsyncJobs` → `cancelAsyncWorks` |
| `agent/lib/src/agent/assembly_snapshot.cpp` | `features.async_work`; 新增 `root["async_work"]` 段与文字渲染 |
| `agent/lib/src/agent/config*.cpp` / `agent/client/src/config_loader.cpp` | 新增 `async_work` 配置块解析 |
| `agent/lib/include/agentxx/agent/config.h` | 新增 7 个字段 (§3.11) |
| `agent/lib/include/agentxx/event/naming.h` + `src/event/hub.cpp` | 值事件族增加 `agentxx.agent.async.*`; `EventWait` 支持可选值过滤 `match` (§3.6) |
| `agent/lib/src/tools/event.cpp` | `opt=works` 新增; `status`/`await`/`watch`/`cancel` 支持 `id`; schema/prompt; 注册条件放宽 |
| `agent/lib/src/tools/subagent.cpp` | 删除工具参数 `async` / `event_name`; 回执文本改为 `[Async work started] id=...`; 异步与否由 `async_work.enable` 决定 (内部直调路径按"无 `tool_call_id`"判为同步) |
| `agent/lib/src/middlewares/subagent_manager.cpp` | 同上 (参数表与提示词) |
| `agent/lib/include/agentxx/agent/io/wire_protocol.h` + `context_stats_wire.h` | `WireContextStats` 追加 `asyncWorks` 数组; `makeContextStats` 填充 (空不写) |
| `agent/lib/src/agent/io/session_server_agent_io.cpp` | `sendContextStats` 向 host 取本会话的任务摘要; 状态变化时主动推一次 |
| `agent/client/src/io/tui/agent_tui.cpp` | `onContextStats` 解析 `asyncWorks` 到 client 状态 |
| `agent/client/src/io/tui/tui_sidebar_content.cpp` + `tui_i18n.cpp` | Info 新增「后台任务」段与文案 |
| `agent/lib/include/agentxx/plugin/api/plugin_api.h` | runtime 表尾追加 4 成员 + 新结构体 + 回调类型 (`VERSION` 2) |
| `agent/lib/include/agentxx/plugin/api/plugin_iface_meta.h` | 表 `IfaceMeta` 行 (基线不变) |
| `agent/lib/include/agentxx/plugin/api/plugin_kit.h` | SDK 便捷层: `submitAsyncWork` / `cancelAsyncWork` / `getAsyncWork` / `listAsyncWork` |
| `agent/lib/src/plugins/plugin_manager_runtime*.{h,cpp}` | 4 个成员实现 + 归属回收 + `list_runtime` 增加 `asyncWorks` |
| `agent/lib/src/plugins/plugin_manager_runtime_config.cpp` | 插件根 `config_json` 支持 `asyncWork` 段 |
| `agent/test/core/test_event_subagent.cpp` | 改名调用 + 排队语义断言 (不再断言并发上限失败); `sessionId == asyncId` 断言 |
| `agent/test/**` (新增/扩展) | 见 §9 |

### 7.2 兼容性

- **wire**: 只追加 `WireContextStats.asyncWorks` (JSON 数组, 空不写); 老客户端忽略, 老服务端不下发 → 界面不显示该段;
- **模型工具**: `agentxx_subagent` 名字不变, 但**去掉 `async` / `event_name` 参数** (提交即异步, 由配置开关决定);
  `agentxx_event` 的 `opt` 新增 `"works"`, `status`/`await`/`watch`/`cancel` 新增 `id`;
- **插件 ABI**: 只追加表尾成员, `MIN_VERSION` 不动; 老主程序下插件 `ifaceHas` 判定为假后降级;
- **内部 API 直接换名**: 不留 deprecated 转发 (`cancelAsyncJobs` / `runningAsyncJobs` / `startAsyncSubagentTask` /
  `AsyncSubagentJob` / `maxConcurrentSubagents` / `maxDepth` 一次替换, 含注释与日志措辞);
- **行为变化 (写进发布说明)**:
    1. 模型委派**默认不再阻塞轮次** (只要 `async_work.enable=true`), 结果由事件返回;
    2. 删除进程级并发上限与嵌套深度上限; 只留每队列额度 3;
    3. 事件名固定为 `agentxx.agent.async.done` (旧 `...event.subagent.done.<id>` 不再生成), 定向等待改用 `id` 过滤;
    4. 异步任务的 `sessionId` 就是 `async_id` (旧 `subagent_<名>_<agentId>_<n>` 不再生成);
    5. HIL 新增 `deny` / `allow` (默认仍 `bubble`);
- **不受影响**: 上下文压缩等内部直调 `spawnBatch` 的路径仍同步等结果 (没有 `tool_call_id`)。

## 8. 实施计划

| 期 | 内容 | 验收 |
|---|---|---|
| **P0** 抽取与改名 | 新增 `async_work.{h,cpp}`, 搬移异步派发并改名; 删除并发上限与深度; `sessionId = asyncId` | 全量测试全绿 (除行为变化项); `grep` 不到 `asyncJobs_` / `AsyncSubagentJob` / `maxConcurrentSubagents` / `sessionDepth_` |
| **P1** 队列与额度 | 每队列 FIFO + 额度 3 (不设排队/深度上限); 台账 + `linger` 清扫; 指标/快照/配置块 | 同一会话提交 10 个只跑 3 个、FIFO 补位; 两个会话互不阻塞; 无限排队不失败 |
| **P2** 自包含与平等 | 显式挂载清单; 创建者已销毁路径; `notify_delivery`; 会话落盘 `{创建者会话目录}/agent/{async_id}`; HIL 三策略 (chatbot 默认 allow) | "提交后立刻销毁创建者"能跑完并留台账; 传 `plugins` 的 agent 能调文件工具; 开 store 后有库; `deny`/`allow` 生效 |
| **P3** 可见与可控 | 事件名单一化 + hub 值过滤 `match`; `agentxx_subagent` 去 `async` 参数; `agentxx_event opt=works/status/await/cancel`; runtime 表 4 成员 + kit | 模型能查/等/取消; 插件能提交/取消/查询; `boundaries` / `plugin_runtime` 通过 |
| **P4** 界面与下游 | `WireContextStats.asyncWorks` + TUI Info 段; chatbot 接入 (§11); 诊断文案; 文档 | `tui_surface` / `wire_roundtrip` 通过; `plugin_example_chatbot` 全流程通过 |

P0 纯重构 (除删上限/深度), P1 有队列, P2 才是"平等 + 可干活", P3/P4 是可见与可控。
每期可独立回滚 (P1 起开关是 `async_work.enable`)。

## 9. 测试计划

| 模块 (新增/扩展) | 覆盖 |
|---|---|
| `async_work` (新, 同步组) | 队列 FIFO 与补位; 每队列额度 3 且互不影响; 不设排队上限 (提交 200 个不失败); 台账 `linger` 清扫; `sessionId == asyncId`; 台账 JSON 形状 |
| `event_subagent` (扩展) | 改名后语义不变; 提交即异步 (工具立即返回 `async_id`); 取消整批; **创建者销毁后任务跑完并留台账**; 内部直调仍同步 |
| `event_tools` (扩展) | `opt=works` 的 `scope/state/limit/id`; `opt=await id=` / `opt=status id=` / `opt=cancel id=`/`scope=mine`; 未启用时的降级文本 |
| `event_wait_match` (新, 可并入 `event_tools`) | `match={asyncId}` 只收自己那条; 不匹配的值不消耗等待; `replay` 语义 |
| `async_work_store` (新, 可并入第一项) | `session_store=true` 时 `{创建者会话目录}/agent/{async_id}/session.db` 存在且可读; 同一创建者多个任务共存; 关闭时不落盘 |
| `async_work_hil` (新, 可并入第一项) | `bubble` (创建者活着/已死) / `deny` / `allow`; `interrupt_timeout` 到点取消 |
| `plugin_agent_runtime` (扩展) | 表成员: 提交/回调顺序/取消/查询/归属回收; 老主程序降级 |
| `boundaries` / `plugin_runtime` (扩展) | 表四件套与成员顺序台账; 三态判定与偏移 |
| `wire_schema` / `wire_roundtrip` (扩展) | `asyncWorks` 字段 schema 与往返 (空数组不写) |
| `tui_surface` (扩展) | Info「后台任务」段: 有运行项 / 仅排队 / 全空 / 超长截断 |
| `config_keys` (扩展) | 7 个新键解析与默认值; 生成物更新 |
| `plugin_example_chatbot` (扩展) | 提交 → 排队 → 跑完 → 结果事件 → 会话被唤醒 → 发消息; 取消 |
| `async_work_bench` (可选, benchmark) | 3 并发下的 RSS/连接数/CPU (§10.2) |

## 10. 风险与备选方案

### 10.1 为什么不选"常驻通道池"

| 维度 | 常驻通道池 (K 条通道当工人) | 每任务一个自销毁 agent (本方案) |
|---|---|---|
| 隔离 | 靠约定 (同一会话上下文跨任务累积) | 天然隔离 (独立 `AgentContext`) |
| 内存 | 通道关闭后 `Session` 仍留在根的 `SessionsManager` 里 (无淘汰接口) | 跑完 `destroyAgentAsync` 整体释放 (现状已如此) |
| 并发控制 | 插件自己数 workers | 核心队列统一 (模型/插件/未来异步工具共用) |
| 工具/配置 | 用根的配置, 不能按任务变 | 每任务显式清单 |
| 代价 | 省一次 init | 每任务一次 `BaseAgent::init` (含 provider 建立) |

### 10.2 provider 与连接池膨胀

每个任务的 agent 在 `init` 里新建自己的 `ModelProviderRegistry` (含 HTTP 连接池)。3 并发 ≈ 3 套连接,
加上根会话与压缩子代理, 峰值连接数约为 `(根 + 压缩 + 后台任务数) × 每模型 keep-alive 数`。
缓解: 队列额度默认 3; benchmark 记录连接数与 RSS; 彻底解决要单独做"provider registry 下沉"课题
(需先确认 provider 并发安全), 不在本期。

### 10.3 没有全局上限/深度上限/排队上限的后果

按评审要求一律不设限, 需要清醒记录后果:

| 风险 | 说明 | 谁负责 |
|---|---|---|
| 递归派生 | 后台任务的 agent 若挂了委派工具 (`tools: ["*"]`), 它可以再提交后台任务, 核心不再拦 | 提交方: 决定是否把委派工具给后台任务 (chatbot 的后台任务用 `tools` 白名单或不含 `agentxx_subagent`) |
| 总量膨胀 | 会话多、每个额度 3、又都不设全局上限 → 同时运行的 agent 数 = 3 × 活跃队列数; 每个都自带 provider (§10.2) | 运维/配置: 需要时调小 `max_concurrent` 或关闭部分 agent 的 `async_work.enable` |
| 排队无上限 | 恶意/失控的提交方可以无限排队 → 内存随提交量增长 (每条 spec + 台账) | 插件侧自控 (chatbot 有业务队列, 不会无限投) |
| 成本 | 没有闸门意味着"模型想派多少就派多少" | 提示词与工具描述里写明适用场景; 需要时再加只读统计 |

只提供**观察手段** (计数/台账/Info 段), 不加任何拒绝或告警 (§3.12)。

### 10.4 容器 io 线程争用

K 个后台任务的 token 流与工具调度都在容器 io 线程上交错执行 (与多会话现状一致)。
工具卸载到工作线程池, 默认额度 3 时可控; benchmark 里看 TUI 帧耗时与轮次延迟。

### 10.5 队列内公平

队列之间互不影响; 同一队列内严格 FIFO, 长任务会压住后面的短任务。需要时后续在 spec 尾部追加 `priority`。

### 10.6 事件名单一化后的等待语义

单名 + `id` 过滤依赖 hub 新增的 `match`。要保证:

- 不匹配的值**不消耗**等待 (否则别人的任务完成会把我的 `first` 等待吃掉) —— 实现上在匹配失败时跳过该值;
- `replay` 时同样按 `match` 过滤历史值;
- 事件名族扩展 (`agentxx.agent.async.*`) 只加这一族, 其他规则不动; `async.` 段归框架保留。

### 10.7 语义变化对既有使用方的影响

- 模型委派默认异步: 依赖"委派期间阻塞"的提示词/测试要改 (改用 `opt=await id=`);
- 内部直调 (`summarization` 等) 仍同步: 以"是否有 `tool_call_id`"区分, 无需新参数;
- `agentxx_event` 的 `opt` 与参数变化: 提示词、生成物、测试同步更新。

### 10.8 备选方案 (未采用)

| 备选 | 为什么不用 |
|---|---|
| 保留 `async` 参数 (每个工具自己决定同步/异步) | 同一个能力两套用法, 模型容易误用; 用配置统一 |
| 每任务一个事件名 (现状) | 概念多、事件表膨胀; 单名 + `id` 过滤更省, 但需要 hub 支持值过滤 |
| 只加插件表入口, 不做内置模块 | 普通会话 (模型侧) 同样需要, 做成模块才不重复 |
| 队列放在客户端 (TUI/FFI) | 后台任务必须与 agent 同生命周期, 客户端断开就没了 |

## 11. chatbot 接入

### 11.1 配置形态

插件根的 `config_json` 增加 `asyncWork` 段 (落地点: `plugin_manager_runtime_config.cpp`):

```jsonc
// chatbot 人设根 config_json 追加 (camelCase 与 yaml 的 async_work.* 一一对应)
"asyncWork": {
  "enable": true,
  "maxConcurrent": 3,        // 每个聊天人设最多 3 个后台任务同时跑 (只看自己这条队列)
  "interruptPolicy": "allow",// 后台任务在群里不弹权限窗 (按评审意见的默认)
  "sessionStore": false,     // 先不落盘
  "lingerMs": 1800000        // 半小时内可用 chatbot_task status 查到
}
```

### 11.2 提交时传什么 (显式清单)

| 字段 | chatbot 传什么 |
|---|---|
| `kind` | `agent` |
| `plugins_json` | 该人设**现有的插件挂载列表**, 去掉 `agentxx_chatbot` 自身 (后台任务没有聊天会话, `chatbot_*` 工具在那里无法定位平台对象, 结果由插件自己发) |
| `mcp_servers_json` / `skills_json` / `memory_files_json` / `rag_docs_json` | 人设根现有的同名配置 (与主会话完全一致) |
| `work_dir` / `language` / `permission_allow_json` / `permission_deny_json` | 留空 = 跟随人设根 (推荐) |
| `tools_json` | `["*"]` 或按人设白名单 (**不含** `agentxx_subagent`, 避免后台任务再派任务) |
| `notify_delivery` | `"next-step"` (聊天会话被唤醒, 由人设组织语言) |
| `interrupt_policy` | `allow` (见配置) |

### 11.3 chatbot 侧改动

| 部件 | 内容 |
|---|---|
| `chatbot_task.h/.cpp` (新) | 任务队列 (业务层: 优先级/去重/取消/面板快照); 提交走 `submitAsyncWork`; 完成靠 `on_event` → post 到插件 io 线程 |
| `chatbot_tools` | 新增工具 `chatbot_task` (`submit` / `status` / `cancel` / `list`) |
| 命令 | `/agentxx-cmd task <submit|status|cancel|list>` (`CommandMode::Action` 分发) |
| 结果通知 | ① `notify_delivery` 唤醒聊天会话; ② 会话忙/被关时插件直接用 `outbound` 发 (被动→主动降级已有) |
| 面板 | 「机器人」栏加一段: 本会话后台任务 (运行/排队/最近完成) |
| 不阻塞 | 提交即返回; 任务跑在容器 io 线程, 不占插件 io 线程 |
| 与 TUI Info 段的关系 | 两处数据源不同: TUI Info 段看全部任务 (核心推送), chatbot 面板看业务字段 (自己的队列) |

**实施后的修正 (2026-11-05, 已完成)**: 结果通知的第 ① 不够 —— 插件提交的任务挂在
**内置根**的中枢上 (`runtimeSubmitAsyncWork` 用 `primaryAgent_` 的上下文), 而聊天会话属于
**人设根**, `notify_delivery` 投递会因 "session not found" 被丢掉。实际实现在 `on_event`
里由插件自己投: 通道在就把结果写成一条 `next-turn` 输入, 不在就用 `outbound` 直发。
另外提交快照的 `plugins` 必须支持 `exclude` (字符串数组) 才能做到"按人设清单挂、但去掉自己"
—— `all` 模式下用 `list` 表达不了。

### 11.4 与现有 `ChatbotRouter` 的关系

- `ChatbotRouter` 管"聊天会话的通道池 (一聊一条通道)";
- `chatbot_task` 管"后台任务队列";
- 两者共享 `outbound` 与 `chatKey → sessionId` 映射, 但**不共享通道**: 后台任务没有聊天通道, 靠结果事件回到聊天会话。

## 12. 待确认问题

1. **值过滤的形态**: hub 的 `match` 用"顶层字段相等"的通用 JSON 对象 (`{"asyncId":"async-7"}`),
   还是只做专用的 `asyncId` 过滤? 我建议通用 + 只暴露 `id` 参数给模型。
2. **`async_work.enable=false` 的工具行为**: 退化为同步执行 (本文), 还是干脆不注入委派工具?
   我建议"同步执行", 这样关掉开关不改变可用性, 只改变阻塞性。
3. **chatbot 的 `allow` 范围**: 全允许 (含命令执行), 还是"文件读允许、写/命令仍询问 (bubble)"?
   逐条声明会需要一个 `interrupt_policy` 的细分 (按工具类别), 本期先整体 `allow` 可以吗?
4. **会话目录的前置条件**: `{创建者会话目录}` 在创建者**没有**会话存储时不存在, 此时
   ① 回退到 `{dataDir}/sqlite/sessions/agent/{async_id}`, 还是 ② 直接拒绝 `session_store=true`? 我建议 ①。
5. **台账保留**: 结束后保留 5 分钟 (`linger_ms`) 作为纯内存回收, 可以吗? 还是"一直留到进程退出"?
6. **插件表成员数**: 4 个 (`submit` / `cancel` / `get` / `list`) 是否合并成 3 个 (`get` 并入 `list(id 可空)`)? 人工核对可行
7. **`kind=tool` 的实现时间**: 本期只做 `kind=agent`、结构预留, 确认?
   (插件的后台工具走同一条通道); 人工核对: 先做 agent, 然后做 tool, 异步启动的参数名称要统一为 `async:bool`
8. **provider 共享**: 本期不做 (§10.2) 确认? 共享 provider/HTTP 连接池需要做，这个应该迁移到 AgentHost 提供，所有 AgentNode 共享, 以便 agent、tool 都共享使用

## 13. 文档更新清单

| 文档 | 更新内容 |
|---|---|
| `docs/zh-cn/design/index.md` | "事件系统" / "多会话与并发" 补"后台异步任务 (async work)"; 架构图加 `AsyncWorkManager` |
| `docs/zh-cn/design/events.md` | 异步派发改名异步任务; 单一事件名 + `match` 过滤; 排队/取消/查询; 值形状 |
| `docs/zh-cn/design/plugins.md` | §19 表成员与 SDK 便捷层; §20 选型表 (触发器 vs 异步任务) |
| `docs/zh-cn/design/tui.md` | §2.7 Info 段新增「后台任务」 |
| `docs/zh-cn/design/configuration.md` + `config-keys.md` | `async_work.*` 与插件根 `asyncWork` 段; 生成物重跑 |
| `docs/zh-cn/design/wire-protocol-fields.md` | `WireContextStats.asyncWorks` (生成物重跑) |
| `docs/zh-cn/design/roadmap.md` | 新增一条状态 |
| `resource/history/chatbot/plan.md` | 新增"后台任务队列"一节 (本文 §11 的收敛版) |
| `AGENTS.md` | 事件中枢/异步段落补 async work 要点 (开关、队列额度、单一事件名、`opt=works`) |

