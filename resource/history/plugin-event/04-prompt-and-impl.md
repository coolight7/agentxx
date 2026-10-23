# 提示词与配置归属 + 实现规划

> 关联: [index.md](index.md) · [01-analysis.md](01-analysis.md) · [02-runtime-design.md](02-runtime-design.md) · [03-event-driven.md](03-event-driven.md)

## 1. 两条路径，主次分明

插件要"换系统提示词、换工具、换插件，又不影响内置会话"，有两条路径，职责不同：

| 路径 | 能改什么 | 生效范围 | 本次定位 |
|---|---|---|---|
| **根配置**（`mount_root` 的 `config_json` + `mount_plugins`） | `AgentConfig` 全部可配置项：`prompt.systemPrompt`、`prompt.appendSystemPrompts`、`toolWhitelist`、`enableToolFiltering`、模型、语言、工作目录、各种开关；以及"该根挂哪些插件"（`mount_plugins`） | 该根的**全部会话** | **主路径，本次必做** |
| **会话级提示词来源**（`PromptSourceRegistry`） | 运行时段落（追加/替换基础段的文本） | 单个 `sessionId` | 可选，同一根下不同群微调时才用 |

为什么主路径是根配置：

- `AgentPrompt::systemPrompt` 默认是 coding 人设（文本在 `prompt.cpp`），且 `appendSystemPrompts` 与 `toolPrompt` 都挂在 `AgentConfig` 上。
- 工具集、插件挂载、中间件栈、执行图都绑定 `AgentContext`，会话级根本表达不了。
- 因此"QQ 机器人"必须是一个**自己的根 agent**，而不是内置 agent 的一个会话。

会话级来源仍然有用：同一个根下，A 群和 B 群可以有不同群规；但那属于"同一套工具下的措辞微调"，不是必需品。

## 2. 根配置（主路径）

### 2.1 字段

```json
{
  "agentName": "QQ群助手",
  "model": "gpt-4o",
  "language": "zh-cn",
  "workDir": "D:/work/qq",

  "toolWhitelist": ["agentxx_filesystem_read", "agentxx_filesystem_glob"],
  "enableToolFiltering": true,

  "prompt": {
    "systemPrompt": "你在 QQ 群里回答问题。回答简短，不超过三句话。",
    "append": { "qq-rules": "只回答与群主题相关的问题；不确定时说不确定。" },
    "toolPrompt": {
      "agentxx_filesystem_read": { "depict": "读取群文件", "args": { "path": "文件路径" } }
    }
  },

  "inherit": {
    "model": true,
    "language": true,
    "workDir": true,
    "permission": true,
    "prompt": false,
    "mcp": false,
    "skill": false,
    "memory": false
  },

  "enableSessionStore": true,
  "enableSubagent": false,
  "enableSummarization": true
}
```

| 字段 | 默认 | 说明 |
|---|---|---|
| `prompt.systemPrompt` | 不继承（`AgentPrompt` 默认 = coding） | 给了就整段替换；插件应显式给 |
| `prompt.append` | 空 | 按 key 写入 `appendSystemPrompts`，可带 `order` |
| `prompt.toolPrompt` | 不继承 | 覆盖工具描述（聊天场景可把"文件路径"改成更口语的说法） |
| `pluginMount` | **不在 `config_json` 里** | 由 `spec.mount_plugins` 单独给；空 = `none`（只有内置工具）；两处都给 → `ERR_ARG` |
| `toolWhitelist` + `enableToolFiltering` | 不继承 | 只保留需要的工具 |
| `model` / `availableModels` | 继承 | 根可以换模型 |
| `language` / `workDir` / 权限路径 | 继承（`inherit.*` 可关） | |
| `mcpServerUrls` / `skillDirPaths` / `memoryFilePaths` / `ragDocsPaths` | 清空 | `inherit.*` 打开才拷贝 |
| `agentName` | `"{宿主名}-{name}"` | |
| `enableSessionStore` | true | |
| `enableSubagent` | false | 对外应答的根通常不需要 |

### 2.2 根的插件挂载与递归切断

上一篇之后，插件 `create`/`start` 全程各只有一次，所以根不会是"第二个插件实例"。
它只是一个新 agent，它的工具来自**把已加载的插件挂到它身上**：

1. `spec.mount_plugins` 里的插件在这个根上各跑一次 `mount`（注册工具、应用清单资源），
   不重跑 `start`、不重复建连。
2. 默认 `none`：根只有内置工具（share_store / subagent / git_worktree）。要文件/命令工具
   必须显式列出（`listed`）或全挂（`all`）。插件自己也要用的话，开 `include_creator=true`
   并在 `mount` 里只注册工具。
