# 实施状态: 内置异步任务模块 (AsyncWork)

方案见 [plan.md](./plan.md)。本文件记录实施进度、已完成/待完成内容与注意事项。

## 总览

| 期 | 内容 | 状态 |
|---|---|---|
| P0 | 抽取 `AsyncWorkManager` + 改名 + 删除全局并发上限/嵌套深度 | 已完成 |
| P1 | 每队列 FIFO + 额度 + 台账/linger + 指标/快照/配置块 + 测试模块 | 已完成 |
| P2 | 提交自包含 (显式清单) + 定向投递; 会话落盘 / HIL 三策略 / 创建者已销毁用例 | 已完成 |
| P3 | 单一事件名 + hub 值过滤 `match` + 工具改造 + 插件表 4 成员 | 已完成 |
| P4 | 文档 + 界面; chatbot 接入 | 已完成 (文档 + wire/TUI + chatbot 接入) |
| P5 | 模型注册表下沉 `AgentHost` (所有 AgentNode 共享 provider) | 已完成 |
| P6 | `kind=tool` 异步工具 (模型侧 `agentxx_tool_async` + 插件面追加字段) | 已完成 |

提交记录: `2dca9dc5` (P0) → `164029e0` (P1) → `6ea5a5aa` (P3) → `04ea4a57` (P2) →
`0a40ed4b` (P4 文档) → Info 侧边栏「后台任务」段 + `WireContextStats.asyncWorks`
("async_work: Info 侧边栏「后台任务」段 + WireContextStats.asyncWorks") → HIL 三策略 +
会话落盘 + 创建者已销毁用例 → `session_store_keep_days` 清理 (90f46e94) →
插件表 runtime v2 四成员 (b1ea340d) → 根配置 `asyncWork` 段 (8f5a8971) → P4 chatbot 接入
(`chatbot_task` + 命令 + 面板段 + 宿主 `exclude`; 90d50c79) → P5 模型注册表下沉
(c9515795) → P6 `kind=tool` 异步工具。

## 已完成

### P0 抽取与改名

- 新增 `agent/lib/include/agentxx/agent/async_work.h` + `agent/lib/src/agent/async_work.cpp`:
  `AsyncWorkKind` / `AsyncWorkState` / `AsyncInterruptPolicy` / `AsyncWorkSpec` /
  `AsyncWorkEntry` / `AsyncWorkSubmitResult` / `AsyncWorkManager`
  (台账 + 每队列 FIFO + 额度 + linger 清扫 + 取消/查询 + 指标记账)。
- 从 `agent_host.cpp` 搬走异步派发三段 (`spawnBatch` 的异步分支 / `startAsyncSubagentTask` /
  `runAsyncSubagentTask`), `spawnBatch` 改为构造 `AsyncWorkSpec` 后 `submitAsyncWork`。
- 改名到位: `AsyncSubagentJob`→`AsyncWorkEntry`、`asyncJobs_`→`asyncWork_`、
  `job-<n>`→`async-<n>`、`runningAsyncJobs()`→`runningAsyncWork()`、
  `cancelAsyncJobs*`→`cancelAsyncWorks*`、日志前缀 `[subagent] async job`→`[async_work]`。
- **删除**: `AgentHost::Config::maxDepth` / `maxConcurrentSubagents`、`sessionDepth_`、
  `runningSubagentCount_`、深度/并发拒绝分支、`eventNameSuffix` 与同名事件冲突检查。
  `AgentNode::depth` 保留为展示字段, 容器不再限制嵌套。
- 异步任务的 sessionId 直接用 `asyncId` (不再生成 `subagent_<名>_<agentId>_<n>`)。
- `spawnOneTask` 增加可选 `const AsyncWorkSpec*` 参数: 派生基准配置优先取快照里的
  `baseConfig` (创建者已销毁也能派生); 子代理 `AgentContext::agentId` 现在会被填上,
  `AgentNode::sessionId` 记任务会话 id。
- 事件名单一化 (`agentxx/event/naming.h`): 新增值事件族 `agentxx.agent.async.`
  (`kAgentAsyncPrefix`), `isEventName` / `validateEventName` 接受它; 结果事件固定
  `agentxx.agent.async.done`。`SubagentBatchItem::eventName` 保留为内部传输字段。
- 结果值形状: `{asyncId, kind, name, content, hasError, canceled, errorMessage,
  creatorSessionId, startedAtMs, finishedAtMs}`。
