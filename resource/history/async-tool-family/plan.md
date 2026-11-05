# 设计方案: 异步工具族 (async start/query/await/cancel)

## 1. 背景

现在的后台任务（async work）有三处用起来不顺手：

1. **模型侧没有"开始"入口的统一形状**：工具要 `agentxx_tool_async {tool,args}`，子代理却是
   `agentxx_subagent` 自己按配置隐式转后台（同一句调用行为随开关变化，参数里没有任何标记）。
2. **结果默认不回来**：受理文本说"结果会作为输入到达"，实际要模型自己 `agentxx_event watch` /
   `await`，不 watch 就只剩台账。`notifyDelivery` 能力早就在（`AsyncWorkSpec` 字段），只是模型侧
   两条入口都没填。
3. **`agentxx_event` 一工具八种 `opt`**：查询、等待、取消、发布混在一起，schema 里 20 多个参数，
   模型经常选错；`scope` / `state` 两个维度也容易混。

## 2. 目标形状

### 2.1 工具族

| 工具 | 参数 | 行为 |
|---|---|---|
| `agentxx_async_start` | `tool`(必填) / `args`(object) / `notify`(bool, 默认 `true`) / `label` | 提交一条后台任务。`tool` 是任意工具名；`agentxx_subagent` 走 agent 任务（参数与直接调用一致）。`notify=true` 时结果完成即插回会话 |
| `agentxx_async_query` | `group`(`all`/`queued`/`running`/`done`/`failed`/`canceled`, 默认 `all`) / `id` | 给 `id` 查单条（含结果正文）；否则按 `group` 列本会话的后台任务 |
| `agentxx_async_await` | `id`(必填) / `timeout_ms`(默认 30000, 上限 300000) | 阻塞当前轮直到该任务出结果；已完成的直接返回 |
| `agentxx_async_cancel` | `id` / `group`(`all`/`queued`/`running`) | 取消单条（只限本会话启动的）或本会话一批 |
| `agentxx_event` | `opt` = `emit` \| `end`；`event` / `value` / `summary` / `single` / `create` / `label` / `remove` | 只做发布值与结束流 |

删除：`agentxx_tool_async`、`agentxx_event` 的 `watch`/`await`/`status`/`list`/`works`/`cancel`、
`after_ms`/`every_ms`（模型侧定时器随 `watch` 一起移除）。

### 2.2 投递规则（`notify`）

`agentxx_async_start` 的 `notify=true` → `AsyncWorkSpec.notifyDelivery = "next-step"`，`submit` 里
已有的登记逻辑给创建者会话挂一条 `match={"asyncId": id}` 的定向等待，值到达时：

- 轮次在跑 → 作为 `next-step` 输入，在下一次 `ModelCallNode` 前插入上下文；
- 轮次已结束 → `SessionInputSink::submitTurnInput` 入队并开新一轮；
- 会话没有驱动端 → 落待注入输入（`deferred`），不影响台账。

`notify=false` 时不登记，模型只能 `query` / `await`。

### 2.3 subagent 落地方式

`agentxx_async_start {tool:"agentxx_subagent", args:{subagent:"task", message:"..."}}` 由
`AsyncStartTool` 识别目标名后转成 `AsyncWorkSpec{kind=Agent}`（`name`=`subagent`、`message` /
`messages` / `system_prompt` / `tools` / `enable_summarization` 一一对应），走
`AgentHost::spawnOneTask` —— 与旧的隐式异步同一条执行路径，只是入口变成显式的。

`agentxx_subagent` 自己回到纯同步：删掉 `tools/subagent.cpp` 的 `asyncMode` 与中断参数里的
`t["async"]` 标记。`SubagentBatchItem::async` / `buildSubagentResumeValues` 的异步分支保留
（`spawnBatch` 直调的旧路径仍可用）。

## 3. 现状调查（文件 / 函数）

| 位置 | 现状 | 本次动作 |
|---|---|---|
| `agent/lib/src/tools/event.cpp` | 一个类 8 个 `opt`，schema 20+ 参数；`renderAsyncWorks` / `buildWaitSpec` / `renderWaitArmed` 等辅助函数 | 只留 `emit` / `end`，其余分支与辅助函数删除 |
| `agent/lib/src/tools/tool_async.cpp` | `ToolAsyncTool`（`agentxx_tool_async`） | 删除，改成 `async_tools.cpp` 的四个类 |
| `agent/lib/src/tools/subagent.cpp` | `asyncMode = enableAsyncWork && !tool_call_id.empty()`，异步时给中断参数加 `async:true` | 删除该分支（始终同步） |
| `agent/lib/src/agent/base_agent.cpp` | `initEventHub` 受 `enableEvent` 控制；`initTools` 按 `eventHub \|\| asyncEnabled` 注册 `agentxx_event` | 无条件建中枢、无条件注册 `agentxx_event`；`ToolAsyncTool` 换成四个 async 工具 |
| `agent/lib/src/agent/async_work.cpp` | `submit` 支持 `notifyDelivery`（登记定向等待）；`kind=tool` 的 `runToolWork` | 不改逻辑，仅由新工具填 `notifyDelivery` |
| `agent/lib/include/agentxx/agent/config.h` | `enableEvent`（yaml `event.enable`） | 删除 |
| `agent/client/src/config_loader.cpp` + `main.cpp` | 解析/接线 `event.enable` | 删除 |
| `agent/test/core/test_config_keys.cpp` | 期望键 `event.enable` | 删除该条，重新生成 `config-keys.md` |

