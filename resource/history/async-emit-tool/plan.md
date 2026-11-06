# 设计方案: agentxx_async_emit (事件发布工具与发布权限)

## 1. 背景

上一轮把后台任务收敛成 `agentxx_async_start/_query/_await/_cancel` 之后，事件工具
`agentxx_event` 只剩发布侧（`opt=emit` / `opt=end`），但仍有几个问题：

- 名字与 `agentxx_async_*` 不成体系；
- `opt` 是一个只有两个取值、且几乎总是配 `value` 的参数，模型容易传 `opt="start"` 之类；
- 名字被写死只能 `agentxx.agent.event.*`：模型没法给插件的事件发值，而插件也没法
  指定"允许别人发"；
- 返回文本是 `[Event emit] event=… state=… published=… waits=…`，把参数又念一遍，
  真正有用的信息（这次投递给了谁、还剩几个等待）反而没有。

## 2. 现状（调查）

| 位置 | 现状 |
|---|---|
| `agent/lib/include/agentxx/tools/event.h`、`src/tools/event.cpp` | `EventTool`，参数 `opt`/`event`/`value`/`summary`/`single`/`create`/`label`/`remove`，`owner = "tool"` |
| `agent/lib/include/agentxx/event/hub.h` | `EventDefineSpec`（name/kind/owner/description/originSession/historyLimit/ttl/linger）、`EventRecord` 快照、`publishValue` 返回 bool、`endEvent(name, summary, err)` 无发布者身份 |
| `agent/lib/src/event/hub.cpp` | `publishValue` → `ensureRecord`（不存在就建，归属按 `ownerOfName` 推断）→ `dispatchValue`（只投递，不回报投给了谁）；`endEvent` 无权限检查 |
| `agent/lib/src/event/hub.cpp:183` | 总线桥用空 owner 发布 `plugin.*` 主题（归属按名字推断） |
| 内置发布方 | `base_agent.cpp` 的 `publishHubValueIfWanted`（`core:turn` / `tool` / `subagent` / `timer`）、`toolcall.cpp`（`tool`）、`async_work.cpp`（`async_work`）、`hub.cpp` 定时源（`timer`） |
| 插件面 | `AgentxxPluginEventSpec`（struct_size/kind/history_limit/_reserved/ttl_ms/linger_ms/name/description）、`define_event` 幂等、`emit_value` 走 `postValue`（不回报拒绝原因） |
| 提示词 | `prompt.cpp` 的 `agentxx_event` 条目（2 个 opt、7 个参数） |
| 测试 | `event_tools`（58 断言，覆盖 emit/end/single/remove/失败形态）、`event_hub`（244）、`plugin_event`（108） |

## 3. 方案

### 3.1 中枢：发布权限

```cpp
enum class PublishMask : uint32_t {
    None    = 0,        // 只有创建者
    Model   = 1u << 0,  // 模型侧工具调用（归属 `tool`）
    Plugin  = 1u << 1,  // 插件（归属 `plugin:<插件名>`）
    Builtin = 1u << 2,  // 内置源（轮次/工具结束/定时器/后台任务）
    Any     = Model | Plugin | Builtin,
};
```

- `EventDefineSpec.publishMask`（默认 `None`）随定义写进 `EventRecord.publishMask`；
  重复定义时**只升不降**（后来者不能把别人的权限收窄，也不能自己偷偷放宽 —— 见风险 4）。
- 角色判定 `publishRoleOf(owner)`：`plugin:` 前缀 → Plugin；`tool` → Model；其余 → Builtin。
- 允许发布的条件：① 发布者归属与记录 owner 相同；② 记录 `publishMask` 含对应位。
- **隐式创建**（记录不存在时的第一次发布）收紧：
  - Model：只能建 `agentxx.agent.event.*`，且不含保留段（`turn.`/`tool.`/`subagent.`/`timer.`/`host.`）；
  - Plugin：只能建 `plugin.*`（沿用"框架不补前缀、只校验族"的规则）；
  - Builtin：可以建 `agentxx.agent.event.*`（含保留段）与 `agentxx.agent.async.*`。
- `endEvent` / `removeEvent` 用同一套判定（收尾别人的流也要有权限）。

### 3.2 中枢：投递统计

- `dispatchValue` 返回本次值实际投递到的等待（`waitId` + `delivery`）；
- 新增 `publishValueEx(name, value, owner) -> std::expected<PublishOutcome, std::string>`：
  ```
  PublishOutcome { created, endedNow, published, delivered, remainingWaits, deliveries[] }
  ```
  `bool publishValue(...)` 保留为薄包装（老调用方与测试不用改）；
- 新增 `endEventEx(name, summary, owner) -> std::expected<EndOutcome, std::string>`：
  ```
  EndOutcome { alreadyEnded, published, closedWaits }
  ```
  `bool endEvent(...)` 保留，改为带 owner 的重载（`endEvent(name, summary, owner, err)`）。

### 3.3 工具：`agentxx_async_emit`