- `config.h` 新增 7 个 `async_work.*` 字段 + `config_loader` yaml 解析 + `client/main.cpp`
  接线; 配置键目录重新生成 (62 → 69 键)。

### P1 指标 / 快照 / 测试模块

- `util/observability.{h,cpp}`: `noteAsyncWorkSubmitted(size_t queuedNow)` /
  `noteAsyncWorkFinished(bool hasError, bool canceled, int64_t runMs)`, 计数进 `KeyMetrics`
  的 `async_work` 段与 summary 文案, `reset` 一并清零。
- `assembly_snapshot.cpp`: 加载快照新增 `root["async_work"]` 段与对应文字行。
- 新测试模块 `agent/test/core/test_async_work.cpp` (+ 头 `agentxx-test/core/test_async_work.h`,
  在 `test.cpp` 注册): 7 个用例 44 个断言, 覆盖队列 FIFO、额度、无限排队、取消、
  linger 清扫、`queueAhead`、`kind=tool` 拒绝。

### P2 提交快照 (已完成部分)

- `agent_host.cpp` 新增 `applyAsyncWorkOverrides()`: 按 spec 覆盖 `model` / `workDir` /
  `language` / `plugins`(mode + list + args) / `mcpServers`(`ns=url` 或纯 url 推导 ns) /
  `skills` / `memoryFiles` / `ragDocs` / `permissionAllow|Deny`, 派生不再依赖创建者存活。
- `async_work.cpp` 的 `submit`: `notify_delivery` 非空时经 `hub->callSync` 登记一条
  `match={"asyncId":…}`、`replay=false` 的定向等待, 结果直接落到指定会话。
- `test_agent_host.cpp` 嵌套委派用例显式 `cfg->enableAsyncWork = false` (开着时委派走后台,
  断言次数会从 3 次请求降到 2 次)。
- `docs/zh-cn/design/test-modules.md` 重新生成。

### P3 模型可见可调 (已完成部分)

- `event/hub.h`: `EventWaitSpec::match` (顶层字段相等过滤); `hub.cpp` 的
  `waitMatchesSpec()`: 不匹配的值不消耗等待, 带过滤的等待不随事件结束收尾, `watch` 回放
  也按过滤挑选。
- `tools/subagent.cpp`: 删除 `async` / `event_name` 工具参数与 schema 项,
  `asyncMode = agentConfig->enableAsyncWork && !tool_call_id.empty()`; 中断载荷里写内部标记
  `t["async"]=true`; `prompt.cpp` 同步改掉该工具的说明与参数表。
- `events.h`: `RespSubagentBatchItem` 新增 `asyncId`; `SubagentBatchItem::async` /
  `eventName` 注释改为"内部传输字段"。
- `tools/subagent.h` 的 `renderSubagentStartedText`: `[Async work started] id=... event=...`,
  并给出 watch/await/cancel/works 的写法。
- `tools/event.cpp`: `opt` 增加 `works` (共 8 个), 新增 `id` 参数; `buildWaitSpec` 支持
  `id` (自动事件名 + match); 实现 `opt=works` / `status id=` / `cancel id=|scope=mine`,
  新增 `renderAsyncWorks()` 与 `formatElapsed()`; `async_work.cpp` 的 `listJson` 给排队项补
  `queueAhead`。
- `base_agent.cpp`: `agentxx_event` 的注册条件放宽为"有中枢 或 `enableAsyncWork`"。
- 测试: `event_tools` 的 opt 枚举断言 7 → 8; 新增定向等待用例 (无关值不消耗等待、
  无容器时 `works` 报 `[threw]` + 提示 "agent host")。

### P3 插件表四成员 (已完成)

- `plugin_api.h`: `AgentxxPluginAsyncWorkSpec` (含 `on_event` / `user_data`, 无 `event_name`)
  + `AGENTXX_PLUGIN_ASYNC_WORK_EVT_*` 四个阶段 + 表尾追加
  `submit_async_work`/`cancel_async_work`/`get_async_work`/`list_async_work`;
  `_VERSION` 1→2, `_MIN_VERSION` 保持 1, `_BASELINE_SIZE` 改为
  `offsetof(AgentxxPluginRuntimeIface, submit_async_work)` (基线停在追加前)。
- 宿主: `PluginManager::runtimeSubmitAsyncWork` (校验 → 派生 `AsyncWorkSpec` →
  `host->submitAsyncWork`, 归属 `plugin:<实例名>`) / `runtimeCancelAsyncWork` /
  `runtimeGetAsyncWork` / `runtimeListAsyncWork`; `releaseRuntimeOfOwner` 先按 owner
  取消未完成任务再卸根。四个 vtable 包装走 `onInstanceIo` / `queryStringIo` 骨架。
