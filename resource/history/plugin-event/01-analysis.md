# 现状调研与方案对比

> 关联: [index.md](index.md) · [02-runtime-design.md](02-runtime-design.md) · [03-event-driven.md](03-event-driven.md) · [04-prompt-and-impl.md](04-prompt-and-impl.md)

## 1. 插件框架现状

### 1.1 边界与分层

- 跨边界只有纯 C：`PluginxxStringView`（只读借用）、`PluginxxString`（host 分配）、函数指针、`int32_t` 状态码；结构体入参一律 `const Struct*`，出参 `Struct*` + 返回码。
- 能力按 COM 风格查表：核心 vtable 只有 `alloc/free/query_interface`，其余按 IID 查独立表。每张表四件套齐全（IID、`struct_size`、`version/_VERSION`、`_MIN_VERSION/_BASELINE_SIZE`）。
- 新增表要动五处：`plugin_api.h`（表定义）、`plugin_iface_meta.h`（`IfaceMeta` 特化 + 静态断言）、`plugin_manager_vtable.cpp`（实现 + `query_interface` 分支）、`plugin_kit.h`（SDK）、`boundaries` 测试（成员顺序台账）。

### 1.2 agent 侧现有能力（上一篇之后的 11 张业务表）

| IID | 能力 | 与本次需求的关系 |
|---|---|---|
| `agentxx.agent.tools` | 注册/注销工具、调用其它工具 | 无关（插件是工具提供方） |
| `agentxx.agent.permission` | 工具权限声明、批量路径查询 | 直接复用 |
| `agentxx.agent.hooks` | 7 个中间件钩子点 + 有序登记 | 不够：只覆盖"当前 agent"的轮次，拿不到"我另开的会话" |
| `agentxx.agent.session` | 会话 share_store 读写、消息提示 | 按 `session_id` 操作，作用域是当前 agent；会话通道需要自己的读写入口 |
| `agentxx.agent.context` | 读当前 agent 某会话的 LLM 上下文 | 同上 |
| `agentxx.agent.model` | 读主模型配置 | 无切换；切换走新表 |
| `agentxx.agent.prompt` | 读写全局提示词 | 只有全局 get/set，**没有按会话的来源登记** |
| `agentxx.agent.resources` | 贡献 Skill/Memory/MCP | 无关 |
| `agentxx.agent.graph` | 注册节点类型、读写执行图定义 | 本期不用 |
| `agentxx.agent.feature` | 功能点（点/实现/调用） | 无关 |
| `agentxx.agent.mount` | 查当前挂了哪些插件（`list`/`get`） | 无关（但可用于校验根到底挂了什么） |

结论：现有表都是"操作**已经存在**的那个 agent"，没有"驱动一个新会话"的能力，也没有"按会话替换提示词"的能力。

### 1.3 SDK 与线程约定

- `plugin_kit.h` 的 `PluginBase` 持有 `AgentIfaces`（一次查询、缓存到实例），便捷函数是头文件内联的 Shim + Job，`user_data` 恢复实例上下文。
- vtable 入口统一 `enterHost()` → `PluginInstance` + `PluginManager`，再 `ioCallSyncKeep` 投递到 agent IO 线程。
- `ioCallSyncKeep` 在"调用方已经在 io 线程"时**直接执行**（`pluginxx/host/abi_util.h`），所以在回调里调用表入口不会死锁。真正的限制是"入口函数体不能 `co_await`"。

### 1.4 注册可逆（上一篇之后的样子）

上一篇把生命周期拆成两层，本表新增的东西必须想清楚属于哪一层：

| 层 | 内容 | 撤销时机 |
|---|---|---|
| **挂载级**（每 agent 一份，`PluginMount::Entry`） | 工具 / 权限声明 / 钩子 / 提示词贡献 / 功能点 / 图类型 / 图定义 / 资源 / agent 总线事件 | agent 卸载或插件禁用时按固定顺序撤销（上一篇 `02` §6） |
| **实例级**（进程一份，`PluginInstance`） | 能力、宿主事件订阅、以及本表的根 / 通道 / 触发器 | `stop` 与禁用时撤销 |

所以本表新增：根与通道/触发器是**实例级**（在 `start` 里创建，虽然每个通道绑定在某个根上）；
若阶段五做了会话级提示词来源，它是**挂载级**（per-agent，跟工具同一套撤销）。

