# 插件内驱动 agent 会话：实施记录

> 设计来源：[index.md](index.md) · [01-analysis.md](01-analysis.md) · [02-runtime-design.md](02-runtime-design.md) · [03-event-driven.md](03-event-driven.md) · [04-prompt-and-impl.md](04-prompt-and-impl.md)
> 前置：`../plugin-multi-agentnode/`（进程一份插件宿主 + 每 agent 挂载 + agent 引用，已完成）

## 状态总览

| 阶段 | 内容 | 状态 |
|---|---|---|
| 一 | 统一启动壳 `AgentSessionRuntime`（FFI 先接，CLI 第二步） + `parseUserInputJson` + `EmbeddedClientAgentIO` | **已完成** |
| 二 | `AgentHost` 多根（内置根 + 挂载根）+ 会话占用表 | **已完成** |
| 三 | 运行时接口表 `agentxx.agent.runtime`（挂根 / 通道 / 输入 / 事件 / 中断） | **已完成** |
| 四 | 事件驱动：触发器（QUEUE/COALESCE/DROP_IF_BUSY） + 定时器 | **已完成** |
| ~~五~~ | ~~会话级提示词来源~~ | **不做**: 按 agent 级支持修改提示词即可（一个 agent 管理的会话、提示词、插件保持一致），根配置已覆盖 |
| 六 | 文档与示例插件 `agent/plugins/example_runtime` | **已完成** |

## 阶段一：统一启动壳（已完成）

| 文件 | 动作 | 状态 |
|---|---|---|
| `agent/lib/include/agentxx/agent/io/user_input_json.h` + `agent/lib/src/agent/io/user_input_json.cpp` | 新增：输入 JSON 解析（从 FFI 提取；`std::expected` + `UserInputError{badJson, message}` 区分语法错误与参数错误） | 完成 |
| `agent/lib/include/agentxx/agent/io/embedded_client_agent_io.h` + `agent/lib/src/agent/io/embedded_client_agent_io.cpp` | 新增：嵌入式 client 端点基类（中断登记/等待/过期/失败清理、增量合批、线程判定、`onRuntimeStopping`） | 完成 |
| `agent/lib/include/agentxx/agent/io/agent_io.h` | 新增空实现钩子 `onRuntimeStopping()`（关闭流程开始时通知客户端端点） | 完成 |
| `agent/lib/include/agentxx/agent/session_runtime.h` + `agent/lib/src/agent/session_runtime.cpp` | 新增：装配（makePair/setTransport/建服务端点）、启动顺序（两侧接收循环 → init → 登记根 → 就绪 → 驱动循环 → hello）、关闭顺序、控制命令 | 完成 |
| `agent/lib/src/ffi/ffi_client_io.{h,cpp}` | 改为继承 `EmbeddedClientAgentIO`，只保留事件转 C 回调与 FFI 专有消息 | 完成 |
| `agent/lib/src/ffi/ffi_runtime.{h,cpp}` | 改用 `AgentSessionRuntime` + `parseUserInputJson`；FFI 用 `loadPluginsInHost=false` 保持“主程序不代劳加载插件” | 完成 |
| `agent/client/src/mode_runners.cpp` | 本地直连改用 runtime（约 130 行重复装配删除），差异只剩端点类型/尾窗/语言/首个查询 | 完成 |
| `agent/test/core/test_session_runtime.cpp` + `agent/test/include/agentxx-test/core/test_session_runtime.h` | 新增模块 `session_runtime`（55 条断言: 解析 8 类边界、装配拒绝、幂等 start、init 失败上报、skipInit 下一轮完整对话） | 完成 |

验收结果（Debug）：

| 模块 | 结果 |
|---|---|
| `session_runtime` | passed=55 failed=0 |
| `ffi_c_api` | passed=208 failed=0 |
| `boundaries` / `config_keys` | 19 / 178 全绿（新增文件用词符合规则 15） |
| `input_delivery` / `agent` / `agent_host` / `subagent_bus` / `subagent_tool` | 全绿 |
| `agentxx_cli`（本地直连 TUI/CLI） | 编译链接通过，行为不变（装配顺序与原实现一致） |

## 阶段二：AgentHost 多根

