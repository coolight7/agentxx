# 计划：JS 插件 SDK 与 C++ 插件 SDK 对齐

## 1. 背景与目标

`agentxx_javascript_engine` 给脚本一份全局桥 (`agentxx.*`)。桥是**逐条**实现的，
C++ SDK 每加一个接口表成员，脚本侧就可能落后一格。本任务按接口表把差距一次盘清，
把值得对齐的补齐，剩下的写清为什么不适用。

## 2. 现状调查 (按接口表逐条)

判定方法：从 `pluginxx/api/tables.h`、`pluginxx/api/net.h`、`agentxx/plugin/api/plugin_api.h`、
`agentxx/plugin/api/client_plugin_api.h` 抽出每张表的成员，再对引擎的 `def("...")` 注册表
与 `case B_*` 实现逐一核对。

### 2.1 agent 侧业务表 (`agentxx.agent.*`)

| 表 | 成员 | 脚本侧 | 状态 |
|---|---|---|---|
| `agentxx.agent.tools` | `register_tool` / `unregister_tool` / `call_tool_async` | `registerTool` / `unregisterTool` / `callTool` | 已对齐 |
| `agentxx.agent.permission` | `register_tool_permission` / `unregister_tool_permission` | `registerToolPermission` / `unregisterToolPermission` / `registerReadPathPermission` / `registerWritePathPermission` | 已对齐 |
| | `check_paths` | — | **缺** |
| `agentxx.agent.session` | `get_share_store` / `emit_message_tip` / `add_share_store` / `get_session_work_dir` / `is_canceled` | `getShareStore` / `emitMessageTip` / `addShareStore` / `getSessionWorkDir` / `isCanceled` | 已对齐 |
| `agentxx.agent.context` | `get_messages` / `messages_count` | `getMessages` / `messagesCount` | 已对齐 |
| `agentxx.agent.model` | `get_config` | `getModelConfig` | 已对齐 |
| `agentxx.agent.prompt` | `get_prompt` / `set_prompt` / `get_tool_prompt` | `getPrompt` / `setPrompt` / `getToolPrompt` | 已对齐 |
| `agentxx.agent.resources` | 7 个 skill/memory/mcp + `get_own_resources` | `addSkillDir` / `removeSkillDir` / `addMemoryFile` / `removeMemoryFile` / `addMcpServer` / `removeMcpServer` / `getOwnResources` | 已对齐 |
| `agentxx.agent.graph` | `get_graph_json` / `get_graph_name` / `set_graph_json` | `getGraphJson` / `getGraphName` / `setGraphJson` | 已对齐 |
| | `register_node_type` / `unregister_node_type` | — | **不实现** |
| `agentxx.agent.mount` | `list` / `get` | `listMounts` / `getMount` | 已对齐 |
| `agentxx.agent.runtime` | 20 个 (根/通道/触发器/定时器/后台任务) | `mountRoot` … `listAsyncWork` | 已对齐 |

### 2.2 agent 侧通用表

| 表 | 成员 | 脚本侧 | 状态 |
|---|---|---|---|
| `agentxx.broadcasts` | `declare_broadcast` / `publish_broadcast` / `subscribe_broadcast` / `read_broadcast` / `unsubscribe_broadcast` / `end_broadcast` | `declareBroadcast` / `publish` / `subscribe` / `readBroadcast` / `unsubscribe` / `endBroadcast` | 已对齐 |
| | `subscribe_broadcast_ex` (带 `on_end`)、`options_json` (订阅方通知间隔) | — | **缺** |
| `agentxx.capabilities` | `register_capability` / `unregister_capability` / `unregister_capability_ex` / `invoke_capability` / `invoke_capability_ex` / `capability_info` / `list_capabilities` | — (只有引擎自己的 `interpreter.js`) | **缺** |
| `agentxx.plugins` | `list_plugins` / `get_plugin` / `get_own_info` | `listPlugins` / `getPlugin` / — | **缺 `getOwnInfo`** |
| `agentxx.config` | `get_config` / `get_plugin_args` / `get_plugin_config_path` / `get_language` / `set_language` | `getConfig` / `getPluginArgs` / — / `getLanguage` / `setLanguage` | **缺 `getPluginConfigPath`** |
| `agentxx.net` | `call` / `call_async` / `cancel` | `netCall` / `netCallAsync` / `netCancel` (+ fetch/download/ws/http) | 已对齐 |
| `agentxx.scheduler` | `is_io_thread` / `post_to_io` / `sleep` / `op_cancel` / `offload` | — | 不适用 |
| `agentxx.cancel` | `op_cancel` | — | 不适用 |
| `agentxx.tasks` | `register_task` / `cancel_task` | — | 不适用 |
| `agentxx.coroutine_runtime` | 3 | — | 不适用 |
| `agentxx.json` | `json_get_string` / `json_escape` | — | 不适用 |

### 2.3 client 侧表

| 表 | 成员 | 脚本侧 | 状态 |
|---|---|---|---|
| `agentxx.client.ui` | 19 (状态栏项/面板/Info/命令/装饰/渲染器/动作/overlay) | 全部有对应函数 | 已对齐 |
| `agentxx.client.session` / `wire` / `self` | 3 / 1 / 5 | 全部 | 已对齐 |
| `agentxx.client.timer` / `keybind` | 3 / 3 | 全部 | 已对齐 |
| `agentxx.broadcasts` | 同 agent 侧 | 同 agent 侧 | **缺 `options` 与流结束回调** |
| `agentxx.capabilities` | 通用表, client 侧同样由 `PluginHostCore` 提供 | — | **缺** |

## 3. 缺口与决策

### 3.1 实现

