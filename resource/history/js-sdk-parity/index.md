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

进行中 (见 `work.md` 的进度)

## 关键决策

- **实现**：`getOwnInfo` / `getPluginConfigPath`(agent 侧缺)、`checkPaths` + `filterPaths`
  (`check_paths` 三态查询)、`subscribe` 的 `options` 与流结束回调 (`subscribe_broadcast_ex`)、
  能力表全套 (`provideCapability` / `unprovideCapability` / `providePoint` / `listCapabilities` /
  `capabilityInfo` / `invokeCapability`)
- **不实现** (记录理由，见 `plan.md` §3)：`graph.register_node_type`、`scheduler.*`、
  `cancel.op_cancel`、`tasks.*`、`coroutine_runtime.*`、`json.*`

## 结论

- 脚本侧与 C++ 插件 SDK 的**差距已清空**：能对齐的都补齐了，剩下 6 项（图节点类型 /
  scheduler / cancel.op_cancel / tasks / coroutine_runtime / json）在脚本模型下确实不需要，
  理由写在 `plan.md` §3.2 与 `plugins.md` §12。
- 过程中发现并修掉两处**旧 bug**：实例订阅表从未初始化（脚本 `subscribe`/`onHook` 的
  回调从不执行）、一次派发会重复调用同频道的全部处理器（现按订阅下标各调一次）。
- 测试：`plugins` 677、`client_plugins` 832，插件相关 15 个模块全绿（见 `work.md`）。
- 偏差：`providePoint` 只做"登记 / 自省 / 撤销"的端到端验证，没有在真实取值链上断言
  （取值链由 `plugin_point` / `plugin_feature` 覆盖 C++ 侧；脚本侧的登记形状与它们同一张表）。