| 文件 | 动作 | 状态 |
|---|---|---|
| `agent/lib/include/agentxx/agent/agent_host.h` | `MountSpec` / `MountedRoot` / `mountRoot` / `unmountRoot` / `unmountRootsByOwner` / `findRoot` / `roots` / `maxRoots` / `claimSession` / `releaseSession` | 未开始 |
| `agent/lib/src/agent/agent_host.cpp` | 每根一个子代理服务（闭包捕获该根 ctx）；`spawnOneTask` 用发起方 ctx；`publishProgress` 走 hostBus；`AgentRegistry` 重复 id 拒绝；`mountRoot` 强注入 `pluginHost_` | 未开始 |
| `agent/test/core/test_agent_host.cpp` | 多根用例（节点/委派/记账/卸载/预算/存储根） | 未开始 |

验收（04 §4.3 的 8 条）：三根节点并存、挂载根委派、用量记账、卸载后释放、超预算拒绝、现有子代理测试全绿、`pluginMountPolicy` 生效、`(存储根, sessionId)` 冲突。

## 阶段二：AgentHost 多根（已完成）

| 文件 | 动作 | 状态 |
|---|---|---|
| `agent/lib/include/agentxx/agent/agent_host.h` | 新增 `MountedRoot` / `MountSpec`（agentId/owner/primary/registerSubagentService）、`mountRoot` / `unmountRoot` / `unmountRootsByOwner` / `findRoot` / `roots()` / `rootCount()`、`Config::maxRoots`（默认 8）、`claimSession` / `releaseSession` / `sessionOccupancyJson`；`injectShared` / `registerSubagentService` / `unregisterSubagentService` 私有辅助 | 完成 |
| `agent/lib/src/agent/agent_host.cpp` | `attachRoot` 转调 `mountRoot({agentId="root", primary=true})`；每根一个子代理服务（闭包捕获该根上下文，`spawnBatch(req, rootCtx)`）；`spawnOneTask` 的父配置/工具继承/用量记账/share_store/HIL 全改用发起方上下文；`publishProgress` 改走 hostBus 直接 co_spawn；`AgentRegistry::insert` 重复 id 拒绝；`destroyAgent`/`destroyAgentAsync` 一并摘根与子代理服务；会话占用表（`存储根 + sessionId`，带锁，跨插件实例） | 完成 |
| `agent/test/core/test_agent_host.cpp` | 新增 `test_host_multi_root`（三根并存/节点结构/重复 id/超预算/委派/卸载/归属者批量卸载）与 `test_host_session_claim`（同根同名互斥、不同根同名共存、非占用者不能释放） | 完成 |

验收结果（04 §4.3 的 8 条）：

| 编号 | 用例 | 结果 |
|---|---|---|
| 1 | 内置根 + 两个挂载根：`roots().size()==3`，节点 `parentAgentId` 空 / `depth=0` | 通过 |
| 2 | 挂载根里的子代理委派：节点父 id 指向该根，结果正常回收 | 通过 |
| 3 | 用量记账：`spawnOneTask` 用发起方上下文（`usageAccountSessions` 取该根的 sessions） | 通过 |
| 4 | `unmountRoot` 后节点与服务消失 | 通过 |
| 5 | 超 `maxRoots` 拒绝（无节点/无根表残留） | 通过 |
| 6 | 现有子代理测试全绿（`subagent_bus` 21、`subagent_tool` 143、`agent` 199、`summarization` 454） | 通过 |
| 7 | `pluginMountPolicy` 生效（`injectShared` 按策略注管理器；None 不注） | 通过（插件挂载组全绿） |
| 8 | `claimSession` 同存储根同名失败 / 不同存储根同名成功 / 非占用者不能释放 | 通过 |

回归：`plugin_host_mount` 146 / `plugin_multi_mount` 47 / `plugin_multi_instance` 80 / `plugin_cleanup` 99 / `plugin_runtime` 736 / `boundaries` 19 / `session_runtime` 55，全部 failed=0。

## 阶段三：运行时接口表（已完成）

