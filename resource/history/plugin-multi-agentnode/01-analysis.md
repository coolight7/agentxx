# 01 现状分析与方案取舍

本篇讲"现在是什么样、有哪些历史负担、为什么这么选"，不含最终设计（见 `02`）。

## 1. 现状：一个 agent 一个插件管理器

### 1.1 创建与装载

`BaseAgent::init` 里两步与插件有关（`agent/lib/src/agent/base_agent.cpp`）：

| 步骤 | 行 | 做的事 |
|---|---|---|
| `plugin_system` | 356~380 | 建 `ToolRegistry`；`ctx->pluginManager = make_shared<PluginManager>(ctx)`；`setIoExecutor(当前协程 executor)` |
| `plugins_load` | 521~548 | `co_await pluginManager->loadConfiguredPlugins(ctx->agentConfig->plugins)` |

`plugins_load` 排在 `middleware`（权限中间件）、`tools`、`tool_whitelist`、`summarization`、
`graph_definition` 之后，`engine_build` 之前。位置不能动的原因：

1. 权限中间件已存在，插件才能声明工具权限；
2. 图注册表已存在，插件才能注册节点类型；
3. 执行图还没构建，插件改的图定义才能影响本次启动的引擎。

关闭路径三处，行为不同：

| 位置 | 行为 |
|---|---|
| `BaseAgent::shutdownAsync` 阶段③（`base_agent.cpp:1757+`） | 借管理器记录的 executor 执行 `shutdownAsync`：`stop` + `destroy` + `dlclose` |
| `AgentContext::~AgentContext`（`context.cpp:136`） | 同步 `shutdownAll()`；来不及 stop 的实例保留 `CloseFailed` 并告警 |
| `AgentHost::destroyAgent`（`agent_host.cpp:1051`） | 只查 `hasPendingClose()` 并告警 |

### 1.2 管理器到 agent 的耦合点

管理器只持一个 `weak_ptr<AgentContext>`，但在 16 个位置把它当成"当前唯一 agent"：

| # | 位置 | 用到的 per-agent 对象 |
|---|---|---|
| 1 | 构造函数 `plugin_manager_lifecycle.cpp:78` | `ctx->toolRegistry` |
| 2 | `registerTool` | 注册表 + `PluginTool(agentContext_)` |
| 3 | `permissionMiddleware()` | `ctx->middlewareHandleContext->handles` |
| 4 | `registerGraphNodeType` | `ctx->graphRegistry` |
| 5 | `setGraphJson` / `getGraphJson` / `releaseGraphDefinitionSlot` | `ctx->graphDefinitionJson` |
| 6 | 功能点登记 / 清单 | `ctx->features` |
| 7 | `registrationInventory` | `ctx->features`、`ctx->resourceApplier` |
| 8 | 资源注册 `vtable.cpp:946~1101` | `ctx->resourceApplier` |
| 9 | prompt 读写与合成 | `ctx->agentConfig->prompt` |
| 10 | `eventSource()` `domain_hooks.cpp:97` | `ctx->bus` |
| 11 | `postToWorkerThread` | `ctx->threadPool` |
| 12 | `configJson` / `language` / `sessionWorkDir` / 模型配置 | `ctx->agentConfig`、`ctx->sessions` |
| 13 | 会话上下文读写 | `ctx->sessions` |
| 14 | share_store / 提示消息 | `ctx->middlewareHandleContext`、会话 io |
| 15 | 禁用/卸载钩子 `lifecycle.cpp:144~211` | `ctx->features`、`ctx->resourceApplier` |
| 16 | `hookDispatch_` | `ctx->middlewareHandleContext`（中间件链） |

结论：注册动作**必须**落到某个具体 agent 的对象上。所以"一份管理器共享给多个 agent"会立刻出错；
必须把"加载"与"注册"拆开。

### 1.3 十张业务表的 agent 依赖