- 回调生命周期: 包一层 `weak_ptr<PluginInstance> + enabled` 检查, 实例释放/禁用后不再
  进入插件代码。
- `AsyncWorkSpec` 新增 `owner` 与 `onEvent` (+ `AsyncWorkEventPhase`); 管理器在提交/起跑/
  终态 (含排队中取消) 各触发一次, 异常只记日志; `entryJson`/`listJson` 补 `owner`,
  `AgentHost::cancelAsyncWorksOfOwner` 新增。
- SDK: `AsyncWorkOptions` + `submitAsyncWork` / `cancelAsyncWork` /
  `getAsyncWorkJson` / `listAsyncWorkJson` (老主程序按字段空判降级)。
- `boundaries`: 基线允许 `offsetof(表, 成员)` 形态; 成员顺序台账补上四个新成员。
- 测试: `plugin_agent_runtime` 177 → **199 断言** (四成员可空、kind=tool 拒绝、提交→终态、
  `get`/`list` 看到 owner 与结果、回调阶段、别的实例看不到也取消不了、自己的已结束后取消返回
  `ERR_STATE`)。
- 文档: `plugins.md` §19.2/接口表总表、`AGENTS.md` 已同步。

### P4 界面与文档 (已完成部分)

- 线消息: `WireContextStats` 追加 `asyncWorks` (新结构 `WireAsyncWorkItem`:
  `{id, kind, name, state, elapsedMs, queueAhead}`); `makeContextStats` 空数组不发,
  `contextStatsFromJson` 按默认空数组兼容老服务端; 生成物 `wire-schema.json` /
  `wire-protocol-fields.md` 重新生成 (`AGENTXX_UPDATE_WIRE_SCHEMA=1`)。
- 服务端: `SessionServerAgentIO::sendContextStats` 向 `AgentHost` 取本会话创建的
  运行/排队/最近完成项 (最多 10 条 `kAsyncWorksInStats`), 已跑时长由时间戳当场算出。
- 客户端: `TUIRenderState.asyncWorks` + `onContextStats` 覆盖式同步; Info 侧边栏新增
  「后台任务」段 (Context 之后、Append 之前): 头部 `运行 N / 排队 M`, 逐条
  `标记 id 名字 时长|排队位`; 文案键 `info.asyncWorks` / `info.asyncWorksEmpty` /
  `info.asyncWorkSummary` / `info.asyncWorkAhead`。
- 测试: `wire_roundtrip` (字段往返 + 空数组兼容)、`tui_widget` 新增
  `test_info_sidebar_async_works` (4 种形态)、`wire_schema` 新鲜度。
- 文档: `events.md` 第 8 节 / `tui.md` §2.7 / `AGENTS.md` 均已同步 (下面「文档」一段)。

### P4 chatbot 接入 (已完成)

- 宿主: `AgentConfig::pluginMountExclude` (运行时注入, 不是 yaml 配置项) +
  `mountAllAsync` 在 `All` / `Listed` 两种策略下都跳过剔除名单;
  提交快照 `plugins` 新增 `exclude` 数组 (`applyAsyncWorkOverrides`), SDK 注释同步。
- `agent/plugins/agentxx_chatbot/chatbot_task.{h,cpp}` (新, 纯逻辑, 编进测试):
  `ChatbotTaskManager` 管业务台账 —— 提交 (每会话上限 4 条、正文压缩成一行留档)、
  事件状态机 (`queued`/`running`/`done`/`failed`/`canceled`、终态回查台账取结果摘要)、
  取消 (单条 / 本会话全部 / 跨会话拒绝)、文本渲染 (`listText`/`statusText`/`resultNote`/
  `finishedText`/`formatTaskElapsed`)、过期与条数回收。
- `chatbot_tools`: 新增 `chatbot_task` 工具 (`submit`/`status`/`cancel`/`list`);
  `ChatbotToolDeps` 多一个 `tasks`。
- `chatbot_engine`: 新增 `deliverTaskResult` (把结果作为一条 `next-turn` 输入投回会话)
  与 `Runtime::taskCommand` (`/agentxx-cmd task ...` 的实现入口)。
- `chatbot_core`: 内置命令表加 `/agentxx-cmd task` (action = `task`); `validateConfig`
  的 action 白名单同步。
