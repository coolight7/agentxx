# 实施状态: 内置异步任务模块 (AsyncWork)

方案见 [plan.md](./plan.md)。本文件记录实施进度、已完成/待完成内容与注意事项。

## 总览

| 期 | 内容 | 状态 |
|---|---|---|
| P0 | 抽取 `AsyncWorkManager` + 改名 + 删除全局并发上限/嵌套深度 | 已完成 |
| P1 | 每队列 FIFO + 额度 + 台账/linger + 指标/快照/配置块 + 测试模块 | 已完成 |
| P2 | 提交自包含 (显式清单) + 定向投递; 会话落盘 / HIL 三策略 / 创建者已销毁用例 | 部分 |
| P3 | 单一事件名 + hub 值过滤 `match` + 工具改造 + 插件表 4 成员 | 已完成 |
| P4 | 文档 + 界面; chatbot 接入 | 部分 (文档 + wire/TUI 已做, chatbot 待做) |
| P5 | provider/HTTP 连接池下沉到 `AgentHost` 供所有 AgentNode 共享 (§12-8 人工核对结论) | 待开始 |

提交记录: `2dca9dc5` (P0) → `164029e0` (P1) → `6ea5a5aa` (P3) → `04ea4a57` (P2) →
`0a40ed4b` (P4 文档) → Info 侧边栏「后台任务」段 + `WireContextStats.asyncWorks`
("async_work: Info 侧边栏「后台任务」段 + WireContextStats.asyncWorks") → HIL 三策略 +
会话落盘 + 创建者已销毁用例 → `session_store_keep_days` 清理 (90f46e94) →
插件表 runtime v2 四成员 (b1ea340d) → 根配置 `asyncWork` 段 (8f5a8971)。

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

## 待完成

- **P4 剩余**:
    - chatbot 插件接入: `chatbot_task.h/.cpp` (业务队列 + `submitAsyncWork` + `on_event` 回插)、
      `chatbot_task` 工具 (`submit/status/cancel/list`)、`/agentxx-cmd task ...` 命令、
      「机器人」栏任务段。配置形态 (`config_json` 的 `asyncWork` 段) **已实现**。
- **`kind=tool`** (异步工具) 未实现: `AsyncWorkManager::submit` 对 `kind=tool` 明确返回
  `not supported yet`。
- **`session_store_keep_days`**: 已实现 (提交带落盘的任务时清理过期任务会话目录);
  用例在 `async_work` 模块（`test_session_store_claim`）。

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
