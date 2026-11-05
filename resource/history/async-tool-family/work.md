# 实施记录: 异步工具族

## 进度

- [x] 中枢常驻（删 `event.enable`）
- [x] `agentxx_event` 精简为 `emit` / `end`
- [x] `async_tools.{h,cpp}` 四个工具
- [x] `notify` 默认 `next-step`
- [x] subagent 去隐式异步 + `async_start` 支持子代理目标
- [x] 测试模块改造
- [x] 文档
- [x] 全量回归
- [x] 提交 (`00846f3f`)
- [x] 后续决定: 命令工具的 `background:true` 不做，只用文档说明

## 记录

### 1. 中枢常驻（删 `event.enable`）

- `agent/lib/include/agentxx/agent/config.h`: 删 `enableEvent` 字段，注释改为"中枢是常驻设施"。
- `agent/client/{include/agentxx-client,src}/config_loader.*`: 删 `eventEnable` 与 yaml `event.enable`
  解析；`agent/client/main.cpp` 删接线。
- `agent/lib/src/agent/base_agent.cpp`: `initEventHub` 不再判开关（无条件建 hub）；
  `initTools` 无条件注册 `agentxx_event`。
- 注释同步: `plugin_api.h`（`ERR_UNSUPPORTED` 只给老主程序）、`plugin_kit.h`（`hasEventHub`）、
  `plugin_mount.cpp`、`plugin_manager_vtable.cpp`、`example_event.cpp`。
- 生成物: `config-keys.json` / `config-keys.md` 重新生成（`AGENTXX_UPDATE_CONFIG_KEYS=1`，键 214 条断言）。

### 2. `agentxx_event` 精简（emit / end）

- `agent/lib/src/tools/event.cpp` 只留 `emit` / `end` 两个分支与写侧名字校验
  （保留子段拒绝、插件族不可写）；删掉 `buildWaitSpec` / `applyTimerArgs` /
  `renderAsyncWorks` / `renderSatisfiedText` / 等待相关常量和 `kAwait*`。
- `agent/lib/src/event/hub_text.cpp`: 删 `renderWaitArmed`（无使用者，文本里还在教已删除的
  `opt=status`）；`renderAwaitTimeout` 的收尾文案改为指向 `agentxx_async_query`。
  声明同步从 `hub.h` 移除。
- `prompt.cpp` 的 `agentxx_event` 条目重写（8 个 opt / 20+ 参数 → 2 个 opt + 7 个参数）。

### 3. `async_tools.{h,cpp}` 四个工具

- 新文件 `agent/lib/include/agentxx/tools/async_tools.h` +
  `agent/lib/src/tools/async_tools.cpp`（`AsyncStartTool` / `AsyncQueryTool` /
  `AsyncAwaitTool` / `AsyncCancelTool`），删除 `tool_async.{h,cpp}`。
- `agentxx_async_start`（`tool` / `args` / `notify` / `label`）: 目标为 `agentxx_subagent`
  时按 `kind=agent` 提交（`subagent` / `message`|`messages` / `system_prompt` / `tools` /
  `enable_summarization` 一一对应），其它目标查工具注册表后按 `kind=tool` 提交；
  拒绝把 `agentxx_async_*` 套进去；提交前按目标工具问一次权限；补 `sessionId` /
  `tool_call_id`。
- `agentxx_async_query`（`group` 六档 / `id`）: 台账按**本会话**过滤（`asyncWorksJson({}, session, ...)`），
  `group` 在工具内筛；`id` 走单条详情（含结果正文），非本会话启动的按参数错误拒。
- `agentxx_async_await`（`id` / `timeout_ms`）: 已结束直接回台账结果；否则 `hub->awaitOnce`
  （`match={"asyncId":id}`、`replay=true`、`delivery="none"`，默认 30 s、上限 5 min）。
- `agentxx_async_cancel`（`id` / `group`）: `id` 先查台账校验归属与终态；`group=all` 走
  `cancelAsyncWorksOfSession`，`queued`/`running` 逐条按状态取消；终态分组按参数错误拒。
- 注册: `base_agent.cpp` 只在 `async_work.enable` 时注册（**不再要求 init 时已有容器** ——
  嵌入式用法是先 init 后 attach，工具内部取不到容器时给明确错误）。
- 提示词: 删 `agentxx_tool_async` 条目，新增四个条目（与工具参数一一对应）。

### 4. notify 默认 `next-step`

- `AsyncStartTool` 在 `notify != false` 时写 `AsyncWorkSpec.notifyDelivery = "next-step"`；
  `submit` 里既有的登记逻辑替创建者会话挂 `match={"asyncId":id}` 的定向等待。
- 投递路径复用中枢: 轮次在跑 → `Session::pendingInputs_`（下一次模型调用前插入）；
  轮次已结束 → `SessionInputSink::submitTurnInput` 开新一轮（已有实现，未改动）。

### 5. subagent 回归同步