- 插件入口: `ChatbotCtx::tasks` + `makeTaskRuntime` (提交快照: 人设白名单 / 人设插件清单
  去掉自己 + `exclude` / 排队键 `chatbot:<人设名>` / `interruptPolicy = allow` /
  `sessionId` = 聊天会话 id)、`onChatbotAsyncWorkEvent` (容器 io 线程 → post 回插件 io
  线程)、`handleTaskEvent` (记账 → 终态投递 → 刷面板)、`sendTaskResultDirectly`
  (通道已回收时用 outbound 直发)。
- 面板: `PanelSnapshot.tasks` (`PanelTaskView`: id/标题/状态/会话/排队位/耗时) +
  「后台任务」段 (最多 6 条, 运行中带时长与加载帧); 空数组不写进 JSON (老客户端行为不变)。
- 测试: 新模块 `chatbot_task` (138 断言, 纯逻辑); `chatbot_core` 内置命令表断言更新;
  `plugin_example_chatbot` 新增 13b 段 (工具清单 7 个 + `chatbot_task` 提交/清单/详情/取消 +
  两条命令回复, 全流程 236 断言全绿)。
- 文档: 插件 `README.md` §2.4/§4.1、`docs/zh-cn/design/plugins.md` §19.2 (`exclude` 与
  "结果怎么回到创建者")、`AGENTS.md`。
- **与计划的偏差** (实现后确认): 没有用 `notify_delivery` 唤醒聊天会话 —— 插件提交的
  任务挂在**内置根**的中枢上 (`runtimeSubmitAsyncWork` 用 `primaryAgent_` 的上下文),
  而聊天会话属于**人设根**, 中枢投递会因 "session not found" 被丢掉。改成插件自己在
  `on_event` 里投递 (会话在就 `next-turn` 输入, 不在就用 `outbound` 直发)。
- **已知限制** (本期不做): 人设的 MCP / Skill / Memory / RAG 没有逐人设配置, 所以后台
  任务拿不到这些 (子代理配置派生时会清空, 插件也没地方读); 中断策略固定 `allow`,
  没做人设级配置项; 任务结果不进 TUI Info 段 (那里是主程序台账, 自然会包含)。

### P4 根配置 asyncWork 段 (已完成)

- `plugin_manager_runtime_config.cpp`: 根 `config_json` 支持 `asyncWork` 段
  (`enable` / `maxConcurrent` / `interruptPolicy` / `sessionStore` / `lingerMs` /
  `resultMaxBytes` / `sessionStoreKeepDays`), 非法中断策略退回 `bubble`。
- 测试: `plugin_agent_runtime` 199 → **209 断言** (字段落值 + 非法策略回退)。
- 文档: `plugins.md` §19.3 根配置表补一行。 (已完成部分)

- `docs/zh-cn/design/events.md`: 第 8 节整节改写为「后台任务 (async work)」(委派即提交、
  单一事件名 + `match`、结果值形状、每队列额度、`asyncId` = sessionId、取消入口、
  无中枢只落台账、linger), 第 6 节 `opt` 表补 `works` 与 `id`, 第 7 节内置事件源换成
  `agentxx.agent.async.done`, 第 10/13/14 节与第 1 节的命名说明同步更新。
- `docs/zh-cn/design/index.md`: 「事件中枢与后台任务」小节改写, 补 `AsyncWorkManager`
  的队列/额度与"不设全局/深度上限"。
- `docs/zh-cn/design/plugins.md`: §19.2 入口表加一行"(待做) 后台任务 4 成员"; §21.2 值事件段
  改为 `agentxx.agent.async.done` 例外说明; 接口表总表同步。
- `docs/zh-cn/design/configuration.md`: §5.1 归属表补 `event.*` / `async_work.*` (都归服务端)。
- `docs/zh-cn/design/roadmap.md`: 修订记录增加 2026-11-05 一条 (JOB-1~12 的后台任务部分实施)。
- `docs/en/design/index.md`: 修掉两处"enforces depth and concurrency budgets" (已不再成立)。
- `AGENTS.md`: 新增「后台任务 (async work)」要点段 (开关、每队列额度、单一事件名 + match、
  `opt=works`、`asyncId` = sessionId、无全局/深度上限、未做项清单), 事件中枢段与
  "相关文档"表述同步。

### P5 模型注册表下沉 AgentHost (已完成)

勘察结论 (先纠正一个前提): **HTTP keep-alive 连接池本来就是进程级单例**
(`utilxx::HttpClient` 的连接池是静态的), 所以连接数并没有因为 agent 多而乘倍。
真正重复的是**每个 agent 一份 `ModelProviderRegistry`**:

