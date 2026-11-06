# 任务卡片: 异步工具族 (async start/query/await/cancel)

## 目标

把后台任务的模型侧入口收敛成一组专用工具，并把"结果主动回会话"变成默认行为：

- 新增 `agentxx_async_start` / `agentxx_async_query` / `agentxx_async_await` / `agentxx_async_cancel`；
- `agentxx_tool_async` 改名并扩展为 `agentxx_async_start`（`tool` + `args` + `notify`），以后兼容
  "事件通知订阅"等其它异步内容；
- `agentxx_subagent` 回归成**普通工具**（同步等结果），要后台跑就走 `agentxx_async_start`；
- `agentxx_event` 只保留 `emit` / `end`（发布值、结束流）；
- 事件中枢**常驻**：删除 `event.enable` 配置键与"没中枢"的所有降级分支。

## 范围

| 层 | 文件 |
|---|---|
| 工具 | `agent/lib/{include/agentxx/tools,src/tools}` 的 `event.*`、新 `async_tools.*`、`subagent.*` |
| 注册与提示词 | `agent/lib/src/agent/base_agent.cpp`、`agent/lib/src/agent/prompt.cpp` |
| 配置 | `agent/lib/include/agentxx/agent/config.h`、`agent/client/{include,src}/config_loader.*`、`agent/client/main.cpp` |
| 后台任务 | `agent/lib/src/tools/*`、`agent/lib/src/agent/async_work.cpp`（notify 默认值） |
| 插件注释 | `plugin_api.h` / `plugin_kit.h` / `plugin_mount.cpp` / `plugin_manager_vtable.cpp` 里"没中枢"的说法 |
| 测试 | `async_tools`(新) / `event_tools` / `event_subagent` / `async_work` / `config_keys` / `plugin_event` / `event_hub_tools` |
| 文档 | `events.md`、`index.md`、`plugins.md`、`config-keys.md`(生成)、`AGENTS.md`、`roadmap.md` |

## 状态

已完成（实现 + 测试 + 文档 + 全量回归；提交记录见下方）。

## 关键决策

1. **subagent 不再隐式异步**：`agentxx_subagent` 永远同步等结果；后台执行走
   `agentxx_async_start {tool:"agentxx_subagent", args:{subagent, message, ...}}`（内部按
   `kind=agent` 提交）。参数形状与直接调用完全一致。
2. **结果默认主动回上下文**：`notify=true`（默认）时提交即登记一条 `next-step` 定向等待 ——
   轮次在跑就插到下一次 `ModelCallNode` 前，轮次已结束就入队触发下一轮；`notify=false` 只落台账。
3. **`agentxx_event` 只做发布**：`watch` / `await` / `status` / `list` / `works` / `cancel` 全部
   迁出（查询/等待/取消归 `async_*`），定时器参数 `after_ms` / `every_ms` 随 `watch` 一起移除。
4. **中枢常驻**：删除 `event.enable`；插件面 `AGENTXX_PLUGIN_EVENT_ERR_UNSUPPORTED` 保留为兼容
   常量，但主程序不再返回它（只有老主程序会）。
5. **`group=all` 指本会话**：不再有 `scope=all` 的跨 agent 视图；`scope` 参数整体删除
   （原 `scope` 是"谁的任务范围"，`state` 是任务状态，两者不是一回事，这里只保留状态维度）。
6. **`agentxx_async_*` 仍受 `async_work.enable` + 容器约束**；`agentxx_event` 与中枢不受开关控制。

## 结论

- 模型侧现在只有四个异步工具 + 一个发布工具，`opt` 迷失的问题没有了：开始 / 查 / 等 / 取消
  各一个工具，参数各自两三个；
- **结果默认自己回会话**（`notify` 默认 `true`，`next-step` 投递：在跑就插下一次模型调用前，
  已结束就开新一轮），模型不再需要“先 watch 再等”；
- `agentxx_subagent` 回到普通工具的形态（同步等结果），后台跑统一走
  `agentxx_async_start {tool:"agentxx_subagent", args:{...}}`，为以后“事件订阅”类目标留了同一入口；
- 事件中枢常驻（没有 `event.enable`），`agentxx_event` 只负责发布侧；
- 全量回归 41,853 / 1（唯一失败是 `boundaries` 规则 15 的 third_party 既有用词）。
- 与计划偏差: ①工具注册不再要求 init 时已有容器（嵌入式先 init 后 attach）；
  ②`agentxx_async_await` 只接 `id`（不再支持等任意事件），见 [plan.md](./plan.md) §6 风险段。