3. 递归三层防护：建根只能在 `start`；`mount` 期间调 `mount_root` → `ERR_STATE`；
   创建者默认不挂到自己的根（`include_creator=false`）。
4. 漏配可自查：根的 `agentxx.agent.mount.list` 能看到实际挂了哪些；模型也会直接发现没工具。

### 2.3 配置校验与失败

- 派生完成后调用 `AgentConfig::validate()`；失败直接返回 `ERR_ARG`，日志给出具体字段。
- `mount_root` 返回 id 后 init 异步执行；init 失败时状态置 `Failed`，该根下挂起通道收到 `EVENT_ERROR` + `EVENT_CLOSED`。

## 3. 会话级提示词来源（可选）

### 3.1 现状与缺口

`PromptSourceRegistry` 只有 `Stable`（追加进 system）与 `Dynamic`（请求末尾），基础 `systemPrompt` 无法按会话替换；`agentxx.agent.prompt` 表也只有全局 get/set。

### 3.2 设计（后续阶段）

- `PromptSectionSlot` 增加 `Base`：替换基础 `systemPrompt`，配置附加段与运行时段保留。
- prompt 表尾追加 `register_source` / `unregister_source`（基线不变，`VERSION` 2→3，`MIN_VERSION` 不变，`ifaceHas` 判定；新成员同样带 `agent` 引用，见上一篇 §3）。
- 插件回调返回 `NO_OPINION` 表示"不是我的会话，保持宿主原提示"——因为 perSession 来源会被**所有**会话调用，包括内置 coding 会话。

```cpp
PromptSourceOptions opt;
opt.key = "plugin.qq.persona";
opt.slot = PromptSlot::Base;
opt.perSession = true;
registerPromptSource(ctx, opt, [&ctx](std::string_view ctxJson) -> std::optional<std::string> {
    auto sid = Json::parse(ctxJson).value("sessionId", "");
    if (!sid.starts_with("qq_group_")) {
        return std::nullopt;          // 不是我的会话：没意见
    }
    return ctx.personaFor(sid);
});
```

这一阶段不做也不影响 QQ 插件上线（人设已经在根配置里）；做它的价值是"一个根下多群不同群规"。

## 4. 实现规划

### 4.1 阶段

```
阶段一  统一运行时 AgentSessionRuntime（FFI 先接；CLI 第二步）
   ▼
阶段二  AgentHost 多根（内置根 + 挂载根）
   ▼
阶段三  运行时接口表（mount_root / open_channel / 事件 / 中断）
   ▼
阶段四  事件驱动（触发器 + 定时器）
   ▼
阶段五  会话级提示词来源（可选）
   ▼
阶段六  文档与示例插件
```

### 4.2 阶段一：`AgentSessionRuntime`

| 文件 | 动作 |
|---|---|
| `agent/lib/include/agentxx/agent/session_runtime.h` | 新增（02 §3） |
| `agent/lib/src/agent/session_runtime.cpp` | 新增：装配、启动顺序、关闭顺序、控制命令 |
| `agent/lib/include/agentxx/agent/io/agent_io.h` | 新增空实现钩子 `onRuntimeStopping()` |
| `agent/lib/include/agentxx/agent/io/user_input_json.h` + `.cpp` | 新增：`parseUserInputJson`（从 FFI 提取） |
| `agent/lib/include/agentxx/agent/io/embedded_client_agent_io.h` + `agent/lib/src/agent/io/embedded_client_agent_io.cpp` | 新增：从 `FfiClientAgentIO` 提炼事件汇基类 |
| `agent/lib/src/ffi/ffi_client_io.h/.cpp` | 改为继承 `EmbeddedClientAgentIO` |
| `agent/lib/src/ffi/ffi_runtime.cpp` | 改用 `AgentSessionRuntime` + `parseUserInputJson` |
| `agent/client/src/mode_runners.cpp` | 第二步：改用 runtime（验证通过后） |

验收：

- FFI 现有测试全绿（事件、payload、中断、合批）。
- `parseUserInputJson` 8 类边界单测。
- CLI 本地 direct / 远程 WS 行为不变。

### 4.3 阶段二：AgentHost 多根