注册接口只能在 `mount` 里调用（上一篇）；在 `start` 里调 agent 级接口会返回 `ERR_NO_AGENT`。
这条也适用于本表的通道事件：**不要在 `start` 里注册 agent 级实体**。

## 2. 三方启动路径对比（"是否已经统一"的答案）

### 2.1 现状

| 环节 | CLI 本地直连（`mode_runners.cpp`） | FFI（`ffi_runtime.cpp`） | 插件（本次要做的） |
|---|---|---|---|
| 会话驱动循环 | `SessionServerAgentIO::run()` | 同 | 同 |
| 端点/传输契约 | `AgentIOBase` + `ChannelAgentIOTransport` | 同 | 同 |
| 中断流程 | `WireInterruptRequest/Response` | 同 | 同 |
| 事件模型 | `WireDelta/WireTurnResult/...` | 同 | 同 |
| **启动壳**（建宿主、加载插件、定启动顺序、init+挂载、挂根、hello、就绪通知） | 自己写一遍 | 自己写一遍 | 原本要写第三遍 → **本次三处都用 `AgentSessionRuntime`** |
| **控制命令**（input/cancel/model/interrupt） | `AgentIOBase` 方法 + TUI 端点 | FFI 自己解析 JSON 再转 wire | 原本要第三份 → **本次统一到 runtime + `parseUserInputJson`** |
| 事件交付（兜底到宿主） | `TUIClientAgentIO` / `StdIOClientAgentIO` | `FfiClientAgentIO` | `PluginSessionAgentIO`（继承 `EmbeddedClientAgentIO`） |

**答案：核心（驱动循环、端点契约、中断、事件模型）已经统一；启动壳与控制命令解析是三处重复的来源，本次统一到 `AgentSessionRuntime`。**

### 2.2 三处启动壳的共同步骤

对照 `mode_runners.cpp::setupLocalUnifiedDirect` 与 `FfiAgentRuntime::start`，步骤完全同构：

```
1. ChannelAgentIOTransport::makePair(clientEx, agentEx)
2. clientIO->setTransport(clientTrans)
3. SessionServerAgentIO(agentEx, agent, cfg) + setTransport(serverTrans)
4. co_spawn(serverIO->runTransportLoop())
5. co_spawn(clientIO->runTransportLoop())
6. co_spawn( AgentHost::create(...) → host->loadPluginsAsync()
             → agent->init()（内部挂载插件） → host->attachRoot(agent)
             → clientIO->requestAppendComponentInfo(sessionId)
             → clientIO->onServerReady()
             → serverIO->run() )
7. clientIO->sendToPeer(WireHello{...})
```

差异只有四点，且都能参数化：

| 差异 | CLI | FFI | 插件 |
|---|---|---|---|
| client 端点类型 | TUI / StdIO | FFI 端点（独立线程） | 插件端点（同 io 线程） |
| `initialSyncTailCount` | TUI 100，stdio 0 | 0 | 0 |
| `initNotifier` 去向 | 客户端进度条 | 日志环形缓冲 | 不需要 |
| 是否已有 agent（已 init） | 否 | 否 | **是**（挂载根在 mount_root 时已 init） |

结论：抽一个 `AgentSessionRuntime`，把 1~7 收进去，四点差异做成参数，重复就没有了。对插件通道：agent 已在 `mount_root` 时 init 过，所以 `skipInit=true`，并传入 `mount_root` 所在的那个宿主（`Config::host`）。

## 3. FFI 现状（要被复用的东西）

### 3.1 对外能力

`agentxx/ffi_api.h` 共 22 个导出函数与 13 种事件：

| 分组 | 函数 | 插件侧是否需要 |
|---|---|---|
| 生命周期 | `create/start/stop/destroy` | 部分（建/停会话通道） |
| 会话交互 | `send_input/cancel/select_model/switch_session/set_language/get_language` | 需要（switch_session 由"通道"替代） |
| 模型管理 | `add_model/remove_model/list_models/get_model_info` | 本期不做增删 |
| 上下文与历史 | `get_context_messages/list_sessions` | 需要 |
| 中断 | `interrupt_respond` + `EVT_INTERRUPT_REQ/EXPIRED` | 需要 |
| 事件 | `EVT_READY/SYNC/DELTA/TURN_END/CONTEXT_STATS/...` | 需要子集 |
| 宿主工具 | `tool_register/unregister/respond` | 不需要（插件自身是工具宿主） |
| 其它 | `send_wire/set_delta_batch/drain_logs/event_queue_*` | 不需要（插件有 log/events 表） |