| 表 | 主要依赖 | 能否脱离 agent |
|---|---|---|
| `tools` | 工具注册表 + 绑定的 AgentContext | 不能 |
| `permission` | 权限中间件 | 不能 |
| `hooks` | 中间件链 + 派发器 | 不能 |
| `session` | 会话 / 共享存储 | 不能 |
| `context` | 会话 | 不能 |
| `model` | `agentConfig->model` | 不能 |
| `prompt` | `agentConfig->prompt` | 不能 |
| `resources` | 资源应用器 | 不能 |
| `graph` | 图注册表 + 图定义 | 不能 |
| `feature` | 功能点注册表 | 不能 |
| 十张通用表 | 实例自身 / 宿主 | 多数可以；`events`（agent 总线）、`config.get_session_work_dir`、`cancel.is_cancelled` 需要 agent 引用 |

### 1.4 五条启动路径

| 路径 | 文件 | 现状顺序 |
|---|---|---|
| 进程内直连（TUI/CLI） | `client/src/mode_runners.cpp:232~242` | `init → attachRootAgentHost → run` |
| JSONL | `client/src/io/jsonl/jsonl_mode.cpp:118~124` | `init → attachRootAgentHost` |
| agent server（WS） | `client/main.cpp:1036~1040` | `init → attachRootAgentHost → server->start` |
| FFI | `lib/src/ffi/ffi_runtime.cpp:429~452` | `init → AgentHost::create + attachRoot` |
| 基准/测试 | `benchmark/**`、`test/core/test_agent_host.cpp` | `init`（多数不建宿主） |

共同点：**宿主总在 `init` 之后创建**。所以"让宿主负责加载"必须同时改启动顺序。

### 1.5 多实例三铁律

`plugins.md` §3 要求插件内不得有可变全局/静态缓存，状态放 `plugin_ctx`。
多实例来源是 FFI 多句柄与测试。本次改造**不取消**它：进程里仍可能有多份宿主，
只是**一份宿主内部**从"每 agent 一个实例"变成"一个实例多个挂载"。

## 2. 为什么"每 agent 一个管理器"撑不住

群聊插件若每群一个 agent（`../plugin-event/` 已论证），则：

1. **加载风暴**：N 个群 → N 个 agent → `create/start` 各 N 次，`start` 里建连接、扫目录、起线程全部 ×N。
2. **配置分裂**：同一插件的 `args` 要在 N 份配置里重复写，改一处漏一处。
3. **状态无法共享**：QQ 只允许一条长连接，N 个实例意味着 N 次登录，或被迫使用进程级单例（违反三铁律）。
4. **无法路由**：消息到达时插件需要"该给哪个 agent"，跨实例没有通道。

反方向（一份管理器 + 多 agent）见 §1.2 不可行。只能拆。

## 3. 硬约束

| # | 约束 | 出处 | 含义 |
|---|---|---|---|
| C1 | 实例必须等 inflight 与注册摘净后才能 `dlclose` | `PluginHostLifecycle` | 卸载要走"摘注册 → 等 inflight → dlclose" |
| C2 | 注册类接口只在宿主 io 线程执行 | `ioCallSyncKeep` | 宿主有唯一的 io executor |
| C3 | 工具对象必须绑定目标 agent | `PluginTool(agentContext, ...)` | 工具不能共享，只能每挂载一份；工具对象随身带 agent 引用 |
| C4 | 提示词/权限/图/功能点/资源都是 per-agent | `AgentContext` 字段 | 同名注册要在每个 agent 各做一次 |
| C5 | 事件源是 agent 总线（会话事件也发在这里） | `agentcall.cpp:26` | 事件订阅天然 per-agent |
| C6 | 子代理从不加载插件，其 `ioCtx` 从未 `run()` | `makeSubagentConfig` | 挂载默认不适用子代理；跨 executor 挂载要拒绝 |
| C7 | 插件 `plugin_ctx` 是单实例状态，多挂载共享 | 现状 | 插件必须按 agent 分键 |
| C8 | 同进程允许多份宿主（FFI 多句柄、测试） | `plugins.md` §3 | 宿主内不能有可变全局 |
| C9 | 会话 id 在**每个 agent 内**唯一，跨 agent 可重名 | `SessionsManager` 按 `(存储根, id)` 定位 | 所以 agent 级接口必须带 agentId；同存储根被两个 agent 打开要拒绝（`02` §7） |
| C10 | 每个插件回调都必须能确定"属于哪个 agent" | — | 隐式状态盖不住插件自建线程/定时器/任务完成回调 → agent 级接口显式带 `PluginxxScopeRef`（`02` §3） |