- `agent/lib/src/tools/subagent.cpp`: 删 `asyncMode` 与中断参数里的 `t["async"]`。
- `renderSubagentStartedText` / `buildSubagentResumeValues` 的异步分支保留（`spawnBatch`
  直调路径仍在用，测试也覆盖）。
- `event_subagent` 用例 3 改成走 `agentxx_async_start`（模型工具调用的真实路径）。

### 6. 测试

- 新模块 `async_tools`（`agent/test/core/test_async_tools.cpp` + 头 + `test.cpp` 注册，
  86 断言）: start（工具/子代理两条路径、notify 默认与关闭、参数与目标校验）、
  query（六档 group、单条 id、归属隔离）、await（已完成/运行中/超时）、
  cancel（id / group / 终态 / 归属）。
- `event_tools` 重写为发布侧（definition 只认 emit/end、建/发/结束/移除、single、
  失败形态），58 断言（原 109）。
- `event_subagent`: 删"没有中枢"用例（中枢恒常驻），结果投递断言兼容两种落点
  （驱动端输入或待注入输入），52 断言。
- `config_keys`: 删 `event.enable` 条目；`plugin_event`: 保留"嵌入环境没中枢 → UNSUPPORTED"
  的降级用例（现主程序不会走到），108 断言。
- `test-modules.md` 重新生成（新增模块 `async_tools`）。

### 7. 文档

- `events.md`: §3/§5 文本样例、§6 换成工具族表、§7 内置源说明、§8 开头与要点（notify、
  取消、查询、子代理）、§9 预算表（删 `event.enable`）、§12/§13/§14。
- `index.md`: 工具表（event + 四个 async_*）、事件中枢段与后台任务段的入口说明。
- `plugins.md`: §19.2 结果读取入口、§21 事件关系、§20.2 `define_event` 说明、`ERR_UNSUPPORTED`。
- `AGENTS.md`: 事件中枢段与后台任务段（工具族、notify、取消/查询、子代理同步、`event.enable` 删除）。
- `roadmap.md`: 2026-11-06 条目。

### 8. 后续决定: `execute_command` 的 `background:true` 不做

- 2026-11-06 定: 后台跑命令不需要命令工具自己实现, 用
  `agentxx_async_start {tool:"agentxx_execute_bash_command", args:{command, timeout:0}}`
  把那次命令调用放到后台就够了; 流式输出以后在 `agentxx_async_*` 上扩。
- 文档同步: `events.md` §13 的「后台命令」条目、`AGENTS.md` 两处 (插件事件表的
  "未做"清单、后台任务段的说明)、`roadmap.md` 的 2026-11-05 条目。
- 代码无改动。

### 9. 后续清理: 删掉指向已删除 opt 的残留

- `agent/lib/src/agent/prompt.cpp`: 系统提示词 "Context Markers" 段里的 `[Event] ...` 行
  原写 "a value you asked for with `agentxx_event` (`opt=watch`/`opt=await`)" —— 这两个 opt 已删除,
  改成描述真实来源 (后台任务结果 / 插件 / 定时器发布的 bubble)。
- 删掉无人调用的状态文本函数 `renderWaitStatus` / `renderEventStatus` (`event/hub.h` 声明 +
  `event/hub_text.cpp` 实现) 以及只给它们用的 `kindText` / `stateText` / `waitStateText`
  (与上一轮删 `renderWaitArmed` 同一个理由)。
- 注释同步: `agent/config.h` 的 `eventWaitTimeoutMs`、`event/hub.h` 的 `waitTimeoutDefault` /
  `deliveryDefault`、`event/hub.cpp` 四处 (`opt=status` / `opt=list` / `agentxx_event opt=await`)、
  `plugins/plugin_manager_vtable.cpp` 的事件记录 JSON 注释。

## 验证

- 逐模块: `async_tools` 86 / `async_work` 121 / `event_tools` 58 / `event_hub` 244 /
  `event_delivery` 46 / `event_subagent` 52 / `config_keys` 214 / `plugin_event` 108 /
  `plugin_agent_runtime` 214 / `chatbot_task` 138 —— 全绿。
- 全量回归 (Debug + ASan/UBSan): **41,853 项通过 / 1 项失败**, 唯一失败是 `boundaries`
  规则 15 的 5 条用词违规（全在 `agent/third_party/cxx_pluginxx_ui` 子模块, 既有问题）。

## 遇到的问题

- **工具注册时机**: 原来 `if (asyncEnabled && !host.expired())` 注册，而嵌入式/测试是
  "先 init 后 attach" → `agentxx_async_start` 根本没注册上（`event_subagent` 用例 3 暴露）。
  改为只看 `async_work.enable`，容器缺失由工具运行期报明确错误。
- **测试断言时序**: `notify` 的投递落点取决于"任务完成时轮次是否还在跑"，结果可能在
  `sink` 也可能在 `pendingInputs_`，断言改成两者取其一。
- `g_da_sim_requests` 是加锁包装（`DaSimValue`），遍历要用 `.get()` 而不是直接 begin/end。