- `BaseAgent::initModelRegistry` 每次都新建一份 registry + provider 缓存; 子代理/插件根/后台
  任务每个都来一遍 —— 同一模型 N 份 provider 实例 (每份一个 `ModelConfig` 拷贝);
- 运行时 `AddModel` / `RemoveModel` 只改**内置根**那一份: 先挂好的人设根看不到新模型。

实现:

- `AgentHost` 新增 `modelRegistry_` + 公开 `modelRegistry()`; 三处注入共享:
  `create()` 提前给主 agent、`createAgent()` 创建后立即给 (调用方往往先 `init` 后 `mountRoot`,
  晚注入等于白建一份缓存)、`injectShared()` 与子代理派生处;
- `BaseAgent::initModelRegistry` 改为"已经在就复用, 只补表里没有的模型": 同名不覆盖
  (否则每来一个 agent 都会清掉共享表里缓存的 provider), 默认模型只在"建表那一次"按
  `currentModelName` 定, 后来者不抢;
- 晚注入兜底: `injectShared` 发现 agent 已经自建过 registry 时, 先把它认识的模型并进
  共享表 (不覆盖) 再换指针;
- 测试: `agent_host` 新增 `test_host_shared_model_registry` (173 断言): 同一份指针、
  两个 agent 拿到的 `getProvider("a")` 是同一个实例、默认模型由先 init 的定、
  运行时增删模型两边同时可见、无容器的独立 `BaseAgent` 仍自建一份。

偏差: 计划里写的是"provider/HTTP 连接池下沉", 实际只做了模型注册表 (连接池已共享)。

### P6 `kind=tool` 异步工具 (已完成)

- `AsyncWorkSpec`: 新增 `toolName` / `toolArgs` (只在 `kind == Tool` 时使用)。
- `AsyncWorkManager::submit`: `kind=tool` 要求 `toolName` 非空 + 有创建者上下文
  (工具表在创建者身上); `message` 只有 `kind=agent` 才必需。
- `async_work.cpp` 新增 `runToolWork(creator, spec, token)`:
  - 在创建者工具表里查 `spec.toolName`, 找不到立即失败收尾;
  - 参数拷一份进协程帧, 并补 `sessionId` / `tool_call_id` (与前台 `ToolcallWrapNode`
    一致, 下游工具会读);
  - `asio::co_spawn` 执行 `tool->execute_async(args)`, 状态放 `shared_ptr<RunState>`
    (不会因为外层提前退出而悬挂), 异常经 `catchErrorAsync` 转成失败结果;
  - 每 50 ms 看一次取消令牌 (取消是尽力而为: 工具不认识取消时会继续跑到自己的超时,
    结果被丢弃);
  - `ToolStatus` → `RespSubagentBatchItem` 的映射 (`Ok` 进 `content`, `Canceled` 置
    `Canceled`, `Interrupted`/`Failed` 置 `hasError`);
  - 结果落台台账 (并把 `kind` 记成 `tool`), 事件仍是 `agentxx.agent.async.done`。
- 新工具 `agentxx_tool_async` (`agent/lib/{include/agentxx/tools/tool_async.h,src/tools/tool_async.cpp}`):
  参数 `tool` / `args` / `label`; 校验工名存在、不允许套自己 (不嵌套); **提交前按目标工具
  与参数问一次权限** (`Topic::ToolPermissionCheck`, 与前台同一套权限中间件) —— 否则只要能
  调本工具就能绕过目标工具自己的限制; 返回 `[Async tool started] id=...` 与取值/取消写法。
  只在 `async_work.enable` 且上下文有容器时注册 (`base_agent.cpp`), 提示词在
  `prompt.cpp` 的 `toolPrompt` 表内。
- 插件面: `AgentxxPluginAsyncWorkSpec` 尾部追加 `tool_name` / `tool_args_json`
  (基线改为 `offsetof(tool_name)`, `plugin_spec.h` 里登记该特化); vtable 用 `specHas` 读;
  `runtimeSubmitAsyncWork` 只在 `kind=tool` 且工名为空时拒 (`ERR_ARG`);
  SDK `AsyncWorkOptions` 加 `toolName` / `toolArgs` (`toolName` 非空即按 tool 提交)。