## 4. 兼容负担：哪些留、哪些清

这里要把两件事分清楚：

- **ABI 演进机制**（版本号、基线、`struct_size`、`ifaceHas`、`specHas`）——它们保证"**以后**改接口
  不必让所有插件重编"，本次**全部保留**；
- **历史包袱**（为旧插件/旧写法存在的代码路径与文档）——本次**全部清除**。

用户的要求是"抛弃历史版本兼容性，跟未来版本保持兼容"，对应的就是这两句话。

### 4.1 保留：ABI 演进机制（一处都不删）

| # | 机制 | 位置 | 作用 |
|---|---|---|---|
| 1 | 全局 `PLUGINXX_API_VERSION` / `PLUGINXX_MIN_API_VERSION` | `pluginxx/api/abi.h` | 插件声明 API 版本；主程序按 `>= MIN` 判定 |
| 2 | 每表 `version` / `_VERSION` / `_MIN_VERSION` | `pluginxx/api/tables.h`、`plugin_api.h` | 各表独立演进，互不牵连 |
| 3 | 表基线 `_BASELINE_SIZE` + `struct_size` 字段 | 同上 | 追加成员后仍能读旧长度的表 |
| 4 | `IfaceMeta<T>` 特化 | `pluginxx/api/iface_meta.h`、`plugin_iface_meta.h` | 把"最低版本 + 基线"集中一处 |
| 5 | `validateInterface`（IID + `version >= MIN` + `size >= BASELINE`） | `pluginxx/kit/kit.h` | 整表可用性判定 |
| 6 | `ifaceHas<&T::m>` / `ifaceHasVersion` | 同上 | 追加成员的可用性判定 |
| 7 | 跨边界结构体 `struct_size` + `specSizeOk` / `specHas` | `plugin_spec.h` | 结构体尾部追加字段的兼容 |
| 8 | `boundaries` 的四件套校验与成员顺序台账 | `test/core/test_boundaries.cpp` | 防泄漏/防漏改 |

### 4.2 本次换代必须做的两件事

| 动作 | 具体 | 作用 |
|---|---|---|
| 提升下限 | `PLUGINXX_API_VERSION` 1→2、`PLUGINXX_MIN_API_VERSION` 1→2 | 新主程序明确拒绝 v1 插件（不进入"半可用"状态） |
| 升表版本 | 改动过的 14 张表：`VERSION` 1→2、`MIN_VERSION` 1→2、`BASELINE` = 新 `sizeof`（**IID 不变**） | 新插件在旧主程序上取这几张表得到 `NULL` → 走"主程序不支持"分支 |

两条各管一个方向：`MIN_API_VERSION` 管"旧插件 + 新主程序"，表的 `MIN_VERSION`/`BASELINE`
管"新插件 + 旧主程序"。本次不换 IID 的理由（每张表 `sizeof` 都变了 + 上面两条兜底，
不存在能落到"按旧布局读新表"的组合）见 `02` §10.3。

### 4.3 清除：与 ABI 机制无关的历史包袱