| 文件 | 动作 | 状态 |
|---|---|---|
| `agent/lib/include/agentxx/plugin/api/plugin_api.h` | `agentxx.agent.runtime`（v1/min1/基线 sizeof）+ 8 个错误码 + 16 类通道事件 + 4 个 spec + 3 个策略常量 + 16 个入口 | 完成 |
| `agent/lib/include/agentxx/plugin/api/plugin_iface_meta.h` | `IfaceMeta<AgentxxPluginRuntimeIface>` + 两条 static_assert | 完成 |
| `agent/lib/include/agentxx/plugin/api/plugin_spec.h` | 4 个 spec 的 `AGENTXX_DECLARE_SPEC_BASELINE` | 完成 |
| `agent/lib/include/agentxx/plugin/runtime_state.h` | 新增：`RuntimeRoot`/`RuntimeChannel`/`RuntimeTrigger`/`RuntimeTimer` + 预算常量；`RuntimeRoot::sessionKey()` | 完成 |
| `agent/lib/src/plugins/plugin_manager_runtime.cpp` | 挂根 / 卸载根 / 开通道 / 启动通道 / 排队冲刷 / 关通道 / 输入与控制 / 查询 / 按归属回收 | 完成 |
| `agent/lib/src/plugins/plugin_manager_runtime_config.cpp` | `config_json` → `AgentConfig` 派生（继承开关 + 会话目录 `{宿主根}/{rootId}`）+ `mount_plugins` 解析 | 完成 |
| `agent/lib/src/plugins/plugin_channel_agent_io.{h,cpp}` | 通道端点：wire 事件 → 通道事件（正文/思考/工具/生成阶段/提示/中断/回执/队列）+ 60ms 正文合批 | 完成 |
| `agent/lib/src/plugins/plugin_manager_runtime_trigger.cpp` | 触发器（QUEUE/COALESCE/DROP_IF_BUSY）+ 定时器（一次性/周期）+ 背压上限 | 完成 |
| `agent/lib/src/plugins/plugin_manager_vtable.cpp` | 16 个 `xx_runtime_*` 入口 + `query_interface` 分支（字符串视图拷成拥有型再投递） | 完成 |
| `agent/lib/src/plugins/plugin_manager.h` | `kInterfaceTableCount = 10 + 12`；运行时段开放；`mountingDepth_`；前向声明 | 完成 |
| `agent/lib/src/plugins/plugin_manager_lifecycle.cpp` | `mountAgentAsync` 里加 `MountDepthGuard`；禁用/卸载调 `releaseRuntimeOfOwner` | 完成 |
| `agent/lib/src/agent/agent_host.cpp` | `createAgent` 把新建根的 `ioCtx` 换成容器这一条（否则根的 IO 线程没人 run）并重建总线 | 完成 |
| `agent/test/core/test_boundaries.cpp` | 表数量 21 → 22 + 成员顺序台账 + 两处 Group expect 改 12 | 完成 |
| `agent/test/plugin/test_plugin_agent_runtime.cpp` + 头 | 新增模块 `plugin_agent_runtime`（177 条断言） | 完成 |

### 实现中发现并修掉的问题

| 问题 | 原因 | 处理 |
|---|---|---|
| 根部会话卡死（READY 等不到） | `BaseAgent` 自建 `ioCtx`，容器只 run 自己那条；根的驱动循环没人推进 | `AgentHost::createAgent` 把根的 `ioCtx` 换成容器这一条并重建总线 |
| `mount_root` 返回 OK 但 `out_root_id` 空 | vtable 入口用 `onInstanceIo`，没把 id 写回主程序堆字符串 | 改为 `ioCallSyncKeep` + `hostMemorySetString` |
| 插件收不到 `EVENT_CLOSED` | `runtimeCloseChannelNow` 先把 `closed = true` 再交付，交付处对已关闭通道直接丢弃 | 先交付再置位 |
| 通道没有正文增量（`DELTA_TEXT` 恒 0） | 合批关闭时 `TextToken` 落到 `emitDeltaEvent` 的 `default` 分支 | `emitDeltaEvent` 增加 `TextToken`/`ThinkToken` 分支；通道打开 60ms 合批 |
| 触发器投递不生效 | `runtimeSendInput` 要求 `inst` 非空，内部投递传的是 `nullptr` | `inst` 为空视为内部投递（不做归属校验） |
| 内存会话下不同根同名被误判冲突 | 无会话存储时 `storeRoot` 空，占用键退化成全局同名 | `RuntimeRoot::sessionKey()`：无存储时用根 id |
| ASan heap-use-after-free（通道关闭后回调） | `notifyReady/notifyError/notifyClosed` 投递的闭包只捕 `this` | 改捕 `shared_from_this()` |

### 验收（`plugin_agent_runtime`，177 条断言全绿）

