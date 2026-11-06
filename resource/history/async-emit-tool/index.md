# 任务卡片: agentxx_async_emit (事件发布工具与发布权限)

## 目标

1. `agentxx_event` 更名为 `agentxx_async_emit`，与 `agentxx_async_*` 一组；
2. 用 `end: bool` 代替 `opt`，一次调用可以只发值、只收尾、或发值后收尾；
3. 事件名不再由工具做白名单（任意合法事件名都可以尝试发布），
   能不能发由**创建该事件时声明的发布者**决定；
4. 工具结果改成"有新信息的短句"，不再复述参数、不再像日志；
5. 顺手删掉 `execute_command` 的 `background:true` 相关文档与注释（从未实现、也不打算实现）。

## 状态

已完成（实现 + 测试 + 文档 + 提交）。

## 关键决策

1. **发布权限记在事件定义上**：`EventDefineSpec.publishMask`（默认 0 = 只有创建者能发）。
   创建者自己总能发布；其他人按 `model` / `plugin` / `builtin` 位判定。
   模型创建的 `agentxx.agent.event.*` 归属固定 `tool`，所以"模型自己发自己的事件"不受限。
2. **名字放开，创建收紧**：工具允许任意合法事件名（`agentxx.agent.event.*` / `agentxx.agent.async.*` /
   `plugin.*`）；但**隐式创建**只允许模型在 `agentxx.agent.event.*` 的非保留段里做。
   保留段（`turn.` / `tool.` / `subagent.` / `timer.` / `host.`）不能由模型创建，避免抢内建名字。
3. **`opt` 拆成 `end`**：`value` 给了就发布，`end: true` 就收尾；两个都没给是参数错误。
   `single` / `create` / `remove` 删除（单值语义用 `end: true` 表达，记录按 linger 自然回收）。
4. **结果文本给新信息**：投递给了几个等待（wait id + delivery）、累计发布条数、还剩几个等待、
   是否新建了事件与谁能发、是否收尾及因收尾结束的等待数；不复述事件名与值。
5. **插件面同样可声明**：`AgentxxPluginEventSpec` 尾部追加 `publish_mask`，
   老主程序按 `struct_size` 忽略；`emit_value` / `end_event` / `remove_event` 被拒时报
   `AGENTXX_PLUGIN_EVENT_ERR_DENIED`。

## 结论

- 模型侧只剩一个事件工具 `agentxx_async_emit`，参数 6 个（`event` / `value` / `end` /
  `summary` / `allow_publish` / `label`），`opt` 的取值再也不会被记错；
- 发布权限从"谁都能发"变成"创建时决定"，跨族发布（模型给插件事件发值）成为正当用法；
- 事件中枢多了 `PublishMask`、`publishValueEx` / `endEventEx`（含投递统计），
  工具与插件面都用同一套判定；
- 全量回归：**41,907 项通过 / 1 项失败**，唯一失败是 `boundaries` 规则 15 在
  `agent/third_party/cxx_pluginxx_ui` 里的 5 处用词（既有问题，与本次无关）；
- 与计划偏差：①匿名发布（内部代码/测试不带身份）视为记录自己的归属，与旧行为一致，
  只有模型工具与插件面做身份校验；②内置源在框架事件段内始终可以发布（同一段被多个内置源
  共用）；③`single` / `create` / `remove` 参数随 `opt` 一起删掉，单值语义用 `end: true` 表达；
  ④`allow_publish` / `label` 只在创建那一次生效。