| # | 位置 | 现状 | 处置 |
|---|---|---|---|
| 1 | `plugin_manager_lifecycle.cpp` | `PluginInstance::PromptBackup` + `restorePromptBackup` + `promptKeys_` 的 `base/applied`，与贡献模型重复 | **删除旧"备份写回"模型**，只留贡献模型 |
| 2 | `plugin_manager_hooks.cpp` | 基础登记（覆盖式、按点撤销）与扩展登记（句柄式）双入口 + `base` 标志 | **合并为一种**（句柄式） |
| 3 | `plugin_manager_vtable.cpp` | `tools` / `feature` 各有一个 `op_cancel`，与 `pluginxx.cancel` 语义重复 | 收拢到 `pluginxx.cancel`（表尾追加） |
| 4 | `agent_host.cpp:184` | `attachRootAgentHost(agent)`：先 `init` 后建宿主的便捷入口 | **删除**（启动顺序改为先建宿主） |
| 5 | （原计划）宿主收养自建宿主 | 为兼容"先 init 后建宿主" | **不实现** |
| 6 | `plugin_kit.h:3548` | `AGENTXX_PLUGIN_AGENT_EXPORT` 与 `_LIFECYCLE_EXPORT` 两套宏 | 合并为一套，固定 8 参数（含 `mount`/`unmount`，可为 `nullptr`） |
| 7 | `config_loader.cpp:1326` | `warnLegacySectionKeys`（`models`/`plugins`/`use_model` 旧键告警） | **删除** |
| 8 | `config_loader.cpp` | 纯列表写法（`plugins: [...]`）的废弃告警 | **删除** |
| 9 | `AgentConfig::plugins` | 名字是复数、语义已变为"加载表" | 改名 `pluginLoadList` |
| 10 | 计划中的"声明层" | `start` 里注册 → 声明 → 每挂载重放 | **不做**（注册只能在 `mount`） |
| 11 | 若干注释 | "兼容旧契约""不再区分内置/外置""重置前"等历史解释 | 改写为当前事实 |

### 4.4 汇总

| 项 | 数量 |
|---|---|
| 删除代码（估） | 约 600~800 行（PromptBackup、attachRoot 与收养、legacy 告警、钩子双分支、重复 `op_cancel`、双宏） |
| 新增代码（估） | 约 800~1000 行（宿主/挂载、新表、SDK、示例） |
| 净变化 | 大致持平，但少了一层"两阶段注册"和一类历史分支 |
| 保留 | §4.1 的 8 类演进机制，一个不删 |

> 本表的"删除"不含任何版本/基线代码。凡是为了**未来**兼容而存在的判定，全部保留。
## 5. 方案对比

| 方案 | 做法 | dlopen | `create/start` | 连接数 | 状态隔离 | 改动面 |
|---|---|---|---|---|---|---|
| A. 库缓存 + 每 agent 一份实例 | 缓存句柄与清单，实例仍按 agent 建 | 1 | N | N | 天然 | 小 |
| **B. 宿主加载 + 每 agent 挂载（选定）** | 一份实例，N 个挂载 | 1 | 1 | N（资源仍 per-agent） | 插件按挂载隔离 | 大 |
| C. 宿主加载 + 每 agent 一份实例 | 句柄共享、实例 N 份 | 1 | N | N | 天然 | 中 |
| D. 插件自带 agent | 插件自己建 root agent | 1 | 1 | 1 | 插件自理 | 中（另一件事） |

- **A 否**：`start` 里的进程级工作仍做 N 次，配置仍分裂，无法跨 agent 共享状态。
- **C 否**：`create/start` 仍 N 次，跨实例通信仍要外部通道。
- **D 是另一件事**：它解决"插件要自己的提示词/工具集"（`../plugin-event/`），
  其内部 agent 仍需要"插件被挂载到多个 agent"的能力，所以 B 是 D 的前提。
- **B 选定**：满足全部七条要求。

## 6. 关键论证

### 6.1 为什么删掉"声明层"（上一轮设计保留的那一层）

