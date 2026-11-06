# 实施记录: agentxx_async_emit

## 进度

- [x] 中枢: `PublishMask` + 权限判定 + `publishValueEx` / `endEventEx`
- [x] 插件面: 结构体尾部 `publish_mask`、拒绝码、kit 选项
- [x] 工具: `agentxx_async_emit`（替换 `agentxx_event`）+ 提示词 + 注册
- [x] 测试: `event_tools` 重写、`event_hub` 兼容、`plugin_event` 权限用例
- [x] 文档: events.md / index.md / plugins.md / tui.md / AGENTS.md / roadmap.md
- [x] 删除 `background:true` 相关表述
- [x] 构建 + 目标模块 + 全量回归 + 提交

## 记录

### 1. 中枢: 发布权限与投递统计

- `event/hub.h`:
  - 新增 `PublishMask`（None/Model/Plugin/Builtin/Any）与 `PublishRole`；
  - `EventDefineSpec.publishMask`、`EventRecord.publishMask`；
  - 自由函数 `publishRoleOf` / `publishRoleName` / `publishMaskText` / `mayPublish` /
    `mayCreateByPublish`；
  - `PublishOutcome`（created/endedNow/published/delivered/remainingWaits/deliveries）与
    `EndOutcome`（alreadyEnded/published/closedWaits）；
  - `publishValueEx` / `endEventEx`，`endEvent(name, summary, owner, err)`、
    `removeEvent(name, owner, err)`、`postEnd(name, summary, owner)`。
- `event/hub.cpp`:
  - 角色判定 `plugin:` 前缀 → Plugin、`tool` → Model、其余 → Builtin；
  - 创建资格：Model 只能建 `agentxx.agent.event.*` 非保留段、Plugin 只能建 `plugin.*`、
    Builtin 可建框架两段；
  - 发布判定：创建者自己 → 放行；Builtin + 框架段 → 放行；其余看 `publishMask`；
  - **没有带身份的调用**（内部代码/测试）视为记录自己的归属，与旧行为一致；
  - `dispatchValue` 返回本次投递到的等待（`{"waitId","delivery"}`），
    `publishValueEx` 据此填 `delivered` / `deliveries`；
  - 总线桥按名字推断归属再发布（否则会被当成内置源）；
  - `listEventsJson` 增加 `publishTo`。
- `define()` 重复定义时 `publishMask` **只升不降**（放宽可以，收窄不行）。

### 2. 插件面

- `plugin_api.h`：`AgentxxPluginEventSpec` 尾部 `uint32_t publish_mask`，
  基线改为 `offsetof(AgentxxPluginEventSpec, publish_mask)`（旧插件的短结构仍被接受）；
  新增 `AGENTXX_PLUGIN_EVENT_PUBLISH_MODEL/PLUGIN/BUILTIN/ANY` 与
  `AGENTXX_PLUGIN_EVENT_ERR_DENIED (-7)`。
- `plugin_spec.h`：补 `AGENTXX_DECLARE_SPEC_BASELINE(AgentxxPluginEventSpec, ...)`。
- `plugin_manager_vtable.cpp`：define 读尾部字段（`specHas(&...::publish_mask, struct_size)`）；
  emit/end/remove 先按中枢权限判定，越权返回 `ERR_DENIED`；`eventRecordJson` 增加 `publishTo`。
- `plugin_kit.h`：`EventDefineOptions.allowPublish`，`defineEvent` 写入尾部字段。

### 3. 工具

- `tools/event.{h,cpp}` 删除，新增 `tools/async_emit.{h,cpp}`（`AsyncEmitTool`，名字
  `agentxx_async_emit`）：`event` 必填，`value` / `end` / `public_publish` / `label`；
  参数组合非法（既无 value 也无 end、summary 无 end）按参数错误；
  创建前用 `mayCreateByPublish` 校验资格，创建时写 `publishMask` 与 `label`。
- 结果文本：`Created a new stream (publishing allowed to: …)` + `N value(s) published so far` +
  `delivered to M waiter(s): w-3 next-step…` / `nobody was waiting` + `K waiter(s) still attached` +
  `stream closed after N value(s); K waiter(s) finished without a value`；不回显事件名与值。
- `base_agent.cpp` 注册改名；`prompt.cpp` 条目重写（含"谁能发布"一节的说明）。

### 4. 测试

- `event_tools` 重写为 6 个用例（定义/创建与计数/投递统计/收尾/权限/失败形态），97 断言。
- `plugin_event` 新增 `test_publish_permission`（默认只有创建者、放开 model 后模型可发可收尾、
  别家事件返回 `ERR_DENIED`、放开 plugin 后通过），123 断言。
- `event_hub` / `event_delivery` / `event_subagent` / `async_work` 未改动，全部通过（匿名发布
  仍按记录归属处理，见第 1 节）。
- 期间修掉一个真问题：`publishRoleOf` 最初按 `plugin.` 前缀判断，而插件归属是 `plugin:<名>`。

### 5. 文档

- `events.md`：§2 生产者示意、§6 工具表、新增 §6.1（发布权限与结果文本）、§12.3 名字规则、
  §14 测试说明；删掉 §13 的"后台命令"条目。
- `index.md`：工具表与事件中枢段的工具名。
- `plugins.md`：接口表清单、§20.2 成员语义与返回码、§20.4 kit 选项、§20.2 名字规则段。
- `tui.md`：Info 侧边栏「后台任务」段的取消入口指向 `agentxx_async_cancel`。
- `AGENTS.md`：模型侧工具族段、插件事件表段（`publish_mask` / `ERR_DENIED`），
  删掉 `execute_command` 的 `background:true` 说明。
- `roadmap.md`：新增 2026-11-06 条目，删掉 background:true 的表述；
  `resource/history/async-work` 里的"可选后续 background"一并删掉。

### 6. 验证

- 构建：`cmake --build agent\build\windows-debug --config Debug`（exit=0）。
- 目标模块：`event_tools` 97 / `event_hub` 244 / `event_delivery` 46 / `event_subagent` 52 /
  `async_tools` 86 / `plugin_event` 123 / `plugin_runtime` 736 / `plugin_cleanup` 99 /
  `async_work` 121 / `config_keys` 214 / `prompt_sources` 47 —— 全绿。
- `boundaries`：仅剩 `agent/third_party/cxx_pluginxx_ui` 里 5 处"载体"用词（既有问题）。
- 全量回归结果见 `index.md` 的结论段。