### 3.2 内部实现（复用点）

```
FfiAgentRuntime
  ├─ 创建 CodeAgent + 配置                 ← 插件场景：mount_root 按自己的配置新建
  ├─ agent->init()                         ← 插件场景：mount_root 时已完成
  ├─ AgentHost::create + loadPluginsAsync + attachRoot ← 每进程一次（CLI/FFI/插件都走宿主）
  ├─ ChannelAgentIOTransport::makePair     ← 提炼进 AgentSessionRuntime
  ├─ SessionServerAgentIO                  ← 提炼进 AgentSessionRuntime
  ├─ FfiClientAgentIO                      ← 泛化为 EmbeddedClientAgentIO（事件汇）
  └─ 独立 Client-IO 线程                    ← 只有 FFI 需要（外部回调不可信）
```

三处实现细节必须沿用：

1. `SessionServerAgentIO::run()` 已含队列、暂停、`input_ack`、中断，不要另写。
2. 中断应答是"client 端点挂起等 → 宿主给答案 → 回 `WireInterruptResponse`"，未应答表在 client 端点。
3. 关闭顺序：**先关 client 侧（失败挂起中断 + close transport）→ `serverIO->stop()` → 等 `run()` 退出 → 释放对象**。

### 3.3 FFI 为什么不能直接给插件用

| 原因 | 说明 |
|---|---|
| 接口形态 | 一个句柄一个会话；插件需要 N 个通道 |
| 线程 | FFI 为不可信外部调用方准备独立 Client-IO 线程；插件多一个线程是浪费 |
| 配置 | FFI 自建 `CodeAgent`；插件经 `mount_root` 自建（否则无法自定义提示词/工具，且会与内置会话共用配置） |
| 归属 | FFI 句柄不参与插件 owner 记账 |

结论：**提炼实现，不暴露 FFI 接口**。

## 4. AgentHost 为什么是单根，多根是否合适

### 4.1 现状

| 位置 | 现状 |
|---|---|
| `rootAgent_` | 单个成员；`attachRoot` 已有根时只记警告并返回 |
| `subagentServerId_` | 单个，注册在根 agent 的 bus 上 |
| `spawnOneTask` | 用 `rootAgent_` 取父配置、工具继承、用量记账、share_store 桥接 |
| `registry` | 根节点固定 `agentId="root"`、`parentAgentId=""`、`depth=0` |
| `attachRootAgentHost` | 注释明确"仅支持单根（进程级 agent 节点容器）" |

### 4.2 单根的原因（回看设计意图）

- `AgentHost` 的定位是"**进程级** agent 节点容器"：一个主 agent + 它的子代理树。`hostBus`、`maxDepth`、`maxConcurrentSubagents` 都是"整棵树"的预算。
- 子代理的默认父上下文、用量记账、`taskScope` 都需要一个确定的"主"。
- 现有使用方都是"一个进程一个主体"：CLI 一个，FFI 一个句柄一个。

### 4.3 多根是否合适

先分清两种回退力度：

| 维度 | 多根（新 `AgentConfig`） | 仅多会话（同配置） |
|---|---|---|
| 系统提示词 | 整段自定义 | 只能用追加/后续替换段 |
| 插件挂载策略 | 自定义（只挂需要的工具插件） | 与宿主完全一致 |
| 工具白名单 / 过滤 | 自定义 | 无法按会话设置 |
| 中间件栈 / 执行图 | 独立 | 共享 |
| 上下文 / 模型 / 工作目录 / 权限边界 | 独立 | 独立 |
| 资源 | 每根一次完整 init：AgentContext + 工具 + **插件挂载**（主程序共用的插件实例，不重复 dlopen/create/start）+ 图 | 几乎为零 |
| 预算语义 | 需明确（建议全局共享） | 沿用现有 |
| 递归风险 | 插件可能在根里再建根 | 不存在 |

结论：**需要多根，但它是"配置隔离"的手段，不是"群隔离"的手段**。