上一轮为了"老插件零改动"，让 `start` 里的注册变成"声明"，再由宿主在每个 agent 上重放。
本次不保留兼容后，这一层只剩坏处：

- 同一件事有两种写法（`start` 里声明 / `mount` 里直接注册），作者要猜"我该写哪"；
- 声明要深拷贝所有字符串（`PluginxxStringView` 指向插件内存），多一份内存与代码；
- 注册的返回值失去意义（声明阶段无法判断目标 agent 上的冲突）；
- 需求第 5 条明确说"prompt、配置等 agent 相关内容应当**在挂载上做**"。

所以改为：**`start` 只能做进程级；agent 侧注册一律在 `mount` 里做；在 `start` 里注册直接报错**。
代价是内置插件都要改（机械改动），收益是一条规则、零歧义。

### 6.2 为什么要改成显式的 agent 引用

上一稿靠隐式"当前挂载"（宿主在回调前后压栈），理由是不想动 60+ 个函数指针签名。
本次重新评估后改为**显式**，因为隐式状态在多 agent 下必然出错：

- 插件自建线程（JS 引擎线程、网络线程）、定时器、`tasks.run` 完成回调、offload 回调里
  根本没有"当前挂载"，只能靠"最近一次回调用过的"猜，或者按 sessionId 猜；
- 猜错就是串数据：写错 share store、读错上下文、`is_cancelled` 误判别的轮次、
  权限判定用错工作目录；
- 将来给子 agent 挂载时，隐式栈还要表达嵌套，而子 agent 的调用可能发生在父的协程里；
- 出问题时日志只有一处，看不出是哪个 agent。

显式方案的成本是一次性的：每个 agent 级接口多一个 `const PluginxxScopeRef*`，
插件在 `mount` 里拿到的引用存进 per-agent 状态，之后任何线程都用同一份（`02` §3）。
规则也跟着简化成一句：**带引用 → 用；不带或带错 → 报错**（没有路由、没有兜底）。

### 6.3 为什么保留演进机制、只提升下限

本次是破坏性变更，但“破坏”只针对**历史**：

- 旧插件（v1）被 `MIN_API_VERSION` 挡住，错误信息直接告诉作者重编；
- 布局变了的表把 `VERSION`/`MIN_VERSION` 提到 2，新插件放到旧主程序上取表得到 `NULL`，
  不会按旧布局读新表；
- 而 `version` / `struct_size` / `ifaceHas` / `specHas` 全部保留。

因为下一次改动（比如给 `agentxx.agent.mount` 追加一个成员）不应该再让全部插件重编：
按现有规则，表尾追加成员 → `BASELINE` 不动、`VERSION` +1、读取方 `ifaceHas` 判定，
老插件继续可用。这就是“跟未来版本保持兼容”。

如果删掉这套机制，以后每次加一个成员都得把整个生态重编一次——显然不是想要的结果。

## 7. 顺带要修的隐患

| # | 隐患 | 现象 | 修法 |
|---|---|---|---|
| H1 | 插件工具与内置工具重名不会被拒绝 | `ToolRegistry::setStaticToolNames` 在 `engine_build`（`base_agent.cpp:642`）才调用，插件工具在 `plugins_load`（`521`）已注册，此时静态名单为空 → 同名工具同时出现在请求里 | 挂载时按 `ctx->toolNames` 再查一次；`setStaticToolNames` 对已注册项做冲突清理并告警 |
| H2 | 插件工具不进 `ctx->toolNames`，子代理 `tools:["*"]` 继承不到 | 行为可接受（插件工具依赖插件状态），但缺项不明显 | 保持行为；在 `tools:["*"]` 解析处补注释与日志 |
| H3 | `GraphRegistry` 无删除类型名接口 | 卸载后类型名残留（slot 失活已规避崩溃） | 保持现状；诊断输出"残留类型名"计数 |
