# 实施状态: 内置异步任务模块 (AsyncWork)

方案见 [plan.md](./plan.md)。本文件记录实施进度、已完成/待完成内容与注意事项。

## 总览

| 期 | 内容 | 状态 |
|---|---|---|
| P0 | 抽取 `AsyncWorkManager` + 改名 + 删除全局并发上限/嵌套深度 | 已完成 |
| P1 | 每队列 FIFO + 额度 + 台账/linger + 指标/快照/配置块 | 进行中 (队列/额度/台账/配置已完成; 指标+快照待补) |
| P2 | 提交自包含 (显式清单)、创建者先死、会话落盘、HIL 三策略 | 部分 (提交快照已带 `baseConfig`) |
| P3 | 单一事件名 + hub 值过滤 `match`、工具改造、插件表 4 成员 | 部分 (事件名已单一化, `match` 未做) |
| P4 | wire `asyncWorks` + TUI Info 段 + chatbot 接入 + 文档 | 待开始 |
| P5 | provider/HTTP 连接池下沉到 `AgentHost` 供所有 AgentNode 共享 (§12-8 人工核对结论) | 待开始 |

## 已完成

### P0 抽取与改名

- 新增 `agent/lib/include/agentxx/agent/async_work.h` + `agent/lib/src/agent/async_work.cpp`:
  `AsyncWorkSpec` / `AsyncWorkEntry` / `AsyncWorkSubmitResult` / `AsyncWorkManager`
  (台账 + 每队列 FIFO + 额度 + 停用清扫 + 取消/查询)。
- 从 `agent_host.cpp` 搬走异步派发三段 (`spawnBatch` 的异步分支 / `startAsyncSubagentTask` /
  `runAsyncSubagentTask`), `spawnBatch` 改为构造 `AsyncWorkSpec` 后 `submitAsyncWork`。
- 改名到位: `AsyncSubagentJob`→`AsyncWorkEntry`、`asyncJobs_`→`asyncWork_` (管理器内台账)、
  `job-<n>`→`async-<n>`、`runningAsyncJobs()`→`runningAsyncWork()`、
  `cancelAsyncJobs*`→`cancelAsyncWorks*`、日志前缀 `[subagent] async job`→`[async_work]`。
- **删除**: `AgentHost::Config::maxDepth` / `maxConcurrentSubagents`、`sessionDepth_`、
  `runningSubagentCount_`、深度/并发拒绝分支、`eventNameSuffix` 与同名事件冲突检查。
  `AgentNode::depth` 保留为展示字段 (恒 0), 容器不再限制嵌套。
- 异步任务的 sessionId 直接用 `asyncId` (不再生成 `subagent_<名>_<agentId>_<n>`)。
- `spawnOneTask` 增加可选 `const AsyncWorkSpec*` 参数: 派生基准配置优先取
  快照里的 `baseConfig` (创建者已销毁也能派生); 子代理 `AgentContext::agentId` 现在会被填上。
- 事件名单一化 (`agentxx/event/naming.h`): 新增值事件族 `agentxx.agent.async.`,
  `isEventName` / `validateEventName` 接受它; 结果事件默认名 = `agentxx.agent.async.done`。
  `SubagentBatchItem::eventName` 仍可显式指定 (内部直调兼容)。
- 结果值形状改为 `{asyncId, kind, name, content, hasError, canceled, errorMessage,
  creatorSessionId, startedAtMs, finishedAtMs}` (单一标识 `asyncId`)。
- `config.h` 新增 7 个 `async_work.*` 字段 + `AgentConfig` 接线 (client 侧 yaml 解析、
  `main.cpp` 的 applyServerRuntimeConfig)。

### P1 (已完成部分)

- 每队列 FIFO + 额度 (`async_work.max_concurrent`, 默认 3); 队列之间互不影响;
  不设排队条数/嵌套深度/全局上限。
- 台账条目 + `linger_ms` 到期清扫 (内存回收), `result_max_bytes` 截断结果正文。
- 取消: 单条 (排队中直接出队并补位, 运行中置令牌)、按创建者、按创建者会话。
- `AgentHost` 公开入口: `submitAsyncWork` / `cancelAsyncWork(s)` /
  `cancelAsyncWorksOfSession` / `asyncWorkJson` / `asyncWorksJson` /
  `runningAsyncWork` / `queuedAsyncWork` / `shutdownAsyncWorks`。
  生命周期安全: 所有投递与协程帧都持有容器强引用 (容器含管理器, 不会被提前释放)。

## 待完成

- P1 收尾: `KeyMetrics` 计数 (`asyncWorkSubmitted/Finished/Failed/Canceled/QueuedPeak/RunMs`)、
  加载快照 `root["async_work"]` 段、`--dump-diagnostics` 文字段、`async_work` 新测试模块
  (队列 FIFO/额度/无限排队/linger 清扫/sessionId==asyncId)。
- P2: 显式挂载清单 (`plugins`/`mcp_servers`/`skills`/`memory_files`/`rag_docs`/`work_dir`/
  `language`/权限清单) 落到派生配置; 创建者已销毁路径的自测; `notify_delivery`;
  `session_store` 落盘 `{创建者会话目录}/agent/{async_id}`; HIL 三策略应答器。
- P3: hub 等待值过滤 `match`; `agentxx_subagent` 去掉 `async`/`event_name` 参数
  (改为 `async_work.enable` 决定, 内部直调按"无 tool_call_id"判同步);
  `agentxx_event` 的 `opt=works` 与 `status/await/watch/cancel` 的 `id`;
  `agentxx.agent.runtime` 表尾 4 成员 + kit + 归属回收 + `boundaries`/`plugin_runtime` 台账。
- P4: `WireContextStats.asyncWorks` + TUI Info「后台任务」段 + chatbot 接入 + 文档更新。
- P5: provider/HTTP 连接池下沉 `AgentHost`。

## 注意事项

- plan.md §12 的人工核对结论:
    - `kind=tool` 后做 (先 agent), 异步启动的参数名统一为 `async:bool`
    - provider/HTTP 连接池共享要做: 迁移到 `AgentHost` 提供, 所有 `AgentNode` 共享,
      agent / tool 都使用同一份
- 行为变化 (写进发布说明):
    1. 删除进程级并发上限与嵌套深度上限, 只留每队列额度;
    2. 结果事件名默认固定 `agentxx.agent.async.done`, 值里用 `asyncId` 区分任务;
    3. 异步任务 `sessionId` = `asyncId`;
    4. 结果值键名改为 `asyncId`/`canceled` (原 `resultId`/`Canceled`);
    5. 无事件中枢时仍受理, 结果只落台账 (`delivered=false`)。
- 构建/测试 (Windows):
    - `cmake --build agent\build\windows-debug --config Debug --parallel 6`
      (外层 superbuild; 内层 lib 目录 `agentxx_lib_repo-prefix/src/agentxx_lib_repo-build`)
    - 测试 `agent\build\windows-debug\exec\agentxx_test.exe --log-level warn <模块>`
    - `config_keys` 生成物: `$env:AGENTXX_UPDATE_CONFIG_KEYS='1'` 跑一次即重写
      `agent/schema/config-keys.json` 与 `docs/zh-cn/design/config-keys.md`
- `boundaries` 模块失败来自 `agent/third_party/cxx_pluginxx_ui` (第三方子模块用词规则),
  与本次改动无关。