1. `getOwnInfo()`：agent 侧缺 (`plugins.get_own_info`)；client 侧已有。
2. `getPluginConfigPath()`：agent 侧缺 (`config.get_plugin_config_path`)；client 侧已有。
3. `checkPaths(paths, scope, sessionId?)` + `filterPaths(...)`：对应 `permission.check_paths`
   三态批量查询。只读判定、不发起询问；`filterPaths` 按 fail-closed 丢弃非允许项
   (与 kit 的 `filterPathPermissions` 同语义)。
4. `subscribe(topic, handler, options)`：`options` 支持 `{intervalMs, mode, onEnd}`。
   无 `onEnd` 时用 `subscribe_broadcast` (带 `options_json`)；有 `onEnd` 时用
   `subscribe_broadcast_ex`。旧的 `subscribe(topic, handler)` 行为不变。
5. 能力表 (`agentxx.capabilities`)：
   - `provideCapability({id, handler, ownerTag?, priority?, timeoutMs?, merge?, impls?, phases?})`
     → 登记一行实现 (异步处理器，脚本回调在 JS 线程执行)；
   - `unprovideCapability(id, ownerTag?)`；
   - `providePoint(pointId, handler, {priority?, timeoutMs?, ownerTag?})` → 按功能埋点的
     形状登记 (`impls: many` + `merge: first` + 只开 `impl` 阶段)，回答按 kit 的
     `pointAnswer` 规范化 (无 `value`/`disable` 时包成 `{"value": …}`)；
   - `listCapabilities()` / `capabilityInfo(id)`：自省；
   - `invokeCapability(id, args?, {timeoutMs?, phase?})` → Promise (完成回调在 io 线程，
     结果投递回 JS 线程 settle)。

### 3.2 不实现 (及理由)

- ~~`graph.register_node_type` / `unregister_node_type`~~：**后来已实现**（见 `work.md` §5）。
  当初判断“图引擎在自身线程同步调用节点执行函数”有误 —— 节点执行走的是 `run_start` +
  `notify->done` 的异步操作契约，与能力行同一形状，因此不需要 io→JS 同步桥。脚本现在用
  `registerNodeType({type, run, configSchema?})` 登记，`run(input)` 返回节点输出对象。
- `scheduler.is_io_thread` / `post_to_io` / `sleep` / `op_cancel` / `offload`：脚本没有
  "线程" 概念 (`is_io_thread` 无意义)，定时用 `setTimeout`，`offload` 只能跑 C++ 函数。
- `cancel.op_cancel`：脚本侧没有通用 op 句柄；网络有 `netCancel`、通道有 `cancelChannel`、
  后台任务有 `cancelAsyncWork`、能力调用由 Promise 自带 `timeoutMs`。
- `tasks.register_task` / `cancel_task`：脚本的后台工作走 `submitAsyncWork`
  (已有 owner 归属、取消与配额)。
- `coroutine_runtime.*`：脚本没有协程。
- `json.*`：JSON 是 JS 内置能力。

## 4. 实施步骤

1. 枚举与注册表：新增 `B_GET_OWN_INFO`(agent) / `B_GET_CONFIG_PATH_AGENT` /
   `B_CHECK_PATHS` / `B_FILTER_PATHS` / `B_PROVIDE_CAPABILITY` / `B_UNPROVIDE_CAPABILITY` /
   `B_PROVIDE_POINT` / `B_LIST_CAPABILITIES` / `B_CAPABILITY_INFO` / `B_INVOKE_CAPABILITY`。
2. agent 侧 case 实现 (1)(2)(3)。
3. `subscribe` 扩展 (两侧共用同一段 case)：保留旧签名，新增 options 解析与 `onEnd` 存储。
4. 能力表：新增 `JsCapImpl` (一条 JS 实现行) 与跨线程 trampoline (`jsCapImplStart` /
   `jsCapImplCancel`)，在实例上下文里记账并进 `rollbackActions`；`invokeCapability` 用
   `JsCapInvokeBridge` (与 `JsNetBridge` 同构) 把结果投回 JS 线程 settle。
5. client 侧同样开放能力表 (client 的通用表本来就由 `PluginHostCore` 提供，只是桥没开)。
6. 文档：`plugins.md` §12 增补一节；`AGENTS.md` 的 JS 插件段补一句。
7. 测试：agent 侧 (`test_plugins.cpp`) 与 client 侧 (`test_client_plugins.cpp`) 各补断言；
   示例脚本 `example_js/plugin.js` 补 `providePoint` / `checkPaths` / `subscribe` 的用法。

## 5. 验证

- 编译：插件动态库 (`build_plugins.ps1 agentxx_javascript_engine example_js`)
- 测试：`agentxx_test plugins`、`agentxx_test client_plugins`、以及既有的
  `plugin_point` / `plugin_feature` / `plugin_broadcast` 保证没破坏既有语义
- 手工核对：`providePoint` 登记的实现在功能埋点取值链里生效 (通过 `plugin_point` 的
  JS 侧断言验证)

## 6. 风险

- 能力实现行的处理器是**异步**处理器 (返回 token，稍后 `done`)：JS 线程停/实例卸载时
  必须把在飞的调用按时收尾 (CANCELED/FAILED)，否则主程序操作悬挂。
- `invokeCapability` 一律返回 Promise (不阻塞 JS 线程)：不能用 `drivePromise` 同步等，
  否则同引擎内 A 脚本调 B 脚本时会自锁 (B 的实现在 JS 线程上排队)。
- 能力行的撤销顺序：实例卸载时主程序已按挂载记录撤销，脚本侧 `runRollback` 里再撤销一次
  必须是幂等的 (注销不存在的行返回 `ERR_NOT_DECLARED`，忽略即可)。
