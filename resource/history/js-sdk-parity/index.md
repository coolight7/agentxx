# JS 插件 SDK 与 C++ 插件 SDK 对齐

## 目标

脚本 (`agentxx_javascript_engine` 加载的 `plugin.js`) 的全局桥按**接口表成员**逐条对齐
C++ 插件 SDK (`plugin_kit.h`)：每个 C ABI 成员要么有同名同义的 JS 函数，要么在文档里
写清为什么脚本侧不需要它。

## 范围

- 引擎桥 (`agent/plugins/agentxx_javascript_engine/agentxx_javascript_engine.cpp`)：
  agent 侧与 client 侧两套桥
- 示例脚本 `agent/plugins/example_js/plugin.js`
- 测试 `agent/test/plugin/test_plugins.cpp`(agent 侧)、`test_client_plugins.cpp`(client 侧)
- 文档 `docs/zh-cn/design/plugins.md` §12、`AGENTS.md`

## 状态

已完成（含后续补上的执行图节点类型，见 `work.md` §5）

## 关键决策

- **实现**：`getOwnInfo` / `getPluginConfigPath`(agent 侧缺)、`checkPaths` + `filterPaths`
  (`check_paths` 三态查询)、`subscribe` 的 `options` 与流结束回调 (`subscribe_broadcast_ex`)、
  能力表全套 (`provideCapability` / `unprovideCapability` / `providePoint` / `listCapabilities` /
  `capabilityInfo` / `invokeCapability`)、执行图节点类型 (`registerNodeType` / `unregisterNodeType`)
- **不实现** (记录理由，见 `plan.md` §3)：`scheduler.*`、`cancel.op_cancel`、`tasks.*`、
  `coroutine_runtime.*`、`json.*`（`graph.register_node_type` 后来发现可行，已实现）

## 结论

- 脚本侧与 C++ 插件 SDK 的**差距已清空**：能对齐的都补齐了，剩下 4 项（scheduler /
  cancel.op_cancel / tasks / coroutine_runtime）在脚本模型下确实不需要，
  理由写在 `plan.md` §3.2 与 `plugins.md` §12。图节点类型与 `json.*` 最初也归在这组里，
  后来发现前者本来就是异步操作契约（`run_start` + `notify->done`）、后者两个成员直接可转译，
  均已实现并做了真实用例。
- 接口级细节（工具选项 / 协作式取消 / client 侧能力行）已在第二轮补齐，见 `work.md` §6；
  剩下的差异只有语言模型类的：无线程与 offload、无协程、无通用 op 句柄、
  以及脚本没有 create/start/mount 三段与进程级单例。
- 过程中发现并修掉两处**旧 bug**：实例订阅表从未初始化（脚本 `subscribe`/`onHook` 的
  回调从不执行）、一次派发会重复调用同频道的全部处理器（现按订阅下标各调一次）。
- 测试：`plugins` 702（含图节点真实执行、工具标志/超时/取消、JSON）、`client_plugins` 835
  （含 client 侧能力行往返），插件相关 20 个模块全绿（见 `work.md`）。
- 偏差：`providePoint` 只做"登记 / 自省 / 撤销"的端到端验证，没有在真实取值链上断言
  （取值链由 `plugin_point` / `plugin_feature` 覆盖 C++ 侧；脚本侧的登记形状与它们同一张表）。