| 编号 | 用例 | 结果 |
|---|---|---|
| 1 | 表头版本/基线门槛；16 个入口存在 | 通过 |
| 2 | 没有容器时 `mount_root` = `ERR_UNSUPPORTED` | 通过 |
| 3 | 配置非法（非 JSON / 模型不存在 / 混入 `pluginMount` / `mount_plugins` 取值非法）逐条 `ERR_ARG`，容器不留根 | 通过 |
| 4 | 挂根 → 开通道 → `READY`（payload 带 rootId/sessionId） | 通过 |
| 5 | 根的提示词/工具白名单生效，内置会话配置一字未动 | 通过 |
| 6 | 同 sessionId 第二通道 `ERR_CONFLICT`；换 id 可开；缺 `on_event`/未知根分别 `ERR_ARG`/`ERR_NOT_FOUND` | 通过 |
| 7 | 输入 → `TURN_START`/`DELTA_TEXT`/`TURN_END`/`INPUT_ACK`；四类参数错误 | 通过 |
| 8 | `list_runtime` / `get_channel_state` / `get_context_messages` 内容正确，缺通道返回失败 | 通过 |
| 9 | 两根两通道事件互不串，各自提示词独立 | 通过 |
| 10 | 超 `maxRoots` = `ERR_BUDGET` | 通过 |
| 11 | 触发器 QUEUE / COALESCE（三条并一条）/ DROP_IF_BUSY（忙时丢弃 + 计数）；注销后不再投递 | 通过 |
| 12 | 定时器一次性（触发后自动注销）/ 周期（多次触发，注销即停）；四类参数错误 | 通过 |
| 13 | 关通道：`CLOSED` 恰一条、连带注销其触发器与定时器、会话占用释放可重开 | 通过 |
| 14 | 卸载根：其通道全部关闭、根从容器消失、重复卸载 `ERR_NOT_FOUND` | 通过 |
| 15 | 禁用插件：通道收到 `CLOSED`、根被回收、运行时清单清零 | 通过 |

## 阶段四：事件驱动（已完成，随阶段三一起落地）

| 文件 | 动作 | 状态 |
|---|---|---|
| `agent/lib/src/plugins/plugin_manager_runtime_trigger.cpp` | 触发器三种策略 + 背压（64 条 / 8 KB）+ 定时器（一次性/周期，通道关闭即注销） | 完成 |
| `agent/test/plugin/test_plugin_agent_runtime.cpp`（第 11、12 组） | 策略、合并窗口、忙时丢弃计数、一次性/周期定时器、参数校验 | 完成 |

## 阶段五：会话级提示词来源（不做）

原设计经 `PromptSourceRegistry` 加 `PromptSectionSlot::Base`，让它按 `sessionId` 替换基础系统提示词。
**本期不做**：需求改为「按 **agent 级**支持修改提示词即可」—— 一个 agent 管理的会话、提示词、插件等大部分内容保持一致，这已经由 `mount_root` 的 `config_json`
（`prompt.systemPrompt` / `prompt.append` / `prompt.toolPrompt`）+ `mount_plugins` 完全覆盖。

同根下不同会话要不同群规这类需求，可用「一个群一个根」实现（每根一份独立配置），
代价是每根一次 init（见设计文档 §11 成本对照）；群里数量大时更推荐共用一个根。

## 阶段六：文档与示例插件（已完成）

| 文件 | 动作 | 状态 |
|---|---|---|
| `agent/lib/include/agentxx/plugin/api/plugin_kit.h` | `AgentIfaces.runtime` + SDK 便捷层（`MountRootOptions` / `ChannelOptions` / `ChannelEvent` / `TriggerOptions` / `RuntimeTimerOptions` + `mountRoot`/`openChannel`/`closeChannel`/`sendInput`/`sendText`/`cancelChannel`/`selectChannelModel`/`respondInterrupt`/`channelStateJson`/`channelContextMessages`/`listRuntimeJson`/`addTrigger`/`removeTrigger`/`emitEvent`/`addRuntimeTimer`/`removeRuntimeTimer`）；通道回调转接表（`PluginBase::runtimeChannelShims`，回调与关通道重入安全） | 完成 |
| `agent/plugins/example_runtime/{example_runtime.cpp,plugin.yaml,CMakeLists.txt}` | 新增示例插件：`start` 里挂根（自定义提示词 + 工具白名单）→ 开通道 → 配 COALESCE 触发器 → 可选定时器；`mount` 里注册 `example_runtime_say/_ping/_status` 三个工具 | 完成 |
| `agent/plugins/CMakeLists.txt` | 注册 `example_runtime` | 完成 |
| `agent/test/plugin/test_plugin_example_runtime.cpp` + 头 | 新增模块 `plugin_example_runtime`（49 条断言） | 完成 |
| `agent/test/test.cpp` | 注册 `plugin_example_runtime` | 完成 |