- 插件要换系统提示词、只保留只读工具、不挂 coding 相关插件——这些全是 `AgentConfig` 级，会话级表达不了，必须有自己的根。
- 但同一个根可以承载很多会话：QQ/微信群数量多，共用一套配置（一个根）；只有"客服"与"运维"这种不同人设/工具集才各挂一个根。
- “每群一个根”仍然不推荐：群数线性放大启动与内存。

因此本次：

- **支持多根**：`AgentHost` 从单根改为"内置根 + 挂载根"，插件经 `mount_root` 创建带自己配置的根。
- **插件不创建 `AgentHost`，也不新建插件宿主**：统一挂到现有容器（共享 ioCtx / threadPool / hostBus / 子代理预算 / 已加载的插件实例）；根构造时必须继承宿主的 `ctx->pluginHost`（否则 `BaseAgent::init` 会走 `createSelfHosted` 再起一份插件宿主）。
- **根的工具来自插件挂载**：上一篇之后插件 `create/start` 各只一次，根只是**再挂载一次**（`mount`）；根默认不挂任何插件（只有内置工具），要文件/命令工具就在挂根的 spec 里显式给 `listed` 列表。
- **递归切断换成三层**（"根里没有 runtime 表"的做法已失效——插件实例只有一份）：
  ① 建根只能在 `start`（进程级）；
  ② 宿主在驱动某个 `mount` 期间调用 `mount_root` → 直接返回 `ERR_STATE`；
  ③ 创建者插件默认不挂到自己的根上（`include_creator = false`）。

## 5. BaseAgent 的多会话能力（可行性依据）

已按 `sessionId` 隔离：

- 上下文消息、展示历史、链式哈希、`deltaSeq`
- 当前模型、语言
- 取消令牌、活动状态、上下文统计与累计用量
- 工作目录覆写、worktree 绑定、权限隔离边界
- share_store
- **perSession 提示词来源**（`PromptSourceRegistry`，`perSession=true` 按会话各存一份）
- 会话级事件总线（`session->bus` 在 `runTurnAsync` 里按会话创建，`io->registerOnBus` 每轮注册）

仍然 per-agent：工具集、中间件栈、执行图、基础 systemPrompt。

这四项正是“需要多根”的依据：

- 工具集 / 插件挂载：`AgentConfig::toolWhitelist` + `enableToolFiltering` + `pluginMountPolicy`，会话级无法表达。
- 基础 systemPrompt：`AgentConfig::prompt.systemPrompt` 是 agent 级；会话级只能追加段落（或后续新增“替换段”，但那仍共用同一套工具与插件）。
- 中间件栈 / 执行图：绑定 `AgentContext` / 引擎。

所以：**配置类差异 → 新根；对话类差异 → 新会话**。

## 6. 关键约束

| 编号 | 约束 | 依据 | 应对 |
|---|---|---|---|
| C1 | 插件实例全程只有一份（`create`/`start` 各一次），根只是"再挂载一次"（`mount`） | 上一篇：加载在 `AgentHost`，每 agent 挂载 | ① 根构造时强注入 `ctx->pluginHost`（禁止走自建宿主分支）；② 根默认 `pluginMountPolicy = None`，要工具就显式给列表；③ 建根只在 `start` |
| C2 | 会话可变状态只能在其 agent 的 io 线程访问 | `Session::assertIoThread()` | 根与通道的操作全部在宿主 io 线程执行；插件线程只投递 |
| C3 | 同一 EventBus topic 只能一种载荷类型 | `EventBus::get<T>()` 用 `typeid` 校验 | 触发器只用 `std::string` 流（`plugin.*`） |
| C4 | 同一 `(存储根, sessionId)` 只能有一个写入者 | `SessionWriterLease` 进程内可重入；两处打开同一目录会互相覆盖 | 占用表挂到进程唯一的 `AgentHost`，key = `(规范化存储根, sessionId)`（与上一篇 §7.3 同一条规则） |
| C5 | 入口函数体不能 `co_await` | `ioCallSyncKeep` 在 io 线程直接执行；入口里等协程会卡死 | 建/停一律"发起协程后返回"，结果走事件 |
| C6 | `SessionServerAgentIO` 的 delta 重放缓冲按条数分配 | `Config::deltaBufferCap` 默认 4096 | 插件通道设 0；transport 队列容量（`makePair` 的 cap）保持默认 |
| C7 | 工具只能在 `mount` 里注册（`start` 里调 agent 级接口返回 `ERR_NO_AGENT`） | 上一篇 §5.1 | 插件必须把"建根 + 连协议"（`start`）与"注册工具"（`mount`）拆成两半；两者不要混写 |
| C8 | 在 `mount` 回调里建根会递归（新根又要挂载一次） | 每个 agent 入挂都会调一次 `mount` | 宿主在驱动 `mount` 期间记 `mounting_`，`mount_root` 直接返回 `ERR_STATE`；另有两道结构性防护（C1 ③） |