| 文件 | 动作 |
|---|---|
| `agent/lib/include/agentxx/agent/agent_host.h` | `MountedRoot`、`MountSpec`（含 `pluginMountPolicy` / `pluginMounts` / `includeCreator`）、`mountRoot/unmountRoot/unmountRootsByOwner/findRoot/roots()`；`maxRoots`；`claimSession/releaseSession` |
| `agent/lib/src/agent/agent_host.cpp` | 每根注册子代理服务（闭包捕获该根 ctx）；`spawnOneTask` 改用发起方 ctx；`publishProgress` 走 hostBus；`destroyAgent` 分支；**`mountRoot` 把 `pluginHost_` 强注入到新根**（不走 `createSelfHosted`） |
| `agent/lib/src/agent/agent_host.cpp`（AgentRegistry） | 重复 id 拒绝（现在 `insertOrAssign` 会覆盖） |
| `agent/test/.../test_agent_host*` | 新增/扩展用例 |

验收：

| 编号 | 用例 |
|---|---|
| 1 | 挂两个根 + 内置根：registry 三个节点，`parentAgentId` 均为空、`depth=0` |
| 2 | 挂载根里的 `agentxx_subagent` 委派：子节点父 id 指向该根 |
| 3 | 用量记账：挂载根的子代理用量落在挂载根会话上 |
| 4 | `unmountRoot` 后节点消失、服务不可用、`BaseAgent` 释放（`weak_ptr::expired`） |
| 5 | 超 `maxRoots` 拒绝 |
| 6 | 现有子代理测试全绿 |
| 7 | 根的 `pluginMountPolicy=None`（默认）：根只有内置工具；`listed` 后列出的插件工具在该根可见 |
| 8 | 同存储根 + 同名 sessionId 两个根 → 第二个 `claimSession` 失败；不同存储根 + 同名 → 都成功 |

### 4.4 阶段三：运行时表

| 文件 | 动作 |
|---|---|
| `agent/lib/include/agentxx/plugin/api/plugin_api.h` | 表 + spec + 事件（02 §4） |
| `agent/lib/include/agentxx/plugin/api/plugin_iface_meta.h` | `IfaceMeta<AgentxxPluginRuntimeIface>` |
| `agent/lib/include/agentxx/plugin/api/plugin_kit.h` | `AgentIfaces.runtime` + SDK（02 §10） |
| `agent/lib/include/agentxx/plugin/plugin_manager.h` | `PluginInstance::runtimeRoots/runtimeChannels/runtimeTriggers` |
| `agent/lib/include/agentxx/plugin/runtime_state.h` | 新增：`PluginRuntimeState` |
| `agent/lib/src/plugins/plugin_manager_runtime.cpp` | 新增：挂根、开/关通道、输入、查询、事件汇；`mount_root` 里拒绝 `isMounting()` |
| `agent/lib/src/plugins/plugin_manager_runtime_config.cpp` | 新增：`config_json` → `AgentConfig` 派生（§2）+ `mount_plugins` → `MountSpec.pluginMountPolicy/pluginMounts` |
| `agent/lib/src/plugins/plugin_manager_vtable.cpp` | 16 个入口 + `query_interface` 分支 |
| `agent/lib/src/plugins/plugin_manager_lifecycle.cpp` | `detachDomainRegistrations` 摘运行时；`releaseInstanceResources` 兜底 |
| `agent/test/core/test_boundaries.cpp` | 表数量 21 → 22；新表 `IfaceMeta` 与成员顺序台账 |

验收（新模块 `plugin_agent_runtime`，`plugin_runtime` 已被现有测试占用）：

| 编号 | 用例 |
|---|---|
| 1 | `mount_root` → `open_channel` → `EVENT_READY`；`list_runtime` 正确 |
| 2 | `send_input` → `TURN_START` → `DELTA_TEXT` → `TURN_END.text` |
| 3 | 根配置生效：自定义 `systemPrompt` 出现在该根的会话里，内置会话不受影响 |
| 4 | 根配置生效：`toolWhitelist` 过滤后模型只看到白名单工具 |
| 5 | 空文本且无附件 → `ERR_ARG`；同一 `sessionId` 两个通道 → `ERR_CONFLICT` |
| 6 | `close_channel` → `EVENT_CLOSED`；`unmount_root` 连带关闭其通道 |
| 7 | 插件禁用 → 根与通道回收；重新启用后可再挂 |
| 8 | 超 `max_roots` / `max_channels_per_root` → `ERR_BUDGET` |
| 9 | 中断应答后轮次继续；超时收到 `INTERRUPT_EXPIRED` |
| 10 | 两个根并发运行互不串话；根内两个通道互不串话 |
| 11 | 在 `mount` 回调里 `mount_root` → `ERR_STATE`；`include_creator=false` 时创建者不挂到自己的根上 |
| 12 | 配置非法（缺模型/负值）→ `ERR_ARG` + 可读原因 |