参数（6 个）：

| 参数 | 说明 |
|---|---|
| `event` | 必填，任意合法事件名 |
| `value` | 可选，要发布的值（任意 JSON，字符串按纯文本） |
| `end` | 可选，`true` = 发布后收尾（不带 `value` 时只收尾） |
| `summary` | 可选，仅 `end:true` 时有效，收尾信息 |
| `allow_publish` | 可选，仅**新建**事件时有效：`["model","plugin","builtin","any"]`，缺省只有创建者能发 |
| `label` | 可选，仅新建事件时有效，一行说明 |

语义：

- `value` 与 `end` 至少给一个，两个都不给按参数错误；
- 事件存在：按其 `publishMask` 判定能否发布/收尾，`allow_publish` 与 `label` 忽略
  （返回文本里点出"沿用已有定义"）；
- 事件不存在且给了 `value`：按流式事件创建（归属 `tool`），`allow_publish`/`label` 生效；
- 事件不存在且只给 `end`：按参数错误（没有可收尾的流）。

返回文本（短句、只给新信息）：

- 正常：`1 value published; delivered to 2 waiters (w-3 next-step, w-7 none); 1 waiter still attached`
- 无人等：`1 value published; no waiter attached (kept in history for replay)`
- 新建：前缀 `created a new stream event (publish allowed to: model, plugin)`
- 收尾：`stream closed; 2 waiter(s) finished without a value`

### 3.4 插件面

- `AgentxxPluginEventSpec` 尾部追加 `uint32_t publish_mask`（新宏
  `AGENTXX_PLUGIN_EVENT_PUBLISH_MODEL/PLUGIN/BUILTIN`，0 = 只有本插件）；
  老主程序按 `struct_size` 读不到尾部 → 行为与旧版一致。
- 新增返回码 `AGENTXX_PLUGIN_EVENT_ERR_DENIED (-7)`：`emit_value` / `end_event` /
  `remove_event` 被权限拒绝时返回（`emit_value` 先用 `callSync` 同步问一次再排队发，
  避免"接受了但静默丢掉"）。
- `plugin_kit.h` 的 `EventDefineOptions` 增加 `uint32_t allowPublish = 0`，`defineEvent` 写进尾部字段。
- 查询/列表 JSON 增加 `"publishTo": ["model","plugin"]`（空数组 = 只有创建者）。

### 3.5 文档与清理

- 删除 `execute_command` 的 `background:true` 全部表述（`events.md` §13、`AGENTS.md` 两处、
  `roadmap.md`、任务记录），因为它从未实现也不打算实现，留着只是噪音。
- 更新 `events.md` 的工具族表与样例、`index.md` 工具表、`plugins.md` §20、
  `AGENTS.md` 的事件中枢段、`roadmap.md`。

## 4. 数据兼容

- 事件记录是内存态（无落库），`PublishMask` 不需要迁移；重启后由各自的定义重建。
- 插件结构体只在尾部追加，`struct_size` 判定；老插件（不填 `publish_mask`）= 0 = 只有自己能发，
  与老行为（只有自己在发）一致。
- 老模型提示词里出现的 `agentxx_event` 会被新工具名取代；工具表里不再有 `agentxx_event`。

## 5. 分步实施

1. `hub.h` / `hub.cpp`：`PublishMask`、`publishRoleOf`、`mayPublish` / `mayCreate`、
   `publishValueEx` / `endEventEx`、`dispatchValue` 返回投递列表、记录 JSON 增加 `publishTo`。
2. 插件面：`plugin_api.h` 结构体尾部字段 + 宏 + 拒绝码；`plugin_kit.h` 选项；
   `plugin_manager_vtable.cpp` 的 define/emit/end/remove。
3. 工具：`tools/async_emit.{h,cpp}`（替换 `tools/event.{h,cpp}`）、`base_agent.cpp` 注册、
   `prompt.cpp` 条目。
4. 测试：重写 `event_tools`；`event_hub` 增加权限用例；`plugin_event` 增加"别家事件被拒"。
5. 文档与历史记录；删 `background` 表述。
6. 构建 + 目标模块 + 全量回归。

## 6. 风险

1. **收紧隐式创建**：模型不能再往 `plugin.*` 建事件（之前也不行），行为不变；
   但测试里"发布即创建"的写法要确认归属推断一致（`ownerOfName` 先算发布者再判定）。
2. **总线桥**：`plugin.*` 主题的值为空 owner，需在桥里先推断归属再发布，否则会被判成 Builtin 而拒绝。
3. **内置源**：`wants()` 门控不变；`publishHubValueIfWanted` 传的 owner 与记录归属一致，不受影响。
4. **权限只升不降**：重复定义同种事件时，`publishMask` 取并集（收窄会打断已在用的发布方）。
5. **`endEvent` 加 owner**：四处调用（工具/插件/测试/hub 内部 single 自动收尾）都要传对身份，
   内部自动收尾用记录自己的 owner。