## 7. 实现路径对比

| 方案 | 做法 | 优点 | 缺点 | 结论 |
|---|---|---|---|---|
| A. 插件直接调 `BaseAgent::runTurnAsync` | 插件自己 `co_spawn`，手写事件收集 | 代码少 | 要自己实现队列/中断/取消/统计；vtable 不能 co_await；每个插件重复 | 否 |
| B. 插件链接 libagentxx 的 C++ 头，自己构造 `CodeAgent` + `SessionServerAgentIO` | 直接写内部类型 | 灵活 | 破坏 C ABI；插件与主程序 ABI 强耦合；无法 owner 记账；重复加载插件 | 否 |
| C. 插件动态加载 `libagentxx_shared` 走 FFI | 复用现成 API | 零框架改动 | 一句柄一会话；独立线程；配置不能复用宿主；与主程序争用会话库 | 否 |
| D. 多根 + `AgentSessionRuntime` + 新表 `agentxx.agent.runtime` | `AgentHost` 支持多根；统一启动壳；插件挂自己的根并开通道 | 配置类差异（提示词/插件/工具）与会话类差异（对话）各归其位；复用已有实现；owner 记账；事件驱动可扩展 | 需要 AgentHost 多根改造 + 提炼一层 + 新增一张表 | **采用** |

## 8. "事件驱动"还是"直接调用 input"

`send_input` 与"事件触发"最终都是往会话投递一条输入，区别只在**谁决定什么时候投**。

| 维度 | 插件内直接 input | 框架内事件驱动 |
|---|---|---|
| 投递时机 | 插件自己判断 | 框架按策略判断（排队/合并/丢弃/防抖） |
| 重复实现 | 每个插件写一遍合并与排队 | 一份实现，插件只描述规则 |
| 扩展 | 加"定时/外部信号"要改插件 | 新增事件源即可 |
| 与已有队列的关系 | 绕过 `input_ack`/队列状态 | 走同一条投递路径 |
| 复杂度 | 插件侧高、框架侧零 | 框架一次性成本，插件侧极低 |

结论：**两条都保留，默认用法是事件驱动**。底层原语 `send_input` 必须有（触发器就是用它实现的，调试也方便）；触发器是策略层。不做"让 `BaseAgent` 支持事件驱动"。

## 9. 依赖上一篇（`plugin-multi-agentnode`）的结论

| # | 依赖 | 用在哪 |
|---|---|---|
| 1 | 进程一份插件宿主：`AgentHost::pluginHost()`；`createSelfHosted` 只用于"没有外部宿主"的单 agent/测试 | `mount_root` 必须把宿主的 `pluginHost` 注入到新根，否则根会自建一份宿主 |
| 2 | `PluginMount` + `mountAgentAsync` + `pluginMountPolicy` / `pluginMounts` | 决定挂载根挂哪些插件 |
| 3 | `PluginxxScopeRef`（agentId + mount_seq）/ SDK `AgentRef` | 插件在根的 `mount` 里注册工具时用；`root_id` 就是该根的 agentId |
| 4 | `ERR_NO_AGENT`（注册类必须在挂载内） | 约束 C7 |
| 5 | 表版本模型：`VERSION` / `MIN_VERSION` / `BASELINE`（IID 不变，只允许表尾追加） | 本表 v1；阶段五 prompt 表 2→3 |
| 6 | `agentxx.agent.mount` 表（`list` / `get`） | 查某个根挂了哪些插件，用于诊断与自查 |
| 7 | 会话占用规则：同一 `(存储根, sessionId)` 互斥 | 约束 C4 |
| 8 | `plugin.mount` 配置段（`all`/`none`/列表 + `args`） | 根的挂载策略复用同一套语义（JSON 形态，见 02 §4.3） |