- 测试: `async_work` 7 → **10 个用例 / 121 断言** (`test_tool_kind_runs` 用测试工具验证
  真的执行 + `sessionId` 注入 + 台账 `done`/`result`; `test_tool_kind_failures` 覆盖缺工名 /
  缺创建者 / 工具不存在 / 工具返回失败; `test_tool_kind_cancel` 覆盖运行中取消);
  `plugin_agent_runtime` 209 → **214 断言** (旧布局缺 `tool_name` 按 `ERR_ARG` 拒、
  `tool_args_json` 不是对象被拒、kind=tool 受理后按 failed 收尾)。
- 文档: `events.md` §8 (kind=tool 段) / §13 (未做项)、`plugins.md` §19.2、`AGENTS.md` 同步。
- 偏差: 计划里 `kind=tool` 没提权限与 `sessionId` 注入, 实现时补上了 —— 不补会分别变成
  "绕过目标工具限制" 与 "半个内置工具在后台直接报错"。

## 待完成
- 可选后续 (chatbot 侧): 人设级 MCP/Skill/Memory 与后台任务的中断策略配置项;
  后台任务的会话落盘开关 (`asyncWork.sessionStore` 已支持, 插件目前传 `-1` 跟随配置)。
- 可选后续 (后台任务): `execute_command` 的 `background:true` (现在可用 `agentxx_tool_async`
  把那次命令调用丢到后台, 但不如命令层自己支持流式回显)。
- `kind=tool` 的中断处理: 它没有自己的会话/agent, HIL 与权限只在**提交时**按目标工具
  判定一次; 后台执行期间不再问人 (有意为之, 避免无界面时挂死)。

## 注意事项

- plan.md §12 的人工核对结论:
    - `kind=tool` 后做 (先做 agent); 异步启动参数名统一为 `async:bool`;
    - provider/HTTP 连接池共享要做: 迁移到 `AgentHost` 提供, 所有 `AgentNode` 共享,
      agent 与 tool 都使用同一份。
- 行为变化 (写进发布说明):
    1. 删除进程级并发上限与嵌套深度上限, 只保留每队列额度 (`async_work.max_concurrent`);
    2. `agentxx_subagent` 不再有 `async` / `event_name` 参数, 是否异步由 `async_work.enable`
       决定; 内部直调 (无 `tool_call_id`) 一律同步;
    3. 结果事件名固定 `agentxx.agent.async.done`, 值里用 `asyncId` 区分任务;
    4. 异步任务 `sessionId` = `asyncId`;
    5. 结果值键名 `asyncId` / `canceled` (原 `resultId` / `Canceled`);
    6. 无事件中枢时仍受理, 结果只落台账 (`delivered=false`), 用 `agentxx_event opt=works` 查。
- 生命周期硬约束 (改动时要守住):
    - 所有 `asio::post` / `co_spawn` 的闭包都要持有 `std::shared_ptr<AgentHost>`
      (`host->asyncWork_->pump(key)` / `runner(std::move(host), id)`), 因为容器持有管理器,
      靠它保证 `this` 有效;
    - **先取 executor, 再构造投递体**: 不要在同一表达式里既 `std::move(host)` 又
      `host->ioCtx()` (MSVC 参数求值顺序会让 `host` 先被移走, 变成空指针, 已踩过)。
- 构建/测试 (Windows):
    - `cmake --build agent\build\windows-debug --config Debug --parallel 6`
      (外层 superbuild; 内层 lib 目录 `agentxx_lib_repo-prefix/src/agentxx_lib_repo-build`);
    - 测试 `agent\build\windows-debug\exec\agentxx_test.exe --log-level warn <模块>`;
    - 生成物: `$env:AGENTXX_UPDATE_CONFIG_KEYS='1'` (配置键目录)、
      `$env:AGENTXX_UPDATE_TEST_MODULES='1'` (测试模块清单)。
- 全量回归 (2026-11-05, Debug + ASan/UBSan): **41,585 项断言, 1 项失败** (插件表 v2 与根配置
  段完成后重跑); 唯一失败仍是 `boundaries` 规则 15 的 5 条用词违规, 全部位于
  `agent/third_party/cxx_pluginxx_ui` 子模块 (既有问题, 与本任务无关)。
- 全量回归 (P6 完成后, Debug + ASan/UBSan): **41,812 项断言, 1 项失败** —— 同上, 仍是
  `boundaries` 规则 15 的 5 条 (全在 `cxx_pluginxx_ui` 子模块)。P6 自己新增的断言:
  `async_work` 105 → **121**、`plugin_agent_runtime` 209 → **214**。