## 4. 分步实施

1. **中枢常驻**：删 `AgentConfig::enableEvent`、yaml 解析、客户端接线；`initEventHub` 不判开关；
   `initTools` 无条件注册 `agentxx_event`；插件侧"没中枢"注释同步（`plugin_api.h` /
   `plugin_kit.h` / `plugin_mount.cpp` / `plugin_manager_vtable.cpp`）。
2. **`agentxx_event` 精简**：schema 与执行分支只留 `emit` / `end`；删 `renderAsyncWorks`、
   `buildWaitSpec`、`renderWaitArmed`、`renderWaitStatus`、`renderWaitList`、`renderEventStatus`、
   `renderEventList`、`applyTimerArgs`、`kAwaitDefaultMs` / `kAwaitMaxMs`（后两个搬到 `async_tools`）。
3. **`async_tools.{h,cpp}`**：四个类 + 共用参数校验/渲染；`AsyncQueryTool` 用
   `AgentHost::asyncWorksJson(agentId, sessionId, /*includeFinished=*/true, 50)` 后按 `group` 过滤；
   `AsyncCancelTool` 给 `id` 时先查台账校验 `creatorSessionId` 归属，给 `group` 时用
   `AsyncWorkManager::cancelBySession`；`AsyncAwaitTool` 用 `hub->awaitOnce`（`match={"asyncId":id}`，
   `replay=true`，`delivery="none"`）。
4. **notify 默认**：`AsyncStartTool` 在 `notify != false` 时写 `notifyDelivery="next-step"`。
5. **subagent**：删隐式异步；`AsyncStartTool` 增加 `agentxx_subagent` 目标分支。
6. **提示词**：`prompt.cpp` 的 `toolPrompt` 删 `agentxx_tool_async`，新增四个 async 条目，
   重写 `agentxx_event` 条目（只剩 emit/end）。
7. **测试**：
   - 新模块 `async_tools`（`agent/test/core/test_async_tools.cpp`）：start（工具/子代理两条路径、
     `notify` 默认与关闭、权限拒绝、非法参数）、query（group 六档 + id + 归属隔离）、await（已
     完成立即返回、运行中等到结果、超时文本）、cancel（id / group / 别人的 id 拒绝）；
   - `event_tools` 只留 `emit` / `end` 的断言；
   - `event_subagent` 改为 `async_start` 或直接 `submitAsyncWork` 驱动；
   - `config_keys` 删 `event.enable`；`plugin_event` 去掉"没中枢"用例；`event_hub_tools` 同步；
   - `test-modules.md` 重新生成。
8. **文档**：`events.md` 第 6/7/8/12/13/14 节、`index.md`（工具表 + 事件中枢段）、`plugins.md`
   §19.2 与事件表说明、`AGENTS.md`、`config-keys.md`（生成）、`roadmap.md` 修订记录。

## 5. 验证

- 逐模块：`async_tools` / `async_work` / `event_tools` / `event_hub` / `event_subagent` /
  `event_delivery` / `config_keys` / `plugin_event` / `plugin_agent_runtime` / `chatbot_task`。
- 全量回归：允许的唯一失败是 `boundaries` 规则 15（`third_party/cxx_pluginxx_ui` 既有用词）。

## 6. 风险与取舍

- **能力变化**：模型侧失去"等任意事件"与"模型自建定时器"（`watch` + `after_ms`/`every_ms`）；
  插件不受影响（`register_wait` / `register_timer` 照旧）。后续如需，用
  `agentxx_async_start` 扩展"事件订阅"目标补回。
- **`agentxx_subagent` 行为变化**：不再随 `async_work.enable` 改变同步/异步形态；原来的调用点
  （含测试）凡依赖"自动后台"的都要改成显式 `async_start`。
- **通知风暴**：`notify` 默认开，意味着每完成一条后台任务就多一条会话输入；额度默认 3、完成后
  只发一次，风险可接受；模型可用 `notify=false` 关掉。
- **旧插件**：`event.enable` 键被忽略会触发 yaml 未知键警告；插件面不再返回 `ERR_UNSUPPORTED`
  属于"能力变强"，老插件已有的降级分支自然不走。