验收（Debug）：

| 模块 | 结果 |
|---|---|
| `plugin_example_runtime` | passed=49 failed=0 |
| `plugin_agent_runtime` / `plugin_host_mount` / `plugin_multi_mount` / `plugin_multi_instance` | 177 / 147 / 47 / 80 全绿 |
| `plugins` / `plugin_cleanup` / `boundaries` | 600 / 99 / 19 全绿 |

示例插件演示的完整路径（写聊天平台机器人插件时照拄）:

| 步 | 入口 | 说明 |
|---|---|---|
| 1 | `mountRoot(ctx, {.name, .config, .mountPlugins})` | 只能放 `start`；返回即拿到 id（init 异步） |
| 2 | `openChannel(ctx, {.rootId, .sessionId}, 回调)` | 根未就绪时自动排队；就绪后收到 `READY` |
| 3 | `addTrigger(ctx, {.topic, .channelId, .policy=COALESCE})` | 连发合并，避免一群刷屏把上下文打满 |
| 4 | `addRuntimeTimer(ctx, {.channelId, .inputJson, .firstDelayMs})` | 可选；到点投一条输入 |
| 5 | 回调里处理 `READY`/`TURN_END`/`INTERRUPT`/`CLOSED` | 文本是借用视图，要留就自己拷 |
| 6 | `stop` 里按 定时器 → 触发器 → 通道 → 根 顺序收尾 | 入口幂等；主程序也会按归属整批回收 |

## 实施注意

- 插件实例永远只有一份（`create`/`start` 各一次）；根只是"再挂载一次"，`mountRoot` 必须强注入宿主 `pluginHost_`。
- 建根只能在 `start`；`mount` 回调期间调 `mount_root` → `ERR_STATE`；`include_creator` 默认 false。
- 入口函数体不能 `co_await`（`ioCallSyncKeep` 在 io 线程直接执行），建/停一律"发起协程后返回"，结果走事件。
- `(存储根, sessionId)` 独占是进程级：占用表挂 `AgentHost`，不能按插件实例建；内存会话按根 id 区分（`RuntimeRoot::sessionKey`）。
- 通道 delta 重放缓冲设 0（`deltaBufferCap`），transport 队列容量保持默认；正文回调按 60ms 合批。
- FFI 侧保持"不让主程序代劳加载插件"的现有行为（`loadPluginsInHost=false`），避免启动顺序回归。
- 新建根的 `ioCtx` 必须与容器一致（`AgentHost::createAgent` 已统一），否则根的 IO 线程无人 `run()`，通道永远等不到 `READY`。

## 阶段六实施中补充的注意事项

- **SDK 但利入口是同步的**：`mountRoot`/`openChannel`/`closeChannel` 都是“发起后返回”，
  插件不能在这个入口上等结果；要“确实就绪了”就靠 `READY` 事件。
- **通道回调与关通道的重入**：插件常会在 `CLOSED` 回调里释放自己的状态，而在
  其它事件回调里也可能主动关通道；SDK 的转接对象在堆上，且回调前先拷一份
  `shared_ptr`，所以这种重入是安全的。
- **`host->alloc` 出来的字符串必须由插件释放**：返回 id / JSON 文本的入口（`mount_root` /
  `get_channel_state` / `get_context_messages` / `list_runtime`）都经 `PluginString::free`；
  SDK 已封装，直接调 C ABI 时别忘了。
- **通道数量级按“一个根 64 通道”规划**：QQ/微信群共用一个根，人设在根配置里；
  真需要“一群一套人设”时才拆成多个根（每根一次 init，启动与内存线性增长）。
- **示例插件的定时器默认关闭**：它到点会跑一轮（产生一次模型调用），默认打开会让
  只装了示例插件的进程莫名其妙开始花钱。