### 4.5 阶段四：事件驱动

| 文件 | 动作 |
|---|---|
| `agent/lib/src/plugins/plugin_manager_runtime_trigger.cpp` | 订阅、合并窗口、防抖、丢弃、背压 |
| `agent/lib/src/plugins/plugin_manager_runtime_timer.cpp` | 一次性/周期定时器 |

验收（`plugin_agent_trigger`）：QUEUE/COALESCE/DROP_IF_BUSY、窗口内外、定时器语义、8 KB 提前投递、插件禁用撤销、载荷四种形态。

### 4.6 阶段五：会话级提示词来源（可选）

| 文件 | 动作 |
|---|---|
| `agent/lib/include/agentxx/agent/prompt_sources.h` + `.cpp` | `PromptSectionSlot::Base` 与 `baseSection(sessionId)` |
| `agent/lib/src/agent/context.cpp` | `buildSystemPromptStable` 使用替换后的基础段 |
| `plugin_api.h` / `plugin_manager_vtable.cpp` / `plugin_kit.h` | prompt 表 v2（`register_source/unregister_source`） |

验收：Base 段替换、`NO_OPINION` 保持原提示、多来源取第一个、按 owner 撤销、变量替换、刷新时机、内置会话不受影响。

### 4.7 阶段六：文档与示例

| 对象 | 内容 |
|---|---|
| `docs/zh-cn/design/plugins.md` | 新表、多根、通道、触发器；表数量 21 → 22 |
| `docs/zh-cn/design/index.md` | `AgentSessionRuntime`、AgentHost 多根语义与预算 |
| `docs/zh-cn/design/ffi.md` | FFI 改用公共 runtime（接口不变） |
| `agent/plugins/AGENTS.md` | 回调线程、禁用清理、根配置字段、递归限制 |
| `AGENTS.md` | 表数量与"插件可挂根驱动会话"的要点 |
| `agent/plugins/example_runtime/` | 最小示例：一个根（自定义提示词+工具白名单）+ 一个通道 + 一个 COALESCE 触发器 + 一个定时器 |

## 5. 风险与回滚

| 风险 | 影响 | 缓解 | 回滚 |
|---|---|---|---|
| `AgentSessionRuntime` 改动 FFI/CLI 启动 | 会话起不来 | FFI 先接、CLI 后接；保留原路径直到验证 | 阶段一单独回退 |
| AgentHost 多根改动子代理 | 内置功能回归 | 先做阶段二并跑全部子代理测试；`attachRoot` 语义不变 | 阶段二单独回退 |
| 多根时的 init 成本 | 启动慢、内存高 | 默认只挂一个根；`maxRoots` 上限；根只做"插件挂载"，不做 `dlopen`/`create`/`start` | 插件配置调整 |
| 在 `mount` 里建根 | 无限嵌套 | ① 建根只在 `start`；② `mount` 期间 `mount_root` 返 `ERR_STATE`；③ `include_creator=false` + 测试用例 | — |
| 根忘记列 `mount_plugins` | 根只有内置工具 | 示例给全；`list_runtime` 输出每根的挂载摘要 | 插件配置调整 |
| 通道泄漏 | 内存/协程泄漏 | 关闭集中一个函数；ASan 下反复开关 100 次 | 关闭逻辑集中 |
| 回调阻塞 | 所有会话卡住 | 文档禁止；调试构建对超时回调告警 | 调插件 |
| prompt 表 v2 兼容 | 老插件读新表头 | 只追加成员，基线不变；老插件不调用新成员 | 无影响 |

## 6. 工作量（估算）

| 阶段 | 文件 | 代码量（量级） |
|---|---|---|
| 一 | 8 | +900 / −500（多为搬迁） |
| 二 | 4 | +450 / −180 |
| 三 | 10 | +1800 |
| 四 | 3 | +500 |
| 五 | 7 | +600（可选） |
| 六 | 文档 + 示例 | — |

## 7. 开工前检查

- [ ] QQ/微信插件的群数量级（决定"一个根 64 通道"是否够）
- [ ] 是否需要在同一插件内挂多个根（客服/运维分人设）——影响 `max_roots` 默认值
- [x] 挂载根的插件来源：**默认 `none`，创建者用 `mount_plugins` 显式列出**；创建者默认不挂到自己的根（`include_creator=false`），递归另有两道（`start`-only + `mount` 期拒绝）
- [ ] 群会话是否都持久化（建议默认开启）
- [ ] 会话级提示词来源是否本期做（不做也能上线，人设在根配置里）
